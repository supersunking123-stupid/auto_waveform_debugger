#!/usr/bin/env python3
"""
Test Case 16: Cross-Link Backend Sanity Tests
Phase 1: Backend Sanity

Validates the backend infrastructure before running cross-link tests.
"""

import json
import os
import sys
import time
import unittest
from pathlib import Path

# Add project root to path
ROOT_DIR = Path(__file__).resolve().parents[2]  # Go to project root
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class BackendSanityTests(unittest.TestCase):
    """Phase 1: Backend Sanity Tests"""

    @classmethod
    def setUpClass(cls):
        """Set up test fixtures."""
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.root_dir = Path(__file__).resolve().parents[1]  # test_cases directory
        cls.db_path = str(cls.root_dir / "rtl_trace.db")
        cls.waveform_path = str(cls.root_dir / "wave.fsdb")
        cls.rtl_trace_bin = str(cls.root_dir.parent / "standalone_trace" / "build" / "rtl_trace")
        cls.wave_cli_bin = str(cls.root_dir.parent / "waveform_explorer" / "build" / "wave_agent_cli")

        # Known-good reference point
        cls.clock_signal = "top.mem0_rd_bw_mon.clk"
        cls.clock_edge_time = 399970000

        # Verify assets exist
        if not os.path.exists(cls.db_path):
            raise FileNotFoundError(f"RTL DB not found: {cls.db_path}")
        if not os.path.exists(cls.waveform_path):
            raise FileNotFoundError(f"Waveform not found: {cls.waveform_path}")
        if not os.path.exists(cls.rtl_trace_bin):
            raise FileNotFoundError(f"rtl_trace binary not found: {cls.rtl_trace_bin}")
        if not os.path.exists(cls.wave_cli_bin):
            raise FileNotFoundError(f"wave_agent_cli binary not found: {cls.wave_cli_bin}")

    def setUp(self):
        """Clear caches before each test."""
        mcp_mod.wave_signal_resolution_cache.clear()
        mcp_mod.wave_prefix_page_cache.clear()
        mcp_mod.wave_signal_cache.clear()

    def test_1_1_exact_edge_waveform_check(self):
        """Test 1.1: Exact-edge waveform check"""
        print("\n[Test 1.1] Exact-edge waveform check")

        # Test find_edge returns exact time
        edge_result = mcp_mod.find_edge(
            vcd_path=self.waveform_path,
            path=self.clock_signal,
            edge_type="posedge",
            start_time=self.clock_edge_time,
            direction="backward",
        )
        print(f"  find_edge result: {edge_result}")

        self.assertEqual(edge_result.get("status"), "success",
                        f"find_edge failed: {edge_result.get('message')}")
        edge_time = edge_result.get("data")
        self.assertEqual(edge_time, self.clock_edge_time,
                        f"Edge time mismatch: expected {self.clock_edge_time}, got {edge_time}")

        # Test value before edge is "0"
        value_before = mcp_mod.get_value_at_time(
            vcd_path=self.waveform_path,
            path=self.clock_signal,
            time=self.clock_edge_time - 1,
        )
        print(f"  value_before ({self.clock_edge_time - 1}): {value_before}")
        self.assertEqual(value_before.get("status"), "success")
        self.assertEqual(value_before.get("data"), "0",
                        f"Value before edge should be '0', got {value_before.get('data')}")

        # Test value at edge is "1"
        value_at = mcp_mod.get_value_at_time(
            vcd_path=self.waveform_path,
            path=self.clock_signal,
            time=self.clock_edge_time,
        )
        print(f"  value_at ({self.clock_edge_time}): {value_at}")
        self.assertEqual(value_at.get("status"), "success")
        self.assertEqual(value_at.get("data"), "1",
                        f"Value at edge should be '1', got {value_at.get('data')}")

        print(f"  [PASS] Test 1.1: Exact-edge waveform check")

    def test_1_2_structural_trace_sanity(self):
        """Test 1.2: Structural trace sanity"""
        print("\n[Test 1.2] Structural trace sanity")

        result = mcp_mod.rtl_trace(
            args=[
                "trace",
                "--db", self.db_path,
                "--mode", "drivers",
                "--signal", self.clock_signal,
                "--format", "json",
            ],
            rtl_trace_bin=self.rtl_trace_bin,
        )
        print(f"  rtl_trace status: {result.get('status')}")

        self.assertEqual(result.get("status"), "success",
                        f"rtl_trace failed: {result.get('stderr', result.get('message'))}")

        # Parse JSON output
        stdout = result.get("stdout", "").strip()
        self.assertTrue(stdout, "rtl_trace returned empty output")

        try:
            payload = json.loads(stdout)
        except json.JSONDecodeError as e:
            self.fail(f"rtl_trace returned invalid JSON: {e}")

        # Check required fields
        self.assertIn("target", payload, "Missing 'target' field")
        self.assertIn("mode", payload, "Missing 'mode' field")
        self.assertIn("summary", payload, "Missing 'summary' field")
        self.assertIn("endpoints", payload, "Missing 'endpoints' field")

        print(f"  target: {payload.get('target')}")
        print(f"  mode: {payload.get('mode')}")
        print(f"  endpoints count: {len(payload.get('endpoints', []))}")
        print(f"  [PASS] Test 1.2: Structural trace sanity")

    def test_1_3_fsdb_signal_info_sanity(self):
        """Test 1.3: FSDB signal-info sanity"""
        print("\n[Test 1.3] FSDB signal-info sanity")

        result = mcp_mod.get_signal_info(
            vcd_path=self.waveform_path,
            path=self.clock_signal,
        )
        print(f"  get_signal_info status: {result.get('status')}")

        self.assertEqual(result.get("status"), "success",
                        f"get_signal_info failed: {result.get('message')}")

        data = result.get("data", {})
        self.assertTrue(data, "No data returned")

        # Check required fields
        self.assertIn("path", data, "Missing 'path' field")
        self.assertIn("width", data, "Missing 'width' field")
        self.assertIn("type", data, "Missing 'type' field")
        self.assertIn("timescale", data, "Missing 'timescale' field")

        print(f"  path: {data.get('path')}")
        print(f"  width: {data.get('width')}")
        print(f"  type: {data.get('type')}")
        print(f"  timescale: {data.get('timescale')}")
        print(f"  [PASS] Test 1.3: FSDB signal-info sanity")


if __name__ == "__main__":
    unittest.main(verbosity=2)
