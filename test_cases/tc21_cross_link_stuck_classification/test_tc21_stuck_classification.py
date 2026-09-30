#!/usr/bin/env python3
"""
Test Case 21: Cross-Link Stuck Classification
Phase 6: Stuck Classification Validation
"""

import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))
sys.path.insert(0, str(ROOT_DIR / "test_cases"))

import cross_link_common as fx  # noqa: E402
from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class StuckClassificationTests(unittest.TestCase):
    """Phase 6: Stuck Classification Validation"""

    @classmethod
    def setUpClass(cls):
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.root_dir = Path(__file__).resolve().parents[1]
        cls.db_path = fx.DB_PATH
        cls.waveform_path = fx.WAVE_PATH
        cls.rtl_trace_bin = fx.RTL_TRACE_BIN
        cls.wave_cli_bin = fx.WAVE_CLI_BIN
        cls.signal = fx.READY
        cls.time = fx.T_REF
        cls.stuck_time = fx.T_STUCK

        # Build the fixture on demand (no-op when up to date)
        fx.ensure_fixture()

    def setUp(self):
        mcp_mod.wave_signal_resolution_cache.clear()
        mcp_mod.wave_prefix_page_cache.clear()
        mcp_mod.wave_signal_cache.clear()

    def test_6_1_stuck_classification(self):
        """Test 6.1: Stuck classification validation"""
        print("\n[Test 6.1] Stuck classification validation")

        # Inside the stall window ready_in is stuck at 0, the FIFO `full` flag
        # and the `cfg_en` control are stuck at 1, and `flush` is stuck at 0.
        result = mcp_mod.rank_cone_by_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.stuck_time,
            mode="drivers",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(result.get("status"), "success")

        ranking = result.get("ranking", {})
        most_stuck = ranking.get("most_stuck_in_window", [])

        print(f"\n  most_stuck_in_window entries: {len(most_stuck)}")
        self.assertTrue(most_stuck, "No stuck signals found in the stall window")

        # Check stuck class fields
        stuck_by_signal = {}
        stuck_classes_found = set()
        for i, entry in enumerate(most_stuck):
            self.assertIn("is_constant_in_window", entry)
            self.assertIn("stuck_class", entry)
            self.assertIn("stuck_score", entry)
            stuck_classes_found.add(entry.get("stuck_class"))
            stuck_by_signal[entry.get("signal")] = entry
            if i < 5:
                print(f"    [{i}] {entry.get('signal')}: "
                      f"stuck_class={entry.get('stuck_class')}, "
                      f"stuck_score={entry.get('stuck_score')}")

        print(f"\n  Stuck classes found: {stuck_classes_found}")

        # Both stuck-at-1 and stuck-at-0 classes are present in this cone.
        self.assertIn("stuck_to_1", stuck_classes_found)
        self.assertIn("stuck_to_0", stuck_classes_found)
        # The handshake signal itself is stuck low; the control inputs are stuck as designed.
        self.assertEqual(stuck_by_signal[fx.READY]["stuck_class"], "stuck_to_0")
        self.assertEqual(stuck_by_signal["top.dut.u_fifo.full"]["stuck_class"], "stuck_to_1")
        self.assertEqual(stuck_by_signal["top.dut.u_fifo.cfg_en"]["stuck_class"], "stuck_to_1")
        self.assertEqual(stuck_by_signal["top.dut.u_fifo.flush"]["stuck_class"], "stuck_to_0")

        # Higher stuck_score should come first
        scores = [entry.get("stuck_score", 0) for entry in most_stuck]
        self.assertEqual(scores, sorted(scores, reverse=True),
                         "Stuck signals should be ranked by stuck_score descending")

        # Outside the stall window the same handshake signal is NOT stuck.
        active = mcp_mod.rank_cone_by_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            mode="drivers",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(active.get("status"), "success")
        active_stuck = {e.get("signal") for e in active.get("ranking", {}).get("most_stuck_in_window", [])}
        self.assertNotIn(fx.READY, active_stuck,
                         "ready_in toggles at the reference time and must not be reported stuck")

        print(f"  [PASS] Test 6.1: Stuck classification validation")


if __name__ == "__main__":
    unittest.main(verbosity=2)
