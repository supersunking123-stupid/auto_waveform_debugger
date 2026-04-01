#!/usr/bin/env python3
"""
Test Case 18: Cross-Link Exact-Edge Correctness Regression
Phase 3: Exact-Edge Correctness Regression

Mandatory correctness check for known-good clock edge.
"""

import os
import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class ExactEdgeCorrectnessTests(unittest.TestCase):
    """Phase 3: Exact-Edge Correctness Regression"""

    @classmethod
    def setUpClass(cls):
        """Set up test fixtures."""
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.root_dir = Path(__file__).resolve().parents[1]
        cls.db_path = str(cls.root_dir / "rtl_trace.db")
        cls.waveform_path = str(cls.root_dir / "wave.fsdb")
        cls.rtl_trace_bin = str(cls.root_dir.parent / "standalone_trace" / "build" / "rtl_trace")
        cls.wave_cli_bin = str(cls.root_dir.parent / "waveform_explorer" / "build" / "wave_agent_cli")

        # Known-good reference point (mandatory)
        cls.signal = "top.mem0_rd_bw_mon.clk"
        cls.time = 399970000
        cls.edge_type = "posedge"
        cls.direction = "backward"
        cls.mode = "drivers"

        # Verify assets
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

    def test_3_1_exact_edge_correctness(self):
        """Test 3.1: Exact-edge correctness regression (MANDATORY)"""
        print("\n[Test 3.1] Exact-edge correctness regression (MANDATORY)")

        result = mcp_mod.explain_edge_cause(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            edge_type=self.edge_type,
            direction=self.direction,
            mode=self.mode,
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        print(f"  status: {result.get('status')}")

        self.assertEqual(result.get("status"), "success",
                        f"explain_edge_cause failed: {result.get('message')}")

        # Check resolved_edge_time == 399970000
        time_context = result.get("time_context", {})
        resolved_edge_time = time_context.get("resolved_edge_time")
        print(f"  resolved_edge_time: {resolved_edge_time}")
        self.assertEqual(resolved_edge_time, self.time,
                        f"resolved_edge_time mismatch: expected {self.time}, got {resolved_edge_time}")

        # Check value_before_edge == "0"
        waveform = result.get("waveform", {})
        edge_context = waveform.get("edge_context", {})
        value_before = edge_context.get("value_before_edge")
        print(f"  value_before_edge: {value_before}")
        self.assertEqual(value_before, "0",
                        f"value_before_edge should be '0', got {value_before}")

        # Check value_at_edge == "1"
        value_at = edge_context.get("value_at_edge")
        print(f"  value_at_edge: {value_at}")
        self.assertIn(value_at, ("1", "rising"),
                        f"value_at_edge should be '1' or 'rising', got {value_at}")

        print(f"  [PASS] Test 3.1: Exact-edge correctness regression")


if __name__ == "__main__":
    unittest.main(verbosity=2)
