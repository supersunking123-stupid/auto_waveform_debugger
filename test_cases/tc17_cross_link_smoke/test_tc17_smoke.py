#!/usr/bin/env python3
"""
Test Case 17: Cross-Link Smoke Tests
Phase 2: Cross-Link Smoke Tests

Validates the four main cross-link tools with basic smoke tests.
"""

import json
import os
import sys
import unittest
from pathlib import Path

# Add project root to path
ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class CrossLinkSmokeTests(unittest.TestCase):
    """Phase 2: Cross-Link Smoke Tests"""

    @classmethod
    def setUpClass(cls):
        """Set up test fixtures."""
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.root_dir = Path(__file__).resolve().parents[1]
        cls.db_path = str(cls.root_dir / "rtl_trace.db")
        cls.waveform_path = str(cls.root_dir / "wave.fsdb")
        cls.rtl_trace_bin = str(cls.root_dir.parent / "standalone_trace" / "build" / "rtl_trace")
        cls.wave_cli_bin = str(cls.root_dir.parent / "waveform_explorer" / "build" / "wave_agent_cli")

        # Focus point for smoke tests
        cls.signal = "top.mem0_rd_bw_mon.ready_in"
        cls.time = 399970000
        cls.mode = "drivers"

        # Verify assets exist
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

    def _check_common_keys(self, result, test_name):
        """Check common top-level keys in cross-link results."""
        required_keys = [
            "status", "target", "time_context", "structure",
            "waveform", "ranking", "unmapped_signals", "warnings"
        ]
        for key in required_keys:
            self.assertIn(key, result, f"{test_name}: Missing '{key}' field")

    def test_2_1_trace_with_snapshot(self):
        """Test 2.1: trace_with_snapshot"""
        print("\n[Test 2.1] trace_with_snapshot")

        result = mcp_mod.trace_with_snapshot(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            mode=self.mode,
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        print(f"  status: {result.get('status')}")

        self.assertEqual(result.get("status"), "success",
                        f"trace_with_snapshot failed: {result.get('message')}")

        # Check common keys
        self._check_common_keys(result, "trace_with_snapshot")

        # Check structure details
        structure = result.get("structure", {})
        self.assertIn("trace", structure, "Missing structure.trace")
        self.assertIn("cone_signals", structure, "Missing structure.cone_signals")
        cone_signals = structure.get("cone_signals", [])
        self.assertTrue(len(cone_signals) > 0, "cone_signals is empty")

        # Check waveform details
        waveform = result.get("waveform", {})
        self.assertIn("focus_samples", waveform, "Missing waveform.focus_samples")

        print(f"  cone_signals count: {len(cone_signals)}")
        print(f"  [PASS] Test 2.1: trace_with_snapshot")

    def test_2_2_explain_signal_at_time(self):
        """Test 2.2: explain_signal_at_time"""
        print("\n[Test 2.2] explain_signal_at_time")

        result = mcp_mod.explain_signal_at_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            mode=self.mode,
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        print(f"  status: {result.get('status')}")

        self.assertEqual(result.get("status"), "success",
                        f"explain_signal_at_time failed: {result.get('message')}")

        # Check common keys
        self._check_common_keys(result, "explain_signal_at_time")

        # Check explanations
        self.assertIn("explanations", result, "Missing explanations")
        explanations = result.get("explanations", {})
        self.assertIn("candidate_paths", explanations, "Missing explanations.candidate_paths")

        candidate_paths = explanations.get("candidate_paths", [])
        top_candidate = explanations.get("top_candidate")
        top_summary = explanations.get("top_summary")

        # top_candidate can be null if no candidates, but top_summary should exist
        if candidate_paths:
            self.assertIsNotNone(top_candidate, "top_candidate should exist when candidates present")
            self.assertIsNotNone(top_summary, "top_summary should exist when candidates present")

        print(f"  candidate_paths count: {len(candidate_paths)}")
        print(f"  top_candidate: {top_candidate.get('endpoint_path') if top_candidate else 'null'}")
        print(f"  [PASS] Test 2.2: explain_signal_at_time")

    def test_2_3_rank_cone_by_time(self):
        """Test 2.3: rank_cone_by_time"""
        print("\n[Test 2.3] rank_cone_by_time")

        result = mcp_mod.rank_cone_by_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            mode=self.mode,
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        print(f"  status: {result.get('status')}")

        self.assertEqual(result.get("status"), "success",
                        f"rank_cone_by_time failed: {result.get('message')}")

        # Check common keys
        self._check_common_keys(result, "rank_cone_by_time")

        # Check ranking details
        ranking = result.get("ranking", {})
        required_ranking_keys = [
            "all_signals",
            "most_active_near_time",
            "most_stuck_in_window",
            "unchanged_candidates"
        ]
        for key in required_ranking_keys:
            self.assertIn(key, ranking, f"Missing ranking.{key}")

        print(f"  all_signals count: {len(ranking.get('all_signals', []))}")
        print(f"  most_active_near_time count: {len(ranking.get('most_active_near_time', []))}")
        print(f"  most_stuck_in_window count: {len(ranking.get('most_stuck_in_window', []))}")
        print(f"  [PASS] Test 2.3: rank_cone_by_time")

    def test_2_4_explain_edge_cause(self):
        """Test 2.4: explain_edge_cause"""
        print("\n[Test 2.4] explain_edge_cause")

        result = mcp_mod.explain_edge_cause(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            edge_type="anyedge",
            direction="backward",
            mode=self.mode,
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        print(f"  status: {result.get('status')}")

        # Edge cause may return WARN if no edge found
        if result.get("status") == "error":
            msg = result.get("message", "")
            if "no anyedge edge found" in msg:
                print(f"  [WARN] Test 2.4: No edge found for signal (expected for inactive signals)")
                return

        self.assertEqual(result.get("status"), "success",
                        f"explain_edge_cause failed: {result.get('message')}")

        # Check common keys
        self._check_common_keys(result, "explain_edge_cause")

        # Check time_context
        time_context = result.get("time_context", {})
        self.assertIn("requested_time", time_context, "Missing time_context.requested_time")
        self.assertIn("resolved_edge_time", time_context, "Missing time_context.resolved_edge_time")

        # Check waveform edge_context
        waveform = result.get("waveform", {})
        edge_context = waveform.get("edge_context", {})
        if edge_context:
            self.assertIn("value_before_edge", edge_context, "Missing edge_context.value_before_edge")
            self.assertIn("value_at_edge", edge_context, "Missing edge_context.value_at_edge")

        print(f"  requested_time: {time_context.get('requested_time')}")
        print(f"  resolved_edge_time: {time_context.get('resolved_edge_time')}")
        print(f"  [PASS] Test 2.4: explain_edge_cause")


if __name__ == "__main__":
    unittest.main(verbosity=2)
