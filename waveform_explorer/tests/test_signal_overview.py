import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CLI = ROOT / "waveform_explorer" / "build" / "wave_agent_cli"

OVERVIEW_FIXTURE_VCD = """$date
  today
$end
$version
  signal overview fixture
$end
$timescale 1ns $end
$scope module TOP $end
$scope module tb $end
$var wire 1 ! sig $end
$var wire 4 " bus[3:0] $end
$var wire 1 # fast $end
$upscope $end
$upscope $end
$enddefinitions $end
#0
$dumpvars
0!
b0000 "
0#
$end
#2
1#
#4
0#
#6
1#
#8
0#
#10
1!
1#
#12
0#
#14
1#
#16
0#
#18
1#
#20
0!
0#
#22
1#
#24
0#
#25
1!
#26
1#
#28
0#
#30
0!
1#
#32
0#
#34
1#
#36
0#
#38
1#
#40
b0001 "
0#
#44
b0010 "
#48
b0011 "
#70
b0100 "
"""


@unittest.skipUnless(CLI.exists(), "wave_agent_cli not built")
class SignalOverviewCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.TemporaryDirectory()
        cls.vcd_path = Path(cls.temp_dir.name) / "overview_fixture.vcd"
        cls.vcd_path.write_text(OVERVIEW_FIXTURE_VCD, encoding="ascii")

    @classmethod
    def tearDownClass(cls):
        cls.temp_dir.cleanup()

    def _query(self, cmd, args):
        query = {"cmd": cmd, "args": args}
        completed = subprocess.run(
            [str(CLI), str(self.vcd_path), json.dumps(query)],
            check=True,
            capture_output=True,
            text=True,
        )
        return json.loads(completed.stdout)

    def _run(self, waveform_path, cmd, args):
        query = {"cmd": cmd, "args": args}
        completed = subprocess.run(
            [str(CLI), str(waveform_path), json.dumps(query)],
            capture_output=True,
            text=True,
        )
        return completed

    def test_single_bit_overview_reports_dense_region_as_flipping(self):
        result = self._query("get_signal_overview", {
            "path": "tb.sig",
            "start_time": 0,
            "end_time": 40,
            "resolution": 10,
        })

        self.assertEqual(result["status"], "success")
        self.assertEqual(result["timescale"], "1ns")
        self.assertEqual(result["resolution"], 10)
        self.assertEqual(result["segments"], [
            {"start": 0, "end": 9, "state": "0"},
            {"start": 10, "end": 19, "state": "1"},
            {"start": 20, "end": 29, "state": "flipping", "transitions": 3},
            {"start": 30, "end": 40, "state": "0"},
        ])

    def test_multibit_overview_reports_stable_values_and_burst_metadata(self):
        result = self._query("get_signal_overview", {
            "path": "tb.bus[3:0]",
            "start_time": 0,
            "end_time": 80,
            "resolution": 10,
            "radix": "hex",
        })

        self.assertEqual(result["status"], "success")
        self.assertEqual(result["width"], 4)
        self.assertEqual(result["radix"], "hex")
        self.assertEqual(result["segments"], [
            {"start": 0, "end": 39, "state": "stable", "value": "h0"},
            {"start": 40, "end": 47, "state": "flipping", "unique_values": 4, "transitions": 3},
            {"start": 48, "end": 69, "state": "stable", "value": "h3"},
            {"start": 70, "end": 80, "state": "stable", "value": "h4"},
        ])

    def test_auto_resolution_limits_segments(self):
        result = self._query("get_signal_overview", {
            "path": "tb.fast",
            "start_time": 0,
            "end_time": 40,
            "resolution": "auto",
        })

        self.assertEqual(result["status"], "success")
        self.assertEqual(result["requested_resolution"], "auto")
        self.assertIsInstance(result["resolution"], int)
        self.assertGreaterEqual(result["resolution"], 3)
        self.assertLessEqual(len(result["segments"]), 20)

    def test_find_condition_supports_backward_search(self):
        query = {"cmd": "find_condition", "args": {
            "expression": "tb.sig == 1",
            "start_time": 40,
            "direction": "backward",
        }}
        completed = subprocess.run(
            [str(CLI), str(self.vcd_path), json.dumps(query)],
            check=True,
            capture_output=True,
            text=True,
        )
        result = json.loads(completed.stdout)

        self.assertEqual(result["status"], "success")
        self.assertEqual(result["data"], 25)

    def test_malformed_vcd_timestamp_returns_error_instead_of_crashing(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            bad_vcd = Path(temp_dir) / "bad.vcd"
            bad_vcd.write_text(
                """$timescale 1ns $end
$scope module TOP $end
$var wire 1 ! sig $end
$upscope $end
$enddefinitions $end
#abc
0!
""",
                encoding="ascii",
            )

            completed = self._run(bad_vcd, "list_signals", {})

            self.assertNotEqual(completed.returncode, 0)
            result = json.loads(completed.stdout)
            self.assertEqual(result["status"], "error")
            self.assertIn("Failed to load waveform file", result["message"])


if __name__ == "__main__":
    unittest.main()
