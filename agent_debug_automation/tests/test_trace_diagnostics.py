"""Structured rtl_trace diagnostics must survive the MCP analysis wrappers.

The integration cases compile small DBs with real binaries. Set
RTL_TRACE_DIAGNOSTICS_BIN and RTL_TRACE_LEGACY_BIN to test frozen binaries.
Legacy cases explicitly skip when the supplied old compiler is unavailable.
"""

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from agent_debug_automation import clients, sessions, tools
from agent_debug_automation.models import DEFAULT_RTL_TRACE_BIN, DEFAULT_WAVE_CLI


SOURCE = """module diagnostic_top(
    input logic [3:0][7:0] p,
    input logic [7:0] v,
    output logic y,
    output logic z
);
assign y = p[2][1];
assign z = v[2];
endmodule
"""

WAVEFORM = """$timescale 1ns $end
$scope module diagnostic_top $end
$var wire 32 ! p [31:0] $end
$var wire 8 " v [7:0] $end
$var wire 1 # y $end
$var wire 1 $ z $end
$upscope $end
$enddefinitions $end
#0
b0 !
b0 "
0#
0$
#5
b100 "
1$
#10
"""


class TraceDiagnosticsUnitTests(unittest.TestCase):
    def test_error_and_warning_messages_preserve_structured_diagnostics(self):
        diagnostics = [
            {"severity": "warning", "code": "legacy", "message": "Dimensions unverified"},
            {"severity": "error", "code": "ambiguous", "message": "Specify both axes"},
            {"severity": "error", "code": "invalid", "message": "Invalid coordinate"},
        ]
        session = mock.Mock()
        session.query.return_value = {
            "status": "success",
            "stdout": json.dumps({"diagnostics": diagnostics, "endpoints": []}),
        }
        with mock.patch.object(clients, "_get_rtl_serve_session", return_value=session):
            result = clients._rtl_trace_json("unused.db", "t.p[2]")
        self.assertEqual(result["status"], "error")
        self.assertEqual(result["message"], "Specify both axes; Invalid coordinate")
        self.assertEqual(result["warnings"], ["Dimensions unverified"])
        self.assertEqual(result["diagnostics"], diagnostics)


class TraceDiagnosticsRealBinaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = Path(os.environ.get("RTL_TRACE_DIAGNOSTICS_BIN", str(DEFAULT_RTL_TRACE_BIN)))
        cls.legacy_binary = Path(os.environ.get("RTL_TRACE_LEGACY_BIN", "/tmp/rtl_trace_item5_reference"))
        if not cls.binary.is_file():
            raise unittest.SkipTest(f"real rtl_trace binary unavailable: {cls.binary}")
        cls.directory = tempfile.TemporaryDirectory(prefix="mcp_trace_diagnostics_")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.root = Path(cls.directory.name)
        cls.source = cls.root / "diagnostic_top.sv"
        cls.source.write_text(SOURCE)
        cls.waveform = cls.root / "wave.vcd"
        cls.waveform.write_text(WAVEFORM)
        cls.current_db = cls.root / "current.db"
        cls._compile(cls.binary, cls.current_db)
        cls.legacy_db = cls.root / "legacy.db"
        if cls.legacy_binary.is_file():
            cls._compile(cls.legacy_binary, cls.legacy_db)
            # The refusal case must use a real old-format DB, not a new DB
            # with a manually patched version number.
            version = int.from_bytes(cls.legacy_db.read_bytes()[16:20], "little")
            if not 1 <= version <= 5:
                raise AssertionError(f"legacy compiler wrote DB version {version}")

    @classmethod
    def _compile(cls, binary, db):
        result = subprocess.run(
            [str(binary), "compile", "--db", str(db), "--single-unit", str(cls.source),
             "--top", "diagnostic_top"],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def setUp(self):
        clients._cleanup_runtime_state()
        self.addCleanup(clients._cleanup_runtime_state)
        self.store_patch = mock.patch.object(sessions, "SESSION_STORE_DIR", self.root / "sessions")
        self.active_patch = mock.patch.object(sessions, "ACTIVE_SESSION_FILE", self.root / "active.json")
        self.store_patch.start()
        self.active_patch.start()
        self.addCleanup(self.store_patch.stop)
        self.addCleanup(self.active_patch.stop)

    def _require_legacy(self):
        if not self.legacy_db.is_file():
            self.skipTest(f"real legacy compiler unavailable: {self.legacy_binary}")

    def _check_error(self, db, signal, code):
        # Test immediate traces and traversals of both drivers and loads.
        for mode in ("drivers", "loads"):
            for options in (None, {"cone_level": 2, "depth": 3}):
                with self.subTest(mode=mode, options=options):
                    result = clients._rtl_trace_json(
                        str(db), signal, mode=mode, trace_options=options,
                        rtl_trace_bin=str(self.binary),
                    )
                    self.assertEqual(result["status"], "error", result)
                    errors = [d for d in result["diagnostics"] if d["severity"] == "error"]
                    self.assertIn(code, [d["code"] for d in errors])
                    self.assertIn(errors[-1]["message"], result["message"])

        # Invalid coordinates must stop before a waveform cone is collected.
        for handler in (tools.trace_with_snapshot, tools.rank_cone_by_time, tools.explain_signal_at_time):
            with self.subTest(handler=handler.__name__), mock.patch.object(
                tools, "_build_signal_summaries", side_effect=AssertionError("waveform work on trace error")
            ):
                result = handler(
                    db_path=str(db), waveform_path=str(self.waveform), signal=signal,
                    time=5, mode="loads", rtl_trace_bin=str(self.binary),
                    trace_options={"cone_level": 2},
                )
                self.assertEqual(result["status"], "error", result)
                self.assertIn(code, [d["code"] for d in result["diagnostics"]])

    def test_ambiguous_packed_select_is_error_for_trace_cone_and_analysis(self):
        self._check_error(self.current_db, "diagnostic_top.p[2]", "ambiguous_single_axis")

    def test_real_legacy_multidimensional_select_is_error_for_trace_cone_and_analysis(self):
        self._require_legacy()
        self._check_error(self.legacy_db, "diagnostic_top.p[2][1]", "legacy_multidimensional_select")

    def test_legacy_warning_reaches_analysis_as_strings(self):
        self._require_legacy()
        if not DEFAULT_WAVE_CLI.is_file():
            self.skipTest(f"real waveform binary unavailable: {DEFAULT_WAVE_CLI}")
        for handler in (tools.trace_with_snapshot, tools.rank_cone_by_time, tools.explain_signal_at_time):
            # A new serve process per handler also supports a reader which
            # emits this legacy warning only once per serve session.
            clients._cleanup_runtime_state()
            with self.subTest(handler=handler.__name__):
                result = handler(
                    db_path=str(self.legacy_db), waveform_path=str(self.waveform),
                    signal="diagnostic_top.v[2]", time=5, mode="loads",
                    rtl_trace_bin=str(self.binary), wave_cli_bin=str(DEFAULT_WAVE_CLI),
                )
                self.assertEqual(result["status"], "success", result)
                trace = result["structure"]["trace"]
                warnings = [d["message"] for d in trace["diagnostics"] if d["severity"] == "warning"]
                self.assertTrue(warnings, trace)
                self.assertTrue(all(isinstance(w, str) for w in result["warnings"]))
                for warning in warnings:
                    self.assertIn(warning, trace["warnings"])
                    self.assertIn(warning, result["warnings"])


if __name__ == "__main__":
    unittest.main()
