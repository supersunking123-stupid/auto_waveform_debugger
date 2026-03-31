#!/usr/bin/env python3
"""
Test Case 20: Cross-Link Closeness-First Ranking
Phase 5: Closeness-First Ranking Validation
"""

import os
import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class ClosenessFirstRankingTests(unittest.TestCase):
    """Phase 5: Closeness-First Ranking Validation"""

    @classmethod
    def setUpClass(cls):
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.root_dir = Path(__file__).resolve().parents[1]
        cls.db_path = str(cls.root_dir / "rtl_trace.db")
        cls.waveform_path = str(cls.root_dir / "wave.fsdb")
        cls.rtl_trace_bin = str(cls.root_dir.parent / "standalone_trace" / "build" / "rtl_trace")
        cls.wave_cli_bin = str(cls.root_dir.parent / "waveform_explorer" / "build" / "wave_agent_cli")
        cls.signal = "top.mem0_rd_bw_mon.clk"
        cls.time = 399970000

        for path, name in [(cls.db_path, "rtl_trace.db"), 
                           (cls.waveform_path, "wave.fsdb"),
                           (cls.rtl_trace_bin, "rtl_trace"),
                           (cls.wave_cli_bin, "wave_agent_cli")]:
            if not os.path.exists(path):
                raise FileNotFoundError(f"{name} not found: {path}")

    def setUp(self):
        mcp_mod.wave_signal_resolution_cache.clear()
        mcp_mod.wave_prefix_page_cache.clear()
        mcp_mod.wave_signal_cache.clear()

    def test_5_1_closeness_first_ranking(self):
        """Test 5.1: Closeness-first ranking validation"""
        print("\n[Test 5.1] Closeness-first ranking validation")

        result = mcp_mod.rank_cone_by_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            mode="drivers",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(result.get("status"), "success")

        ranking = result.get("ranking", {})
        all_signals = ranking.get("all_signals", [])
        most_active = ranking.get("most_active_near_time", [])

        # Assert all_signals list is non-empty
        self.assertTrue(len(all_signals) > 0, "all_signals is empty")
        self.assertTrue(len(most_active) > 0, "most_active_near_time is empty")

        # Check first 5 entries
        print("\n  First 5 entries in all_signals:")
        for i, entry in enumerate(all_signals[:5]):
            print(f"    [{i}] {entry.get('signal')}: "
                  f"closeness={entry.get('closeness_score')}, "
                  f"activity={entry.get('activity_score')}, "
                  f"total={entry.get('total_score')}")

        print("\n  First 5 entries in most_active_near_time:")
        for i, entry in enumerate(most_active[:5]):
            print(f"    [{i}] {entry.get('signal')}: "
                  f"closeness={entry.get('closeness_score')}, "
                  f"closest_dist={entry.get('closest_transition_distance')}")

        # Assert closeness_score values are monotonically non-increasing in all_signals
        closeness_scores = [entry.get("closeness_score", 0) for entry in all_signals]
        for i in range(1, len(closeness_scores)):
            self.assertGreaterEqual(
                closeness_scores[i - 1], closeness_scores[i],
                f"closeness_score not non-increasing: [{i-1}]={closeness_scores[i-1]} > [{i}]={closeness_scores[i]}",
            )

        # Assert the first entry has the highest closeness_score
        first_entry = all_signals[0]
        self.assertIn("closeness_score", first_entry)
        self.assertEqual(first_entry["closeness_score"], max(closeness_scores),
                         "First all_signals entry should have the highest closeness_score")
        print(f"\n  Top ranked signal: {first_entry.get('signal')} "
              f"(closeness_score={first_entry.get('closeness_score')})")

        print(f"  [PASS] Test 5.1: Closeness-first ranking validation")


if __name__ == "__main__":
    unittest.main(verbosity=2)
