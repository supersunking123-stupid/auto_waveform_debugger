import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
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
        mcp_mod.wave_signal_resolution_cache.clear()
        mcp_mod.wave_prefix_page_cache.clear()
        mcp_mod.wave_signal_cache.clear()

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
        self.assertEqual(result["waveform"]["focus_samples"]["timer_tb.timeout"]["value"], "1")
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
        self.assertEqual(result["waveform"]["edge_context"]["value_at_edge"], "1")

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


if __name__ == "__main__":
    unittest.main()
