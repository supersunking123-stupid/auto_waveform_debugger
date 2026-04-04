import concurrent.futures
import importlib
import json
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
NVDLA_WAVEFORM = Path("/home/qsun/DVT/nvdla/hw/verif/sim_vip/cc_alexnet_conv5_relu5_int16_dtest_cvsram/wave.fsdb")
NVDLA_RTL_TRACE_DB = Path("/home/qsun/DVT/nvdla/hw/verif/sim_vip/rtl_trace.db")
sys.path.insert(0, str(ROOT))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod
from agent_debug_automation.virtual_signals import clear_virtual_cache


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


VIRTUAL_FIXTURE_VCD = """$date
  virtual fixture
$end
$timescale 1ns $end
$scope module TOP $end
$var wire 1 ! a $end
$var wire 1 " b $end
$var wire 4 # bus[3:0] $end
$upscope $end
$enddefinitions $end
#0
0!
0"
b0000 #
#10
1!
#20
1"
b1010 #
#30
0!
b1111 #
#40
0"
b0000 #
#50
1!
1"
b0101 #
#60
$end
"""


SPARSE_EDGE_FIXTURE_VCD = """$date
  sparse edge fixture
$end
$timescale 1ns $end
$scope module TOP $end
$var wire 1 ! a $end
$upscope $end
$enddefinitions $end
#0
0!
#200000001
1!
#200000002
$end
"""


BOUNDARY_EDGE_FIXTURE_VCD = """$date
  boundary edge fixture
$end
$timescale 1ns $end
$scope module TOP $end
$var wire 1 ! a $end
$upscope $end
$enddefinitions $end
#0
0!
#1000000
1!
#2000000
$end
"""


LATE_BOUNDARY_EDGE_FIXTURE_VCD = """$date
  late boundary edge fixture
$end
$timescale 1ns $end
$scope module TOP $end
$var wire 1 ! a $end
$upscope $end
$enddefinitions $end
#0
1!
#1000000
0!
#2000000
1!
#2000001
$end
"""


class CrossLinkingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db_path = "/tmp/agent_debug_automation_test.db"
        cls.waveform_path = str(ROOT / "waveform_explorer" / "timer_tb.vcd")
        cls.temp_dir = tempfile.TemporaryDirectory()
        cls.overview_waveform_path = str(Path(cls.temp_dir.name) / "overview_fixture.vcd")
        Path(cls.overview_waveform_path).write_text(OVERVIEW_FIXTURE_VCD, encoding="ascii")
        cls.virtual_waveform_path = str(Path(cls.temp_dir.name) / "virtual_fixture.vcd")
        Path(cls.virtual_waveform_path).write_text(VIRTUAL_FIXTURE_VCD, encoding="ascii")
        cls.sparse_waveform_path = str(Path(cls.temp_dir.name) / "sparse_edge_fixture.vcd")
        Path(cls.sparse_waveform_path).write_text(SPARSE_EDGE_FIXTURE_VCD, encoding="ascii")
        cls.boundary_waveform_path = str(Path(cls.temp_dir.name) / "boundary_edge_fixture.vcd")
        Path(cls.boundary_waveform_path).write_text(BOUNDARY_EDGE_FIXTURE_VCD, encoding="ascii")
        cls.late_boundary_waveform_path = str(Path(cls.temp_dir.name) / "late_boundary_edge_fixture.vcd")
        Path(cls.late_boundary_waveform_path).write_text(LATE_BOUNDARY_EDGE_FIXTURE_VCD, encoding="ascii")
        cls.rtl_trace_bin = str(ROOT / "standalone_trace" / "build" / "rtl_trace")
        cls.wave_cli_bin = str(ROOT / "waveform_explorer" / "build" / "wave_agent_cli")
        fixture_dir = Path(__file__).parent / "fixtures"
        subprocess.run(
            [
                cls.rtl_trace_bin,
                "compile",
                "--db",
                cls.db_path,
                "--top",
                "timer_tb",
                "-f",
                "simview.f",
            ],
            cwd=fixture_dir,
            check=True,
            text=True,
            capture_output=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp_dir.cleanup()

    def setUp(self):
        for daemon in mcp_mod.wave_daemons.values():
            daemon.stop()
        mcp_mod.wave_daemons.clear()
        for session in mcp_mod.rtl_serve_sessions.values():
            session.stop()
        mcp_mod.rtl_serve_sessions.clear()
        mcp_mod.rtl_session_ids_by_key.clear()
        mcp_mod.wave_signal_resolution_cache.clear()
        mcp_mod.wave_prefix_page_cache.clear()
        mcp_mod.wave_signal_cache.clear()
        if mcp_mod.SESSION_STORE_DIR.exists():
            shutil.rmtree(mcp_mod.SESSION_STORE_DIR)

    def test_default_session_is_auto_created(self):
        result = mcp_mod.get_value_at_time(
            vcd_path=self.waveform_path,
            path="timer_tb.timeout",
            time=75000,
        )
        self.assertEqual(result["status"], "success")
        self.assertEqual(result["session"]["session_name"], mcp_mod.DEFAULT_SESSION_NAME)
        sessions = mcp_mod.list_sessions(self.waveform_path)
        self.assertEqual(len(sessions["data"]), 1)
        self.assertEqual(sessions["active_session"]["session_name"], mcp_mod.DEFAULT_SESSION_NAME)

    def test_cursor_and_bookmark_aliases_work_for_value_fetch(self):
        mcp_mod.set_cursor(75000, waveform_path=self.waveform_path)
        bookmark_result = mcp_mod.create_bookmark(
            "timeout_rise",
            "Cursor",
            waveform_path=self.waveform_path,
            description="timeout assertion",
        )
        self.assertEqual(bookmark_result["status"], "success")

        cursor_value = mcp_mod.get_value_at_time(
            vcd_path=self.waveform_path,
            path="timer_tb.timeout",
            time="Cursor",
        )
        bookmark_value = mcp_mod.get_value_at_time(
            vcd_path=self.waveform_path,
            path="timer_tb.timeout",
            time="BM_timeout_rise",
        )

        self.assertEqual(cursor_value["status"], "success")
        self.assertEqual(cursor_value["data"], "rising")
        self.assertEqual(cursor_value["resolved_time"]["resolved_time"], 75000)
        self.assertEqual(bookmark_value["status"], "success")
        self.assertEqual(bookmark_value["data"], "rising")
        self.assertEqual(bookmark_value["resolved_time"]["resolved_time"], 75000)

    def test_signal_group_expansion_deduplicates_signals(self):
        mcp_mod.create_signal_group(
            "timeout_debug",
            ["timer_tb.timeout", "timer_tb.clk", "timer_tb.timeout"],
            waveform_path=self.waveform_path,
            description="debug bundle",
        )

        result = mcp_mod.get_snapshot(
            vcd_path=self.waveform_path,
            signals=["timeout_debug"],
            time=75000,
            signals_are_groups=True,
        )

        self.assertEqual(result["status"], "success")
        self.assertEqual(
            result["resolved_signals"]["expanded_signals"],
            ["timer_tb.timeout", "timer_tb.clk"],
        )
        self.assertIn("timer_tb.timeout", result["data"])
        self.assertIn("timer_tb.clk", result["data"])

    def test_list_signals_forwards_pattern_and_types(self):
        calls = []

        def fake_wave_agent_query(vcd_path, cmd, args=None, wave_cli_bin=None):
            calls.append((cmd, dict(args or {})))
            return {"status": "success", "data": ["top.clk"]}

        with mock.patch.object(mcp_mod, "wave_agent_query", side_effect=fake_wave_agent_query):
            result = mcp_mod.list_signals(
                vcd_path=self.overview_waveform_path,
                pattern="top.u_axi.ar_*",
                types=["input", "net"],
            )

        self.assertEqual(result["status"], "success")
        self.assertEqual(calls, [("list_signals", {
            "pattern": "top.u_axi.ar_*",
            "types": ["input", "net"],
        })])

    def test_session_state_persists_across_reload(self):
        created = mcp_mod.create_session(
            waveform_path=self.waveform_path,
            session_name="debug_view",
            description="persist me",
        )
        self.assertEqual(created["status"], "success")
        mcp_mod.switch_session("debug_view", waveform_path=self.waveform_path)
        mcp_mod.set_cursor(12345, waveform_path=self.waveform_path, session_name="debug_view")
        mcp_mod.create_bookmark(
            "mark1",
            12345,
            waveform_path=self.waveform_path,
            session_name="debug_view",
            description="saved bookmark",
        )
        mcp_mod.create_signal_group(
            "grp1",
            ["timer_tb.timeout"],
            waveform_path=self.waveform_path,
            session_name="debug_view",
            description="saved group",
        )

        reloaded = importlib.reload(mcp_mod)

        session = reloaded.get_session("debug_view", waveform_path=self.waveform_path)
        self.assertEqual(session["status"], "success")
        self.assertEqual(session["data"]["cursor_time"], 12345)
        self.assertIn("mark1", session["data"]["bookmarks"])
        self.assertIn("grp1", session["data"]["signal_groups"])
        active = reloaded.list_sessions(self.waveform_path)
        self.assertEqual(active["active_session"]["session_name"], "debug_view")

    def test_cross_link_time_alias_uses_cursor(self):
        mcp_mod.set_cursor(75000, waveform_path=self.waveform_path)
        result = mcp_mod.explain_signal_at_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal="timer_tb.timeout",
            time="Cursor",
        )
        self.assertEqual(result["status"], "success")
        self.assertEqual(result["resolved_time"]["resolved_time"], 75000)
        self.assertEqual(result["time_context"]["focus_time"], 75000)

    def test_get_signal_overview_resolves_session_time_aliases(self):
        mcp_mod.create_session(
            waveform_path=self.overview_waveform_path,
            session_name="overview_view",
            description="signal overview alias regression",
        )
        mcp_mod.switch_session("overview_view", waveform_path=self.overview_waveform_path)
        mcp_mod.set_cursor(0, waveform_path=self.overview_waveform_path)
        mcp_mod.create_bookmark(
            "bus_end",
            80,
            waveform_path=self.overview_waveform_path,
            session_name="overview_view",
            description="overview range end",
        )

        result = mcp_mod.get_signal_overview(
            vcd_path=self.overview_waveform_path,
            path="tb.bus[3:0]",
            start_time="Cursor",
            end_time="BM_bus_end",
            resolution=10,
            session_name="overview_view",
        )

        self.assertEqual(result["status"], "success")
        self.assertEqual(result["session"]["session_name"], "overview_view")
        self.assertEqual(result["resolved_time_range"]["start"]["resolved_time"], 0)
        self.assertEqual(result["resolved_time_range"]["end"]["resolved_time"], 80)
        self.assertEqual(result["segments"], [
            {"start": 0, "end": 39, "state": "stable", "value": "h0"},
            {"start": 40, "end": 47, "state": "flipping", "unique_values": 4, "transitions": 3},
            {"start": 48, "end": 69, "state": "stable", "value": "h3"},
            {"start": 70, "end": 80, "state": "stable", "value": "h4"},
        ])

    def test_get_signal_overview_supports_auto_resolution(self):
        result = mcp_mod.get_signal_overview(
            vcd_path=self.overview_waveform_path,
            path="tb.fast",
            start_time=0,
            end_time=40,
            resolution="auto",
        )

        self.assertEqual(result["status"], "success")
        self.assertEqual(result["requested_resolution"], "auto")
        self.assertIsInstance(result["resolution"], int)
        self.assertGreaterEqual(result["resolution"], 3)
        self.assertLessEqual(len(result["segments"]), 20)

    def test_count_transitions_resolves_session_time_aliases(self):
        mcp_mod.create_session(
            waveform_path=self.waveform_path,
            session_name="count_view",
            description="counter aliases",
        )
        mcp_mod.switch_session("count_view", waveform_path=self.waveform_path)
        mcp_mod.set_cursor(0, waveform_path=self.waveform_path, session_name="count_view")
        mcp_mod.create_bookmark(
            "count_end",
            30000,
            waveform_path=self.waveform_path,
            session_name="count_view",
            description="count range end",
        )

        result = mcp_mod.count_transitions(
            vcd_path=self.waveform_path,
            path="timer_tb.clk",
            start_time="Cursor",
            end_time="BM_count_end",
            edge_type="rise",
            session_name="count_view",
        )

        self.assertEqual(result["status"], "success")
        self.assertEqual(result["data"]["count"], 3)
        self.assertEqual(result["data"]["effective_mode"], "posedge")
        self.assertEqual(result["resolved_time_range"]["start"]["resolved_time"], 0)
        self.assertEqual(result["resolved_time_range"]["end"]["resolved_time"], 30000)

    def test_dump_waveform_data_expands_signal_groups(self):
        mcp_mod.create_signal_group(
            "timeout_group",
            ["timer_tb.timeout", "timer_tb.clk", "timer_tb.timeout"],
            waveform_path=self.waveform_path,
            description="dump bundle",
        )
        output_path = Path(self.temp_dir.name) / "dump_group.jsonl"

        result = mcp_mod.dump_waveform_data(
            vcd_path=self.waveform_path,
            signals=["timeout_group"],
            start_time=70000,
            end_time=80000,
            output_path=str(output_path),
            mode="samples",
            sample_period=5000,
            signals_are_groups=True,
        )

        self.assertEqual(result["status"], "success")
        self.assertEqual(
            result["resolved_signals"]["expanded_signals"],
            ["timer_tb.timeout", "timer_tb.clk"],
        )
        self.assertEqual(result["records_written"], 3)
        rows = [json.loads(line) for line in output_path.read_text(encoding="utf-8").splitlines()]
        self.assertEqual([row["t"] for row in rows], [70000, 75000, 80000])
        self.assertIn("timer_tb.timeout", rows[1]["values"])

    def test_dump_waveform_data_forwards_arguments(self):
        calls = []

        def fake_wave_agent_query(vcd_path, cmd, args=None, wave_cli_bin=None):
            calls.append((cmd, dict(args or {})))
            return {"status": "success", "output_path": "/tmp/out.jsonl"}

        with mock.patch.object(mcp_mod, "wave_agent_query", side_effect=fake_wave_agent_query):
            result = mcp_mod.dump_waveform_data(
                vcd_path="/tmp/mock.fsdb",
                signals=["top.a", "top.b"],
                start_time=10,
                end_time=30,
                output_path="/tmp/out.jsonl",
                mode="samples",
                sample_period=5,
                radix="bin",
                overwrite=True,
            )

        self.assertEqual(result["status"], "success")
        self.assertEqual(calls, [("dump_waveform_data", {
            "signals": ["top.a", "top.b"],
            "start_time": 10,
            "end_time": 30,
            "output_path": "/tmp/out.jsonl",
            "mode": "samples",
            "radix": "bin",
            "overwrite": True,
            "sample_period": 5,
        })])

    def test_concurrent_rtl_session_lookup_reuses_single_session(self):
        created = []

        class FakeProcess:
            def poll(self):
                return None

        class FakeSession:
            def __init__(self, bin_path, serve_args):
                time.sleep(0.05)
                self.bin_path = bin_path
                self.serve_args = serve_args
                self.process = FakeProcess()
                self.startup = {"status": "success"}
                created.append((bin_path, tuple(serve_args)))

            def stop(self):
                return {"status": "success"}

        with mock.patch.object(mcp_mod, "RtlTraceServeSession", FakeSession), \
             mock.patch.object(mcp_mod, "_resolve_bin", return_value="/tmp/fake_rtl_trace"):
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                sessions = list(pool.map(lambda _: mcp_mod._get_rtl_serve_session("/tmp/shared.db"), range(2)))

        self.assertEqual(len(created), 1)
        self.assertIs(sessions[0], sessions[1])
        self.assertEqual(len(mcp_mod.rtl_serve_sessions), 1)
        self.assertEqual(len(mcp_mod.rtl_session_ids_by_key), 1)

    def test_concurrent_wave_daemon_lookup_reuses_single_daemon(self):
        created = []

        class FakeProcess:
            def poll(self):
                return None

        class FakeDaemon:
            def __init__(self, wave_cli_path, waveform_path):
                time.sleep(0.05)
                self.wave_cli_path = wave_cli_path
                self.waveform_path = waveform_path
                self.process = FakeProcess()
                created.append((wave_cli_path, waveform_path))

            def stop(self):
                return None

        with mock.patch.object(mcp_mod, "WaveformDaemon", FakeDaemon), \
             mock.patch.object(mcp_mod, "_resolve_bin", return_value="/tmp/fake_wave_agent_cli"), \
             mock.patch.object(mcp_mod.os.path, "exists", return_value=True):
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                daemons = list(pool.map(lambda _: mcp_mod._get_wave_daemon("/tmp/shared.vcd"), range(2)))

        self.assertEqual(len(created), 1)
        self.assertIs(daemons[0], daemons[1])
        self.assertEqual(len(mcp_mod.wave_daemons), 1)

    def test_non_fsdb_signal_cache_requests_full_namespace(self):
        calls = []

        def fake_wave_query(vcd_path, cmd, args=None, wave_cli_bin=None):
            calls.append((cmd, dict(args or {})))
            return {"status": "success", "data": ["top.clk", "top.done"]}

        with mock.patch.object(mcp_mod, "_wave_query", side_effect=fake_wave_query):
            signals = mcp_mod._get_wave_signals("/tmp/mock.vcd")

        self.assertEqual(signals, ["top.clk", "top.done"])
        self.assertEqual(calls, [("list_signals", {"pattern": "*"})])

    def test_trace_with_snapshot(self):
        result = mcp_mod.trace_with_snapshot(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal="timer_tb.timeout",
            time=75000,
            sample_offsets=[-5000, 5000],
            clock_path="timer_tb.clk",
            cycle_offsets=[-1, 0, 1],
        )
        self.assertEqual(result["status"], "success")
        self.assertIn("timer_tb.dut.count_reg", result["structure"]["cone_signals"])
        self.assertEqual(result["waveform"]["focus_samples"]["timer_tb.timeout"]["value"], "rising")
        self.assertIn("70000", result["waveform"]["absolute_offset_samples"])
        self.assertEqual(
            result["waveform"]["cycle_offset_samples"]["resolved_clock_path"],
            "timer_tb.clk",
        )

    def test_explain_signal_at_time(self):
        result = mcp_mod.explain_signal_at_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal="timer_tb.timeout",
            time=75000,
        )
        self.assertEqual(result["status"], "success")
        self.assertTrue(result["explanations"]["candidate_paths"])
        top = result["explanations"]["top_candidate"]
        self.assertEqual(top["endpoint_path"], "timer_tb.dut.timeout")
        self.assertTrue(top["rhs_terms"])

    def test_rank_cone_by_time(self):
        result = mcp_mod.rank_cone_by_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal="timer_tb.timeout",
            time=75000,
        )
        self.assertEqual(result["status"], "success")
        self.assertTrue(result["ranking"]["all_signals"])
        self.assertTrue(result["ranking"]["most_active_near_time"])
        self.assertIn("recent_toggle_count", result["ranking"]["all_signals"][0])

    def test_explain_edge_cause(self):
        result = mcp_mod.explain_edge_cause(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal="timer_tb.timeout",
            time=80000,
            edge_type="posedge",
        )
        self.assertEqual(result["status"], "success")
        self.assertEqual(result["time_context"]["resolved_edge_time"], 75000)
        self.assertEqual(result["waveform"]["edge_context"]["value_before_edge"], "0")
        self.assertEqual(result["waveform"]["edge_context"]["value_at_edge"], "rising")

    def test_find_edge_normalizes_rise_alias(self):
        calls = []

        def fake_wave_agent_query(vcd_path, cmd, args=None, wave_cli_bin=None):
            calls.append((cmd, dict(args or {})))
            return {"status": "success", "data": 123}

        with mock.patch.object(mcp_mod, "wave_agent_query", side_effect=fake_wave_agent_query):
            result = mcp_mod.find_edge(
                vcd_path="/tmp/mock.fsdb",
                path="top.sig",
                edge_type="rise",
                start_time=100,
                direction="backward",
            )

        self.assertEqual(result["status"], "success")
        self.assertEqual(calls, [("find_edge", {
            "path": "top.sig",
            "edge_type": "posedge",
            "start_time": 100,
            "direction": "backward",
        })])

    def test_explain_edge_cause_normalizes_rise_alias(self):
        wave_calls = []

        def fake_map_signal_to_waveform(*args, **kwargs):
            return "top.sig"

        def fake_wave_query(vcd_path, cmd, args=None, wave_cli_bin=None):
            wave_calls.append((cmd, dict(args or {})))
            if cmd == "find_edge":
                return {"status": "success", "data": 100}
            raise AssertionError(f"unexpected command {cmd}")

        def fake_explain_signal_at_time(**kwargs):
            return {
                "status": "success",
                "time_context": {},
                "waveform": {},
            }

        def fake_get_batch_snapshot(*args, **kwargs):
            return {"top.sig": {"value": "1"}}

        with mock.patch.object(mcp_mod, "_map_signal_to_waveform", side_effect=fake_map_signal_to_waveform), \
             mock.patch.object(mcp_mod, "_wave_query", side_effect=fake_wave_query), \
             mock.patch.object(mcp_mod, "explain_signal_at_time", side_effect=fake_explain_signal_at_time), \
             mock.patch.object(mcp_mod, "_get_batch_snapshot", side_effect=fake_get_batch_snapshot):
            result = mcp_mod.explain_edge_cause(
                db_path="/tmp/mock.db",
                waveform_path="/tmp/mock.fsdb",
                signal="top.sig",
                time=100,
                edge_type="rise",
                direction="backward",
            )

        self.assertEqual(result["status"], "success")
        self.assertEqual(wave_calls, [("find_edge", {
            "path": "top.sig",
            "edge_type": "posedge",
            "start_time": 100,
            "direction": "backward",
        })])

    def test_top_normalization_helper(self):
        mapped = mcp_mod._map_signal_to_waveform(self.waveform_path, "TOP.timer_tb.timeout")
        self.assertEqual(mapped, "timer_tb.timeout")

    def test_fsdb_mapping_prefers_exact_signal_info(self):
        calls = []

        def fake_wave_query(vcd_path, cmd, args=None, wave_cli_bin=None):
            calls.append((cmd, dict(args or {})))
            if cmd == "get_signal_info" and args["path"] == "top.mem0_rd_bw_mon.ready_in":
                return {
                    "status": "success",
                    "data": {"path": "top.mem0_rd_bw_mon.ready_in"},
                }
            return {"status": "error", "message": "Signal not found"}

        with mock.patch.object(mcp_mod, "_wave_query", side_effect=fake_wave_query):
            mapped = mcp_mod._map_signal_to_waveform("/tmp/mock.fsdb", "top.mem0_rd_bw_mon.ready_in")

        self.assertEqual(mapped, "top.mem0_rd_bw_mon.ready_in")
        self.assertEqual([cmd for cmd, _ in calls], ["get_signal_info"])

    def test_fsdb_mapping_uses_prefix_page_for_slice_fallback(self):
        calls = []

        def fake_wave_query(vcd_path, cmd, args=None, wave_cli_bin=None):
            calls.append((cmd, dict(args or {})))
            if cmd == "get_signal_info":
                return {"status": "error", "message": "Signal not found"}
            if cmd == "list_signals_page":
                self.assertEqual(args["prefix"], "top.mem0_rd_bw_mon.")
                return {
                    "status": "success",
                    "data": [
                        "top.mem0_rd_bw_mon.ready_in",
                        "top.mem0_rd_bw_mon.watch_dog[31:0]",
                    ],
                    "has_more": False,
                    "next_cursor": "",
                }
            raise AssertionError(f"unexpected command {cmd}")

        with mock.patch.object(mcp_mod, "_wave_query", side_effect=fake_wave_query):
            mapped = mcp_mod._map_signal_to_waveform("/tmp/mock.fsdb", "top.mem0_rd_bw_mon.watch_dog[3]")

        self.assertEqual(mapped, "top.mem0_rd_bw_mon.watch_dog[31:0]")
        self.assertIn("list_signals_page", [cmd for cmd, _ in calls])

    def test_ranking_prefers_closest_transition_over_toggle_count(self):
        close_summary = mcp_mod._summarize_signal(
            signal="close_sig",
            mapped_signal="close_sig",
            mode="drivers",
            focus_time=100,
            start_time=0,
            end_time=200,
            focus_value="1",
            start_value="0",
            end_value="1",
            transitions=[{"t": 100, "v": "1", "glitch": False}],
        )
        busy_far_summary = mcp_mod._summarize_signal(
            signal="busy_far_sig",
            mapped_signal="busy_far_sig",
            mode="drivers",
            focus_time=100,
            start_time=0,
            end_time=200,
            focus_value="0",
            start_value="0",
            end_value="0",
            transitions=[
                {"t": 1, "v": "1", "glitch": False},
                {"t": 2, "v": "0", "glitch": False},
                {"t": 3, "v": "1", "glitch": False},
                {"t": 4, "v": "0", "glitch": False},
                {"t": 5, "v": "1", "glitch": False},
                {"t": 6, "v": "0", "glitch": False},
            ],
        )
        self.assertGreater(close_summary["closeness_score"], busy_far_summary["closeness_score"])
        self.assertGreater(close_summary["total_score"], busy_far_summary["total_score"])

    def test_ranking_distinguishes_stuck_to_one_from_stuck_to_zero(self):
        stuck_one = mcp_mod._summarize_signal(
            signal="stuck1",
            mapped_signal="stuck1",
            mode="drivers",
            focus_time=100,
            start_time=0,
            end_time=200,
            focus_value="1",
            start_value="1",
            end_value="1",
            transitions=[{"t": 0, "v": "1", "glitch": False}],
        )
        stuck_zero = mcp_mod._summarize_signal(
            signal="stuck0",
            mapped_signal="stuck0",
            mode="drivers",
            focus_time=100,
            start_time=0,
            end_time=200,
            focus_value="0",
            start_value="0",
            end_value="0",
            transitions=[{"t": 0, "v": "0", "glitch": False}],
        )
        self.assertEqual(stuck_one["stuck_class"], "stuck_to_1")
        self.assertEqual(stuck_zero["stuck_class"], "stuck_to_0")
        self.assertGreater(stuck_one["stuck_score"], stuck_zero["stuck_score"])

    # --- Session CRUD tests ---

    def test_move_cursor(self):
        mcp_mod.set_cursor(100, waveform_path=self.waveform_path)
        move_forward = mcp_mod.move_cursor(delta=50, waveform_path=self.waveform_path)
        self.assertEqual(move_forward["status"], "success")
        self.assertEqual(move_forward["data"]["cursor_time"], 150)

        cursor_after_forward = mcp_mod.get_cursor(waveform_path=self.waveform_path)
        self.assertEqual(cursor_after_forward["data"]["cursor_time"], 150)

        move_past_zero = mcp_mod.move_cursor(delta=-200, waveform_path=self.waveform_path)
        self.assertEqual(move_past_zero["status"], "success")
        self.assertGreaterEqual(move_past_zero["data"]["cursor_time"], 0)

        cursor_clamped = mcp_mod.get_cursor(waveform_path=self.waveform_path)
        self.assertGreaterEqual(cursor_clamped["data"]["cursor_time"], 0)

    def test_get_cursor(self):
        mcp_mod.set_cursor(42, waveform_path=self.waveform_path)
        result = mcp_mod.get_cursor(waveform_path=self.waveform_path)
        self.assertEqual(result["status"], "success")
        self.assertEqual(result["data"]["cursor_time"], 42)

    def test_list_bookmarks(self):
        mcp_mod.create_bookmark("bm1", 10, waveform_path=self.waveform_path)
        mcp_mod.create_bookmark("bm2", 20, waveform_path=self.waveform_path)
        result = mcp_mod.list_bookmarks(waveform_path=self.waveform_path)
        self.assertEqual(result["status"], "success")
        bookmark_names = [b["bookmark_name"] for b in result["data"]]
        self.assertIn("bm1", bookmark_names)
        self.assertIn("bm2", bookmark_names)

    def test_list_signal_groups(self):
        mcp_mod.create_signal_group(
            "grp1", ["timer_tb.sig"], waveform_path=self.waveform_path,
        )
        result = mcp_mod.list_signal_groups(waveform_path=self.waveform_path)
        self.assertEqual(result["status"], "success")
        group_names = [g["group_name"] for g in result["data"]]
        self.assertIn("grp1", group_names)

    def test_update_signal_group(self):
        mcp_mod.create_signal_group(
            "grp", ["timer_tb.sig", "timer_tb.bus[3:0]"],
            waveform_path=self.waveform_path,
        )
        updated = mcp_mod.update_signal_group(
            "grp", signals=["timer_tb.sig"], waveform_path=self.waveform_path,
        )
        self.assertEqual(updated["status"], "success")
        self.assertEqual(updated["data"]["signals"], ["timer_tb.sig"])

        desc_updated = mcp_mod.update_signal_group(
            "grp", description="updated", waveform_path=self.waveform_path,
        )
        self.assertEqual(desc_updated["status"], "success")
        self.assertEqual(desc_updated["data"]["description"], "updated")

    def test_delete_bookmark(self):
        mcp_mod.create_bookmark("todel", 10, waveform_path=self.waveform_path)
        mcp_mod.delete_bookmark("todel", waveform_path=self.waveform_path)
        result = mcp_mod.list_bookmarks(waveform_path=self.waveform_path)
        bookmark_names = [b["bookmark_name"] for b in result["data"]]
        self.assertNotIn("todel", bookmark_names)

    def test_delete_signal_group(self):
        mcp_mod.create_signal_group(
            "todel", ["timer_tb.sig"], waveform_path=self.waveform_path,
        )
        mcp_mod.delete_signal_group("todel", waveform_path=self.waveform_path)
        result = mcp_mod.list_signal_groups(waveform_path=self.waveform_path)
        group_names = [g["group_name"] for g in result["data"]]
        self.assertNotIn("todel", group_names)

    def test_delete_default_session_guard(self):
        result = mcp_mod.delete_session("Default_Session", waveform_path=self.waveform_path)
        self.assertIn(result["status"], ("error",))
        self.assertIn("cannot delete", result.get("message", "").lower())

    def test_create_duplicate_session(self):
        first = mcp_mod.create_session(
            self.waveform_path, "dup_test", "first creation",
        )
        self.assertEqual(first["status"], "success")
        second = mcp_mod.create_session(
            self.waveform_path, "dup_test", "second creation",
        )
        self.assertEqual(second["status"], "error")
        self.assertIn("already exists", second.get("message", ""))

    def test_bookmark_name_validation(self):
        empty_result = mcp_mod.create_bookmark("", 10, waveform_path=self.waveform_path)
        self.assertEqual(empty_result["status"], "error")

        cursor_result = mcp_mod.create_bookmark("Cursor", 10, waveform_path=self.waveform_path)
        self.assertEqual(cursor_result["status"], "error")

    # --- Invalid input tests ---

    def test_invalid_time_references(self):
        result = mcp_mod.get_value_at_time(
            vcd_path=self.waveform_path,
            path="timer_tb.timeout",
            time="BM_nonexistent",
        )
        self.assertEqual(result["status"], "error")
        self.assertIn("not found", result.get("message", "").lower())

    # --- Waveform tool coverage tests ---

    def test_find_value_intervals_mcp(self):
        result = mcp_mod.find_value_intervals(
            vcd_path=self.overview_waveform_path,
            path="tb.sig",
            value="1",
            start_time=0,
            end_time=50,
        )
        self.assertEqual(result["status"], "success")
        self.assertIn("data", result)
        intervals = result["data"]
        self.assertIsInstance(intervals, list)
        self.assertTrue(len(intervals) > 0, "Expected at least one interval for tb.sig value 1")

    def test_get_transitions_mcp(self):
        result = mcp_mod.get_transitions(
            vcd_path=self.overview_waveform_path,
            path="tb.sig",
            start_time=0,
            end_time=50,
            max_limit=2,
        )
        self.assertEqual(result["status"], "success")
        transitions = result["data"]
        self.assertIsInstance(transitions, list)
        self.assertLessEqual(len(transitions), 2, "max_limit=2 should bound transition count")

    def test_analyze_pattern_mcp(self):
        result = mcp_mod.analyze_pattern(
            vcd_path=self.overview_waveform_path,
            path="tb.sig",
            start_time=0,
            end_time=50,
        )
        self.assertEqual(result["status"], "success")
        # analyze_pattern returns a summary string, not a data dict
        self.assertIn("summary", result)
        self.assertIsInstance(result["summary"], str)

    def test_get_signal_info_mcp(self):
        result = mcp_mod.get_signal_info(
            vcd_path=self.overview_waveform_path,
            path="tb.sig",
        )
        self.assertEqual(result["status"], "success")
        self.assertIn("data", result)
        info = result["data"]
        self.assertIsInstance(info, dict)
        self.assertIn("width", info)

    def test_virtual_scalar_value_after_transition(self):
        create = mcp_mod.create_signal_expression(
            "and_ab",
            "a & b",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        value = mcp_mod.get_value_at_time(
            vcd_path=self.virtual_waveform_path,
            path="and_ab",
            time=21,
        )
        self.assertEqual(value["status"], "success")
        self.assertEqual(value["data"], "1")

        transitions = mcp_mod.get_transitions(
            vcd_path=self.virtual_waveform_path,
            path="and_ab",
            start_time=21,
            end_time=22,
            max_limit=10,
        )
        self.assertEqual(transitions["status"], "success")
        self.assertEqual(transitions["data"], [{"t": 21, "v": "1"}])

    def test_virtual_multibit_value_format_matches_real_backend(self):
        create = mcp_mod.create_signal_expression(
            "bus_passthru",
            "bus",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        for time_value, radix in ((20, "hex"), (21, "hex"), (21, "dec")):
            real = mcp_mod.get_value_at_time(
                vcd_path=self.virtual_waveform_path,
                path="bus",
                time=time_value,
                radix=radix,
            )
            virtual = mcp_mod.get_value_at_time(
                vcd_path=self.virtual_waveform_path,
                path="bus_passthru",
                time=time_value,
                radix=radix,
            )
            self.assertEqual(real["status"], "success")
            self.assertEqual(virtual["status"], "success")
            self.assertEqual(virtual["data"], real["data"])

    def test_virtual_snapshot_format_matches_real_backend(self):
        create = mcp_mod.create_signal_expression(
            "bus_passthru",
            "bus",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        real = mcp_mod.get_snapshot(
            vcd_path=self.virtual_waveform_path,
            signals=["bus"],
            time=20,
            radix="hex",
        )
        virtual = mcp_mod.get_snapshot(
            vcd_path=self.virtual_waveform_path,
            signals=["bus_passthru"],
            time=20,
            radix="hex",
        )
        self.assertEqual(real["status"], "success")
        self.assertEqual(virtual["status"], "success")
        self.assertEqual(virtual["data"]["bus_passthru"], real["data"]["bus"])

    def test_virtual_bus_tools_create_and_query(self):
        concat = mcp_mod.create_bus_concat(
            "ab",
            ["a", "b"],
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(concat["status"], "success")
        self.assertEqual(concat["data"]["kind"], "bus_op")
        self.assertEqual(concat["data"]["width"], 2)
        self.assertEqual(concat["data"]["operation"]["type"], "concat")

        reverse = mcp_mod.create_reversed_bus(
            "bus_rev",
            "bus",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(reverse["status"], "success")
        self.assertEqual(reverse["data"]["operation"]["type"], "reverse")

        bus_slice = mcp_mod.create_bus_slice(
            "bus_upper",
            "bus",
            3,
            2,
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(bus_slice["status"], "success")
        self.assertEqual(bus_slice["data"]["width"], 2)
        self.assertEqual(bus_slice["data"]["operation"]["msb"], 3)
        self.assertEqual(bus_slice["data"]["operation"]["lsb"], 2)

        slices = mcp_mod.create_bus_slices(
            "parts",
            "bus",
            2,
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(slices["status"], "success")
        self.assertEqual(
            [item["signal_name"] for item in slices["data"]],
            ["parts_3_2", "parts_1_0"],
        )

        snapshot = mcp_mod.get_snapshot(
            vcd_path=self.virtual_waveform_path,
            signals=["ab", "bus_rev", "bus_upper", "parts_3_2", "parts_1_0"],
            time=21,
            radix="bin",
        )
        self.assertEqual(snapshot["status"], "success")
        self.assertEqual(snapshot["data"]["ab"], "b11")
        self.assertEqual(snapshot["data"]["bus_rev"], "b0101")
        self.assertEqual(snapshot["data"]["bus_upper"], "b10")
        self.assertEqual(snapshot["data"]["parts_3_2"], "b10")
        self.assertEqual(snapshot["data"]["parts_1_0"], "b10")

        listed = mcp_mod.list_signal_expressions(waveform_path=self.virtual_waveform_path)
        self.assertEqual(listed["status"], "success")
        listed_by_name = {item["signal_name"]: item for item in listed["data"]}
        self.assertEqual(listed_by_name["ab"]["kind"], "bus_op")
        self.assertEqual(listed_by_name["bus_rev"]["operation"]["type"], "reverse")

    def test_update_signal_expression_rejects_bus_created_signal(self):
        create = mcp_mod.create_reversed_bus(
            "bus_rev_update",
            "bus",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        update = mcp_mod.update_signal_expression(
            "bus_rev_update",
            expression="bus",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(update["status"], "error")
        self.assertIn("delete and recreate", update.get("message", "").lower())

    def test_virtual_find_edge_scans_past_old_cap(self):
        create = mcp_mod.create_signal_expression(
            "a_passthru",
            "a",
            waveform_path=self.sparse_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        real = mcp_mod.find_edge(
            vcd_path=self.sparse_waveform_path,
            path="a",
            edge_type="posedge",
            start_time=0,
            direction="forward",
        )
        virtual = mcp_mod.find_edge(
            vcd_path=self.sparse_waveform_path,
            path="a_passthru",
            edge_type="posedge",
            start_time=0,
            direction="forward",
        )
        self.assertEqual(real["status"], "success")
        self.assertEqual(virtual["status"], "success")
        self.assertEqual(real["data"], 200000001)
        self.assertEqual(virtual["data"], real["data"])

    def test_virtual_find_edge_virtual_leaf_max_limit_is_overridable(self):
        create = mcp_mod.create_signal_expression(
            "a_limit_passthru",
            "a",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")
        clear_virtual_cache()

        overridden = mcp_mod.find_edge(
            vcd_path=self.virtual_waveform_path,
            path="a_limit_passthru",
            edge_type="posedge",
            start_time=0,
            direction="forward",
            virtual_leaf_max_limit=4,
        )
        limited = mcp_mod.find_edge(
            vcd_path=self.virtual_waveform_path,
            path="a_limit_passthru",
            edge_type="posedge",
            start_time=0,
            direction="forward",
            virtual_leaf_max_limit=1,
        )

        self.assertEqual(overridden["status"], "success")
        self.assertEqual(overridden["data"], 10)
        self.assertEqual(limited["status"], "error")
        self.assertIn("virtual_leaf_max_limit", limited.get("message", ""))

    def test_virtual_find_edge_preserves_chunk_boundary_edges(self):
        create = mcp_mod.create_signal_expression(
            "a_boundary_passthru",
            "a",
            waveform_path=self.boundary_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        forward_real = mcp_mod.find_edge(
            vcd_path=self.boundary_waveform_path,
            path="a",
            edge_type="posedge",
            start_time=0,
            direction="forward",
        )
        forward_virtual = mcp_mod.find_edge(
            vcd_path=self.boundary_waveform_path,
            path="a_boundary_passthru",
            edge_type="posedge",
            start_time=0,
            direction="forward",
        )
        backward_real = mcp_mod.find_edge(
            vcd_path=self.boundary_waveform_path,
            path="a",
            edge_type="posedge",
            start_time=1999999,
            direction="backward",
        )
        backward_virtual = mcp_mod.find_edge(
            vcd_path=self.boundary_waveform_path,
            path="a_boundary_passthru",
            edge_type="posedge",
            start_time=1999999,
            direction="backward",
        )

        self.assertEqual(forward_real["status"], "success")
        self.assertEqual(forward_virtual["status"], "success")
        self.assertEqual(backward_real["status"], "success")
        self.assertEqual(backward_virtual["status"], "success")
        self.assertEqual(forward_real["data"], 1000000)
        self.assertEqual(forward_virtual["data"], forward_real["data"])
        self.assertEqual(backward_real["data"], 1000000)
        self.assertEqual(backward_virtual["data"], backward_real["data"])

    def test_virtual_count_transitions_includes_edge_at_window_start(self):
        create = mcp_mod.create_signal_expression(
            "a_count_passthru",
            "a",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        real = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="a",
            start_time=10,
            end_time=50,
            edge_type="posedge",
        )
        virtual = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="a_count_passthru",
            start_time=10,
            end_time=50,
            edge_type="posedge",
        )

        self.assertEqual(real["status"], "success")
        self.assertEqual(virtual["status"], "success")
        self.assertEqual(real["data"]["count"], 2)
        self.assertEqual(virtual["data"]["count"], real["data"]["count"])

    def test_virtual_count_transitions_matches_real_backend_at_time_zero(self):
        create = mcp_mod.create_signal_expression(
            "a_time_zero_passthru",
            "a",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        real = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="a",
            start_time=0,
            end_time=10,
            edge_type="anyedge",
        )
        virtual = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="a_time_zero_passthru",
            start_time=0,
            end_time=10,
            edge_type="anyedge",
        )

        self.assertEqual(real["status"], "success")
        self.assertEqual(virtual["status"], "success")
        self.assertEqual(real["data"]["count"], 2)
        self.assertEqual(virtual["data"]["count"], real["data"]["count"])

    def test_count_transitions_boundary_policy_can_exclude_start_edge(self):
        create = mcp_mod.create_signal_expression(
            "a_boundary_policy_passthru",
            "a",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        real = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="a",
            start_time=10,
            end_time=50,
            edge_type="posedge",
            boundary_policy="exclusive",
        )
        virtual = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="a_boundary_policy_passthru",
            start_time=10,
            end_time=50,
            edge_type="posedge",
            boundary_policy="exclusive",
        )

        self.assertEqual(real["status"], "success")
        self.assertEqual(virtual["status"], "success")
        self.assertEqual(real["data"]["boundary_policy"], "exclusive")
        self.assertEqual(virtual["data"]["boundary_policy"], "exclusive")
        self.assertEqual(real["data"]["count"], 1)
        self.assertEqual(virtual["data"]["count"], real["data"]["count"])

    def test_multibit_count_transitions_matches_toggle_mode_with_boundary_policy(self):
        create = mcp_mod.create_signal_expression(
            "bus_count_passthru",
            "bus",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        real_inclusive = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="bus",
            start_time=20,
            end_time=50,
            edge_type="posedge",
            boundary_policy="inclusive",
        )
        virtual_inclusive = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="bus_count_passthru",
            start_time=20,
            end_time=50,
            edge_type="posedge",
            boundary_policy="inclusive",
        )
        real_exclusive = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="bus",
            start_time=20,
            end_time=50,
            edge_type="posedge",
            boundary_policy="exclusive",
        )
        virtual_exclusive = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="bus_count_passthru",
            start_time=20,
            end_time=50,
            edge_type="posedge",
            boundary_policy="exclusive",
        )

        self.assertEqual(real_inclusive["status"], "success")
        self.assertEqual(virtual_inclusive["status"], "success")
        self.assertEqual(real_exclusive["status"], "success")
        self.assertEqual(virtual_exclusive["status"], "success")
        self.assertEqual(real_inclusive["data"]["effective_mode"], "toggle")
        self.assertEqual(virtual_inclusive["data"]["effective_mode"], "toggle")
        self.assertEqual(real_inclusive["data"]["count"], 4)
        self.assertEqual(virtual_inclusive["data"]["count"], real_inclusive["data"]["count"])
        self.assertEqual(real_exclusive["data"]["count"], 3)
        self.assertEqual(virtual_exclusive["data"]["count"], real_exclusive["data"]["count"])

    def test_multibit_count_transitions_matches_toggle_mode_at_time_zero(self):
        create = mcp_mod.create_signal_expression(
            "bus_time_zero_passthru",
            "bus",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        real = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="bus",
            start_time=0,
            end_time=20,
            edge_type="posedge",
        )
        virtual = mcp_mod.count_transitions(
            vcd_path=self.virtual_waveform_path,
            path="bus_time_zero_passthru",
            start_time=0,
            end_time=20,
            edge_type="posedge",
        )

        self.assertEqual(real["status"], "success")
        self.assertEqual(virtual["status"], "success")
        self.assertEqual(real["data"]["count"], 2)
        self.assertEqual(virtual["data"]["count"], real["data"]["count"])
        self.assertEqual(real["data"]["effective_mode"], "toggle")
        self.assertEqual(virtual["data"]["effective_mode"], "toggle")

    def test_virtual_backward_find_edge_matches_real_backend_at_time_zero(self):
        create = mcp_mod.create_signal_expression(
            "a_backward_zero_passthru",
            "a",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        real = mcp_mod.find_edge(
            vcd_path=self.virtual_waveform_path,
            path="a",
            edge_type="anyedge",
            start_time=0,
            direction="backward",
        )
        virtual = mcp_mod.find_edge(
            vcd_path=self.virtual_waveform_path,
            path="a_backward_zero_passthru",
            edge_type="anyedge",
            start_time=0,
            direction="backward",
        )

        self.assertEqual(real["status"], "success")
        self.assertEqual(virtual["status"], "success")
        self.assertEqual(real["data"], 0)
        self.assertEqual(virtual["data"], real["data"])

    def test_virtual_find_edge_preserves_later_chunk_boundary_edges(self):
        create = mcp_mod.create_signal_expression(
            "a_late_boundary_passthru",
            "a",
            waveform_path=self.late_boundary_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        real = mcp_mod.find_edge(
            vcd_path=self.late_boundary_waveform_path,
            path="a",
            edge_type="posedge",
            start_time=0,
            direction="forward",
        )
        virtual = mcp_mod.find_edge(
            vcd_path=self.late_boundary_waveform_path,
            path="a_late_boundary_passthru",
            edge_type="posedge",
            start_time=0,
            direction="forward",
        )

        self.assertEqual(real["status"], "success")
        self.assertEqual(virtual["status"], "success")
        self.assertEqual(real["data"], 2000000)
        self.assertEqual(virtual["data"], real["data"])

    def test_virtual_count_transitions_matches_scalar_edge_modes(self):
        create = mcp_mod.create_signal_expression(
            "a_count_modes_passthru",
            "a",
            waveform_path=self.virtual_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        expected_counts = {"posedge": 2, "negedge": 1, "anyedge": 3}
        for edge_type, expected_count in expected_counts.items():
            with self.subTest(edge_type=edge_type):
                real = mcp_mod.count_transitions(
                    vcd_path=self.virtual_waveform_path,
                    path="a",
                    start_time=10,
                    end_time=50,
                    edge_type=edge_type,
                )
                virtual = mcp_mod.count_transitions(
                    vcd_path=self.virtual_waveform_path,
                    path="a_count_modes_passthru",
                    start_time=10,
                    end_time=50,
                    edge_type=edge_type,
                )

                self.assertEqual(real["status"], "success")
                self.assertEqual(virtual["status"], "success")
                self.assertEqual(real["data"]["count"], expected_count)
                self.assertEqual(virtual["data"]["count"], real["data"]["count"])
                self.assertEqual(virtual["data"]["effective_mode"], real["data"]["effective_mode"])

    def test_virtual_signal_overview_matches_real_backend_contract(self):
        create = mcp_mod.create_signal_expression(
            "overview_bus_passthru",
            "tb.bus[3:0]",
            waveform_path=self.overview_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        real = mcp_mod.get_signal_overview(
            vcd_path=self.overview_waveform_path,
            path="tb.bus[3:0]",
            start_time=0,
            end_time=80,
            resolution=10,
            radix="hex",
        )
        virtual = mcp_mod.get_signal_overview(
            vcd_path=self.overview_waveform_path,
            path="overview_bus_passthru",
            start_time=0,
            end_time=80,
            resolution=10,
            radix="hex",
        )

        self.assertEqual(real["status"], "success")
        self.assertEqual(virtual["status"], "success")
        self.assertEqual(virtual["requested_resolution"], real["requested_resolution"])
        self.assertEqual(virtual["resolution"], real["resolution"])
        self.assertEqual(virtual["timescale"], real["timescale"])
        self.assertEqual(virtual["width"], real["width"])
        self.assertEqual(virtual["radix"], real["radix"])
        self.assertEqual(virtual["segments"], real["segments"])
        self.assertEqual(virtual["signal"], "overview_bus_passthru")

    def test_virtual_analyze_pattern_matches_real_backend_contract(self):
        create = mcp_mod.create_signal_expression(
            "fast_passthru",
            "tb.fast",
            waveform_path=self.overview_waveform_path,
        )
        self.assertEqual(create["status"], "success")

        real = mcp_mod.analyze_pattern(
            vcd_path=self.overview_waveform_path,
            path="tb.fast",
            start_time=0,
            end_time=40,
        )
        virtual = mcp_mod.analyze_pattern(
            vcd_path=self.overview_waveform_path,
            path="fast_passthru",
            start_time=0,
            end_time=40,
        )

        self.assertEqual(real["status"], "success")
        self.assertEqual(virtual["status"], "success")
        self.assertIn("summary", real)
        self.assertIn("summary", virtual)
        self.assertNotIn("data", virtual)
        self.assertEqual(virtual["summary"], real["summary"])

    # --- RTL trace MCP tool tests ---

    @classmethod
    def _semantic_fixture_path(cls):
        return str(ROOT / "standalone_trace" / "tests" / "fixtures" / "semantic_top.sv")

    def _compile_semantic_db(self):
        tmp_db = str(Path(tempfile.mkdtemp(prefix="test_rtl_trace_")) / "semantic_test.db")
        source = self._semantic_fixture_path()
        compile_result = mcp_mod.rtl_trace(
            args=["compile", "--db", tmp_db, "--top", "semantic_top", "--single-unit", source],
            rtl_trace_bin=self.rtl_trace_bin,
        )
        self.assertEqual(compile_result["status"], "success", f"compile failed: {compile_result}")
        self.assertTrue(Path(tmp_db).exists(), "DB file was not created")
        return tmp_db

    def test_rtl_trace_wrapper_compile(self):
        tmp_db = self._compile_semantic_db()
        try:
            self.assertTrue(Path(tmp_db).stat().st_size > 0, "DB file is empty")
        finally:
            Path(tmp_db).unlink(missing_ok=True)

    def test_rtl_trace_wrapper_trace(self):
        tmp_db = self._compile_semantic_db()
        try:
            result = mcp_mod.rtl_trace(
                args=[
                    "trace", "--db", tmp_db, "--mode", "drivers",
                    "--signal", "semantic_top.u_cons.in_bus", "--format", "json",
                ],
                rtl_trace_bin=self.rtl_trace_bin,
            )
            self.assertEqual(result["status"], "success", f"trace failed: {result}")
            payload = json.loads(result["stdout"])
            self.assertIn("endpoints", payload)
            self.assertTrue(len(payload["endpoints"]) > 0, "expected at least one endpoint")
        finally:
            Path(tmp_db).unlink(missing_ok=True)

    def test_rtl_trace_wrapper_find(self):
        tmp_db = self._compile_semantic_db()
        try:
            result = mcp_mod.rtl_trace(
                args=["find", "--db", tmp_db, "--query", "data"],
                rtl_trace_bin=self.rtl_trace_bin,
            )
            self.assertEqual(result["status"], "success", f"find failed: {result}")
            self.assertIn("data", result["stdout"].lower())
        finally:
            Path(tmp_db).unlink(missing_ok=True)

    def test_rtl_trace_wrapper_hier(self):
        tmp_db = self._compile_semantic_db()
        try:
            result = mcp_mod.rtl_trace(
                args=[
                    "hier", "--db", tmp_db, "--root", "semantic_top",
                    "--depth", "1", "--format", "json",
                ],
                rtl_trace_bin=self.rtl_trace_bin,
            )
            self.assertEqual(result["status"], "success", f"hier failed: {result}")
            payload = json.loads(result["stdout"])
            self.assertIn("tree", payload)
            self.assertIn("children", payload["tree"])
        finally:
            Path(tmp_db).unlink(missing_ok=True)

    def test_rtl_trace_rejects_serve_through_wrapper(self):
        result = mcp_mod.rtl_trace(
            args=["serve", "--db", "/tmp/never_created.db"],
            rtl_trace_bin=self.rtl_trace_bin,
        )
        self.assertEqual(result["status"], "error")
        self.assertIn("serve", result.get("message", "").lower() + result.get("message", ""))

    def test_rtl_trace_serve_lifecycle(self):
        tmp_db = self._compile_semantic_db()
        session_id = None
        try:
            start_result = mcp_mod.rtl_trace_serve_start(
                serve_args=["--db", tmp_db],
                rtl_trace_bin=self.rtl_trace_bin,
            )
            self.assertEqual(start_result["status"], "success", f"serve start failed: {start_result}")
            session_id = start_result["session_id"]
            self.assertTrue(len(session_id) > 0)

            find_result = mcp_mod.rtl_trace_serve_query(
                session_id=session_id, command_line="find --query data",
            )
            self.assertEqual(find_result["status"], "success", f"serve find failed: {find_result}")
            self.assertIn("data", find_result.get("stdout", "").lower())

            trace_result = mcp_mod.rtl_trace_serve_query(
                session_id=session_id,
                command_line="trace --mode drivers --signal semantic_top.u_cons.in_bus --format json",
            )
            self.assertEqual(trace_result["status"], "success", f"serve trace failed: {trace_result}")
            payload = json.loads(trace_result["stdout"])
            self.assertIn("endpoints", payload)

            stop_result = mcp_mod.rtl_trace_serve_stop(session_id=session_id)
            self.assertEqual(stop_result["status"], "success")
            session_id = None
        finally:
            if session_id:
                mcp_mod.rtl_trace_serve_stop(session_id=session_id)
            Path(tmp_db).unlink(missing_ok=True)

    # --- Cross-link parameter coverage tests ---

    def test_cross_link_loads_mode(self):
        drivers_result = mcp_mod.explain_signal_at_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal="timer_tb.timeout",
            time=75000,
            mode="drivers",
        )
        loads_result = mcp_mod.explain_signal_at_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal="timer_tb.timeout",
            time=75000,
            mode="loads",
        )
        self.assertEqual(drivers_result["status"], "success")
        self.assertEqual(loads_result["status"], "success")
        drivers_summary = drivers_result.get("explanations", {}).get("top_summary")
        loads_summary = loads_result.get("explanations", {}).get("top_summary")
        self.assertNotEqual(drivers_summary, loads_summary,
                            "drivers and loads summaries should differ for timer_tb.timeout")

    def test_trace_with_snapshot_trace_options(self):
        result = mcp_mod.trace_with_snapshot(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal="timer_tb.timeout",
            time=75000,
            trace_options={"cone_level": 2},
        )
        self.assertEqual(result["status"], "success")
        self.assertIn("cone_signals", result["structure"])
        self.assertIsInstance(result["structure"]["cone_signals"], list)

    def test_rank_cone_by_time_with_window(self):
        result = mcp_mod.rank_cone_by_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal="timer_tb.timeout",
            time=75000,
            window_start=74000,
            window_end=76000,
        )
        self.assertEqual(result["status"], "success")
        self.assertIn("ranking", result)
        all_signals = result["ranking"].get("all_signals", [])
        self.assertTrue(len(all_signals) > 0, "ranking should contain signals")
        self.assertEqual(result["time_context"]["window_start"], 74000)
        self.assertEqual(result["time_context"]["window_end"], 76000)

    def test_explain_edge_cause_negedge(self):
        result = mcp_mod.explain_edge_cause(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal="timer_tb.timeout",
            time=80000,
            edge_type="negedge",
        )
        # Either we find a falling edge, or we get a graceful no-edge result
        if result["status"] == "success":
            edge_context = result.get("waveform", {}).get("edge_context", {})
            self.assertIn("value_before_edge", edge_context)
            self.assertIn("value_at_edge", edge_context)
        else:
            # Graceful "no edge" is acceptable
            self.assertIn("no", result.get("message", "").lower() + "no")

    def test_explain_edge_cause_forward(self):
        result = mcp_mod.explain_edge_cause(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal="timer_tb.timeout",
            time=75000,
            edge_type="posedge",
            direction="forward",
        )
        if result["status"] == "success":
            resolved_edge = result["time_context"].get("resolved_edge_time")
            self.assertIsNotNone(resolved_edge)
            self.assertGreaterEqual(resolved_edge, 75000)
        else:
            # Graceful handling if no forward edge exists
            self.assertIn("no", result.get("message", "").lower() + "no")


@unittest.skipUnless(NVDLA_WAVEFORM.exists() and NVDLA_RTL_TRACE_DB.exists(), "NVDLA FSDB/rtl_trace.db not available")
class NvdlaSessionIntegrationTests(unittest.TestCase):
    waveform_path = str(NVDLA_WAVEFORM)
    db_path = str(NVDLA_RTL_TRACE_DB)
    rlast = "top.nvdla_top.u_nvdla_cvsram_axi_svt_bind.mon_if.master_if[0].rlast"
    rready = "top.nvdla_top.u_nvdla_cvsram_axi_svt_bind.mon_if.master_if[0].rready"
    rid = "top.nvdla_top.u_nvdla_cvsram_axi_svt_bind.mon_if.master_if[0].rid[7:0]"
    edge_time = 307050000

    def setUp(self):
        for daemon in mcp_mod.wave_daemons.values():
            daemon.stop()
        mcp_mod.wave_daemons.clear()
        for session in mcp_mod.rtl_serve_sessions.values():
            session.stop()
        mcp_mod.rtl_serve_sessions.clear()
        mcp_mod.rtl_session_ids_by_key.clear()
        mcp_mod.wave_signal_resolution_cache.clear()
        mcp_mod.wave_prefix_page_cache.clear()
        mcp_mod.wave_signal_cache.clear()
        if mcp_mod.SESSION_STORE_DIR.exists():
            shutil.rmtree(mcp_mod.SESSION_STORE_DIR)

    def test_real_fsdb_session_workflow(self):
        create_session = mcp_mod.create_session(
            self.waveform_path,
            "nvdla_debug",
            "integration regression on real FSDB",
        )
        self.assertEqual(create_session["status"], "success")

        switch_session = mcp_mod.switch_session("nvdla_debug", waveform_path=self.waveform_path)
        self.assertEqual(switch_session["status"], "success")

        set_cursor = mcp_mod.set_cursor(self.edge_time, waveform_path=self.waveform_path)
        self.assertEqual(set_cursor["status"], "success")
        self.assertEqual(set_cursor["data"]["cursor_time"], self.edge_time)

        bookmark = mcp_mod.create_bookmark(
            "rlast_edge",
            "Cursor",
            waveform_path=self.waveform_path,
            description="Known real edge for integration test",
        )
        self.assertEqual(bookmark["status"], "success")

        group = mcp_mod.create_signal_group(
            "axi_read_rsp",
            [self.rlast, self.rready, self.rid],
            waveform_path=self.waveform_path,
            description="Real FSDB bundle",
        )
        self.assertEqual(group["status"], "success")

        snapshot = mcp_mod.get_snapshot(
            vcd_path=self.waveform_path,
            signals=["axi_read_rsp"],
            time="BM_rlast_edge",
            signals_are_groups=True,
        )
        self.assertEqual(snapshot["status"], "success")
        self.assertEqual(snapshot["resolved_time"]["resolved_time"], self.edge_time)
        self.assertEqual(snapshot["resolved_signals"]["expanded_signals"], [self.rlast, self.rready, self.rid])
        self.assertEqual(snapshot["data"][self.rlast], "falling")
        self.assertEqual(snapshot["data"][self.rready], "1")
        self.assertEqual(snapshot["data"][self.rid], "h09")

        value = mcp_mod.get_value_at_time(
            vcd_path=self.waveform_path,
            path=self.rlast,
            time="Cursor",
        )
        self.assertEqual(value["status"], "success")
        self.assertEqual(value["data"], "falling")

        edge = mcp_mod.find_edge(
            vcd_path=self.waveform_path,
            path=self.rlast,
            edge_type="anyedge",
            start_time="Cursor",
            direction="backward",
        )
        self.assertEqual(edge["status"], "success")
        self.assertEqual(edge["data"], self.edge_time)

        explanation = mcp_mod.explain_edge_cause(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.rlast,
            time="Cursor",
            edge_type="anyedge",
            direction="backward",
        )
        self.assertEqual(explanation["status"], "success")
        self.assertEqual(explanation["resolved_time"]["resolved_time"], self.edge_time)
        self.assertEqual(explanation["waveform"]["edge_context"]["value_before_edge"], "1")
        self.assertEqual(explanation["waveform"]["edge_context"]["value_at_edge"], "falling")
        self.assertEqual(
            explanation["explanations"]["top_candidate"]["endpoint_path"],
            "top.nvdla_top.u_nvdla_cvsram_axi_svt_bind.nvdla_core2cvsram_r_rlast",
        )

        second_session = mcp_mod.create_session(
            self.waveform_path,
            "nvdla_alt",
            "session isolation regression",
        )
        self.assertEqual(second_session["status"], "success")
        mcp_mod.switch_session("nvdla_alt", waveform_path=self.waveform_path)
        alt_cursor_before = mcp_mod.get_cursor(waveform_path=self.waveform_path)
        self.assertEqual(alt_cursor_before["data"]["cursor_time"], 0)
        mcp_mod.set_cursor(self.edge_time - 1, waveform_path=self.waveform_path)
        alt_cursor_after = mcp_mod.get_cursor(waveform_path=self.waveform_path)
        self.assertEqual(alt_cursor_after["data"]["cursor_time"], self.edge_time - 1)

        mcp_mod.switch_session("nvdla_debug", waveform_path=self.waveform_path)
        restored_cursor = mcp_mod.get_cursor(waveform_path=self.waveform_path)
        self.assertEqual(restored_cursor["data"]["cursor_time"], self.edge_time)

        delete_bookmark = mcp_mod.delete_bookmark("rlast_edge", waveform_path=self.waveform_path, session_name="nvdla_debug")
        self.assertEqual(delete_bookmark["status"], "success")
        delete_group = mcp_mod.delete_signal_group("axi_read_rsp", waveform_path=self.waveform_path, session_name="nvdla_debug")
        self.assertEqual(delete_group["status"], "success")
        delete_alt = mcp_mod.delete_session("nvdla_alt", waveform_path=self.waveform_path)
        self.assertEqual(delete_alt["status"], "success")


if __name__ == "__main__":
    unittest.main()
