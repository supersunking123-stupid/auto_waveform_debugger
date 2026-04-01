#!/usr/bin/env python3
"""
Test Case 27: History Failure Regression

Verifies that previously failing waveform queries now work correctly
after the signal path resolution fix.

These commands previously failed because FSDB stored signals as packed-vector
paths (e.g., cq_rd_count9[8:0]), while direct lookup only accepted exact names.
The resolver now falls back from bare hierarchical names to uniquely matching
packed-vector FSDB signals.
"""

import os
import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class HistoryFailureRegressionTests(unittest.TestCase):
    """Regression tests for previously failing waveform queries"""

    @classmethod
    def setUpClass(cls):
        """Set up test fixtures."""
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.root_dir = Path(__file__).resolve().parents[1]
        cls.waveform_path = str(cls.root_dir / "wave.fsdb")
        cls.wave_cli_bin = str(cls.root_dir.parent / "waveform_explorer" / "build" / "wave_agent_cli")

        # Time window from original failure reproduction
        cls.start_time = 784000000
        cls.end_time = 800000000

        # Signal paths (bare names without packed suffixes)
        cls.base_path = "top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq"
        cls.signals = {
            "cq_rd_count9": f"{cls.base_path}.cq_rd_count9",
            "cq_rd9_credits": f"{cls.base_path}.cq_rd9_credits",
            "cq_rd_take_thread_id": f"{cls.base_path}.cq_rd_take_thread_id",
            "update_head_next": f"{cls.base_path}.update_head_next",
        }

        # Verify assets exist
        for path, name in [(cls.waveform_path, "wave.fsdb"),
                           (cls.wave_cli_bin, "wave_agent_cli")]:
            if not os.path.exists(path):
                raise FileNotFoundError(f"{name} not found: {path}")

    def setUp(self):
        """Clear caches before each test."""
        mcp_mod.wave_signal_resolution_cache.clear()
        mcp_mod.wave_prefix_page_cache.clear()
        mcp_mod.wave_signal_cache.clear()

    def test_1_cq_count_transitions(self):
        """Test 1: CQ count transitions (previously failed)"""
        print("\n[Test 1] CQ count transitions")

        signal = self.signals["cq_rd_count9"]
        print(f"  Signal: {signal}")
        print(f"  Time window: {self.start_time} - {self.end_time}")

        result = mcp_mod.get_transitions(
            vcd_path=self.waveform_path,
            path=signal,
            start_time=self.start_time,
            end_time=self.end_time,
            max_limit=5,
        )
        print(f"  Result status: {result.get('status')}")

        self.assertEqual(result.get("status"), "success",
                        f"get_transitions failed: {result.get('message')}")

        data = result.get("data", [])
        self.assertTrue(len(data) > 0, "No transitions returned")

        # Check expected transition values
        expected_values = ["b000111011", "b000111010", "b000111011"]
        print(f"  First {min(3, len(data))} transitions:")
        for i, transition in enumerate(data[:3]):
            value = transition.get("v", "")
            print(f"    [{i}] {value}")
            if i < len(expected_values):
                self.assertEqual(value, expected_values[i],
                               f"Transition {i} value mismatch: expected {expected_values[i]}, got {value}")

        print(f"  [PASS] Test 1: CQ count transitions")

    def test_2_cq_credits_transitions(self):
        """Test 2: CQ credits transitions (previously failed)"""
        print("\n[Test 2] CQ credits transitions")

        signal = self.signals["cq_rd9_credits"]
        print(f"  Signal: {signal}")
        print(f"  Time window: {self.start_time} - {self.end_time}")

        result = mcp_mod.get_transitions(
            vcd_path=self.waveform_path,
            path=signal,
            start_time=self.start_time,
            end_time=self.end_time,
            max_limit=5,
        )
        print(f"  Result status: {result.get('status')}")

        self.assertEqual(result.get("status"), "success",
                        f"get_transitions failed: {result.get('message')}")

        data = result.get("data", [])
        self.assertTrue(len(data) > 0, "No transitions returned")

        # Check expected transition values
        expected_values = ["b000111011", "b000111010", "b000111011"]
        print(f"  First {min(3, len(data))} transitions:")
        for i, transition in enumerate(data[:3]):
            value = transition.get("v", "")
            print(f"    [{i}] {value}")
            if i < len(expected_values):
                self.assertEqual(value, expected_values[i],
                               f"Transition {i} value mismatch: expected {expected_values[i]}, got {value}")

        print(f"  [PASS] Test 2: CQ credits transitions")

    def test_3_cq_take_thread_id_transitions(self):
        """Test 3: CQ take-thread-id transitions (previously failed)"""
        print("\n[Test 3] CQ take-thread-id transitions")

        signal = self.signals["cq_rd_take_thread_id"]
        print(f"  Signal: {signal}")
        print(f"  Time window: {self.start_time} - {self.end_time}")

        result = mcp_mod.get_transitions(
            vcd_path=self.waveform_path,
            path=signal,
            start_time=self.start_time,
            end_time=self.end_time,
            max_limit=5,
        )
        print(f"  Result status: {result.get('status')}")

        self.assertEqual(result.get("status"), "success",
                        f"get_transitions failed: {result.get('message')}")

        data = result.get("data", [])
        self.assertTrue(len(data) > 0, "No transitions returned")

        # Check expected transition values
        expected_values = ["b1001", "b0000", "b1001"]
        print(f"  First {min(3, len(data))} transitions:")
        for i, transition in enumerate(data[:3]):
            value = transition.get("v", "")
            print(f"    [{i}] {value}")
            if i < len(expected_values):
                self.assertEqual(value, expected_values[i],
                               f"Transition {i} value mismatch: expected {expected_values[i]}, got {value}")

        print(f"  [PASS] Test 3: CQ take-thread-id transitions")

    def test_4_cq_snapshot_784530000(self):
        """Test 4: CQ snapshot at 784530000 (previously failed)"""
        print("\n[Test 4] CQ snapshot at 784530000")

        time = 784530000
        signals = [
            self.signals["cq_rd_count9"],
            self.signals["update_head_next"],
        ]
        print(f"  Time: {time}")
        print(f"  Signals: {signals}")

        result = mcp_mod.get_snapshot(
            vcd_path=self.waveform_path,
            signals=signals,
            time=time,
        )
        print(f"  Result status: {result.get('status')}")

        self.assertEqual(result.get("status"), "success",
                        f"get_snapshot failed: {result.get('message')}")

        data = result.get("data", {})
        self.assertTrue(data, "No snapshot data returned")

        # Check expected values (accept "changing" for multi-bit signals at edge time)
        expected = {
            self.signals["cq_rd_count9"]: ("b000111011", "changing"),
            self.signals["update_head_next"]: ("b0000000000", "h000"),
        }

        print(f"  Snapshot values:")
        for signal, expected_options in expected.items():
            actual_value = data.get(signal)
            print(f"    {signal} = {actual_value}")
            self.assertIn(actual_value, expected_options,
                           f"Snapshot value mismatch for {signal}: expected one of {expected_options}, got {actual_value}")

        print(f"  [PASS] Test 4: CQ snapshot at 784530000")

    def test_5_cq_snapshot_799290000(self):
        """Test 5: CQ snapshot at 799290000 (previously failed)"""
        print("\n[Test 5] CQ snapshot at 799290000")

        time = 799290000
        signals = [
            self.signals["cq_rd_count9"],
            self.signals["update_head_next"],
        ]
        print(f"  Time: {time}")
        print(f"  Signals: {signals}")

        result = mcp_mod.get_snapshot(
            vcd_path=self.waveform_path,
            signals=signals,
            time=time,
        )
        print(f"  Result status: {result.get('status')}")

        self.assertEqual(result.get("status"), "success",
                        f"get_snapshot failed: {result.get('message')}")

        data = result.get("data", {})
        self.assertTrue(data, "No snapshot data returned")

        # Check expected values (accept hex and binary radix; "changing" for edge time)
        expected = {
            self.signals["cq_rd_count9"]: ("b000000000", "h000", "changing"),
            self.signals["update_head_next"]: ("b0000000000", "h000"),
        }

        print(f"  Snapshot values:")
        for signal, expected_options in expected.items():
            actual_value = data.get(signal)
            print(f"    {signal} = {actual_value}")
            self.assertIn(actual_value, expected_options,
                           f"Snapshot value mismatch for {signal}: expected one of {expected_options}, got {actual_value}")

        print(f"  [PASS] Test 5: CQ snapshot at 799290000")


if __name__ == "__main__":
    unittest.main(verbosity=2)
