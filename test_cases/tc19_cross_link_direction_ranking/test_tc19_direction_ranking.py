#!/usr/bin/env python3
"""
Test Case 19: Cross-Link Direction-Aware Ranking
Phase 4: Direction-Aware Ranking

Validates that drivers and loads modes produce different directional priorities.
"""

import os
import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class DirectionAwareRankingTests(unittest.TestCase):
    """Phase 4: Direction-Aware Ranking"""

    @classmethod
    def setUpClass(cls):
        """Set up test fixtures."""
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
        """Clear caches before each test."""
        mcp_mod.wave_signal_resolution_cache.clear()
        mcp_mod.wave_prefix_page_cache.clear()
        mcp_mod.wave_signal_cache.clear()

    def test_4_1_rank_cone_drivers(self):
        """Test 4.1: rank_cone_by_time drivers"""
        print("\n[Test 4.1] rank_cone_by_time drivers")

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
        self.assertTrue(len(all_signals) > 0, "all_signals is empty")

        # Check first 5 entries for required fields
        for i, entry in enumerate(all_signals[:5]):
            required_fields = [
                "closest_transition_time", "closest_transition_distance",
                "closest_transition_direction", "preferred_transition_side",
                "used_preferred_transition_side", "closeness_score"
            ]
            for field in required_fields:
                self.assertIn(field, entry, f"Missing {field} in entry {i}")

            # drivers should prefer at_or_before
            self.assertEqual(entry.get("preferred_transition_side"), "at_or_before",
                           f"drivers mode should have preferred_transition_side='at_or_before'")

        print(f"  all_signals count: {len(all_signals)}")
        print(f"  [PASS] Test 4.1: rank_cone_by_time drivers")

    def test_4_2_rank_cone_loads(self):
        """Test 4.2: rank_cone_by_time loads"""
        print("\n[Test 4.2] rank_cone_by_time loads")

        result = mcp_mod.rank_cone_by_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            mode="loads",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(result.get("status"), "success")

        ranking = result.get("ranking", {})
        all_signals = ranking.get("all_signals", [])
        self.assertTrue(len(all_signals) > 0, "all_signals is empty")

        # Check first 5 entries for required fields
        for i, entry in enumerate(all_signals[:5]):
            required_fields = [
                "closest_transition_time", "closest_transition_distance",
                "closest_transition_direction", "preferred_transition_side",
                "used_preferred_transition_side", "closeness_score"
            ]
            for field in required_fields:
                self.assertIn(field, entry, f"Missing {field} in entry {i}")

            # loads should prefer at_or_after
            self.assertEqual(entry.get("preferred_transition_side"), "at_or_after",
                           f"loads mode should have preferred_transition_side='at_or_after'")

        print(f"  all_signals count: {len(all_signals)}")
        print(f"  [PASS] Test 4.2: rank_cone_by_time loads")

    def test_4_3_explain_drivers_vs_loads(self):
        """Test 4.3: explain_signal_at_time drivers vs loads"""
        print("\n[Test 4.3] explain_signal_at_time drivers vs loads")

        # Run drivers mode
        drivers_result = mcp_mod.explain_signal_at_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            mode="drivers",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(drivers_result.get("status"), "success")

        # Run loads mode
        loads_result = mcp_mod.explain_signal_at_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            mode="loads",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(loads_result.get("status"), "success")

        # Compare top summaries
        drivers_summary = drivers_result.get("explanations", {}).get("top_summary")
        loads_summary = loads_result.get("explanations", {}).get("top_summary")

        print(f"  drivers top_summary: {drivers_summary}")
        print(f"  loads top_summary: {loads_summary}")

        # They should not be identical -- drivers and loads traverse different directions
        if drivers_summary and loads_summary:
            self.assertNotEqual(drivers_summary, loads_summary,
                                "drivers and loads top_summary should differ for clk signal")

        print(f"  [PASS] Test 4.3: explain_signal_at_time drivers vs loads")


if __name__ == "__main__":
    unittest.main(verbosity=2)
