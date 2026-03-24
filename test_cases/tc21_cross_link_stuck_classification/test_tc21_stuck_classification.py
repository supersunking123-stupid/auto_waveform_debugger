#!/usr/bin/env python3
"""
Test Case 21: Cross-Link Stuck Classification
Phase 6: Stuck Classification Validation
"""

import os
import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class StuckClassificationTests(unittest.TestCase):
    """Phase 6: Stuck Classification Validation"""

    @classmethod
    def setUpClass(cls):
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.root_dir = Path(__file__).resolve().parents[1]
        cls.db_path = str(cls.root_dir / "rtl_trace.db")
        cls.waveform_path = str(cls.root_dir / "wave.fsdb")
        cls.rtl_trace_bin = str(cls.root_dir.parent / "standalone_trace" / "build" / "rtl_trace")
        cls.wave_cli_bin = str(cls.root_dir.parent / "waveform_explorer" / "build" / "wave_agent_cli")
        cls.signal = "top.mem0_rd_bw_mon.ready_in"
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

    def test_6_1_stuck_classification(self):
        """Test 6.1: Stuck classification validation"""
        print("\n[Test 6.1] Stuck classification validation")

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
        most_stuck = ranking.get("most_stuck_in_window", [])

        print(f"\n  most_stuck_in_window entries: {len(most_stuck)}")

        if not most_stuck:
            print("  [WARN] No stuck signals found in cone")
            # Try alternate signal
            print("  Trying alternate signal: top.mem0_rd_bw_mon.valid_in")
            result = mcp_mod.rank_cone_by_time(
                db_path=self.db_path,
                waveform_path=self.waveform_path,
                signal="top.mem0_rd_bw_mon.valid_in",
                time=self.time,
                mode="drivers",
                rtl_trace_bin=self.rtl_trace_bin,
                wave_cli_bin=self.wave_cli_bin,
            )
            ranking = result.get("ranking", {})
            most_stuck = ranking.get("most_stuck_in_window", [])
            print(f"  most_stuck_in_window entries (alternate): {len(most_stuck)}")

        # Check stuck class fields
        stuck_classes_found = set()
        for i, entry in enumerate(most_stuck[:5]):
            self.assertIn("is_constant_in_window", entry)
            self.assertIn("stuck_class", entry)
            self.assertIn("stuck_score", entry)
            stuck_classes_found.add(entry.get("stuck_class"))
            print(f"    [{i}] {entry.get('signal')}: "
                  f"stuck_class={entry.get('stuck_class')}, "
                  f"stuck_score={entry.get('stuck_score')}")

        print(f"\n  Stuck classes found: {stuck_classes_found}")

        # Verify ranking policy if multiple stuck classes present
        if len(most_stuck) >= 2:
            # Higher stuck_score should come first
            for i in range(len(most_stuck) - 1):
                self.assertGreaterEqual(
                    most_stuck[i].get("stuck_score", 0),
                    most_stuck[i + 1].get("stuck_score", 0),
                    "Stuck signals should be ranked by stuck_score descending"
                )

        print(f"  [PASS] Test 6.1: Stuck classification validation")


if __name__ == "__main__":
    unittest.main(verbosity=2)
