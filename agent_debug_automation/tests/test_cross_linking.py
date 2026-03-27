import importlib
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
NVDLA_WAVEFORM = Path("/home/qsun/DVT/nvdla/hw/verif/sim_vip/cc_alexnet_conv5_relu5_int16_dtest_cvsram/wave.fsdb")
NVDLA_RTL_TRACE_DB = Path("/home/qsun/DVT/nvdla/hw/verif/sim_vip/rtl_trace.db")
sys.path.insert(0, str(ROOT))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class CrossLinkingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db_path = "/tmp/agent_debug_automation_test.db"
        cls.waveform_path = str(ROOT / "waveform_explorer" / "timer_tb.vcd")
        cls.rtl_trace_bin = str(ROOT / "standalone_trace" / "build" / "rtl_trace")
        cls.wave_cli_bin = str(ROOT / "waveform_explorer" / "build" / "wave_agent_cli")
        fixture_dir = ROOT / "standalone_trace" / "test_case0"
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
