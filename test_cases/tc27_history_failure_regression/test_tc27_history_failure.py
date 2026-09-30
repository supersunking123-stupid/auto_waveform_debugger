#!/usr/bin/env python3
"""
Test Case 27: History Failure Regression

Verifies that previously failing waveform queries now work correctly
after the signal path resolution fix.

These commands previously failed because FSDB stored signals as packed-vector
paths (e.g., count[2:0]), while direct lookup only accepted exact names.
The resolver now falls back from bare hierarchical names to uniquely matching
packed-vector FSDB signals.

The original failure was reproduced on a large external design; this test
reproduces the same class of failure on the small regenerable cross-link
fixture (test_cases/make_fixture.sh). Expected values below are properties of
the fixture simulation, which is deterministic.
"""

import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))
sys.path.insert(0, str(ROOT_DIR / "test_cases"))

import cross_link_common as fx  # noqa: E402
from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class HistoryFailureRegressionTests(unittest.TestCase):
    """Regression tests for previously failing waveform queries"""

    @classmethod
    def setUpClass(cls):
        """Set up test fixtures."""
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.waveform_path = fx.WAVE_PATH
        cls.wave_cli_bin = fx.WAVE_CLI_BIN

        # Time window (ps): 100 ns of normal traffic, mid-simulation
        cls.start_time = 1000000
        cls.end_time = 1100000

        # Signal paths (bare names without packed suffixes)
        cls.signals = {
            "fifo_count": fx.FIFO_COUNT,       # stored as count[2:0]
            "fifo_wr_ptr": fx.FIFO_WR_PTR,     # stored as wr_ptr[1:0]
            "src_data": fx.SRC_DATA,           # stored as data[15:0]
            "src_id": fx.SRC_ID,               # stored as id[7:0]
        }

        # Build the fixture on demand (no-op when up to date)
        fx.ensure_fixture()

    def setUp(self):
        """Clear caches before each test."""
        mcp_mod.wave_signal_resolution_cache.clear()
        mcp_mod.wave_prefix_page_cache.clear()
        mcp_mod.wave_signal_cache.clear()

    def _check_first_transitions(self, key, expected_values):
        signal = self.signals[key]
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

        print(f"  First {min(len(expected_values), len(data))} transitions:")
        self.assertGreaterEqual(len(data), len(expected_values))
        for i, expected in enumerate(expected_values):
            value = data[i].get("v", "")
            print(f"    [{i}] {value}")
            self.assertEqual(value, expected,
                           f"Transition {i} value mismatch: expected {expected}, got {value}")

    def test_1_fifo_count_transitions(self):
        """Test 1: FIFO count transitions (previously failed)"""
        print("\n[Test 1] FIFO count transitions")
        self._check_first_transitions("fifo_count", ["b100", "b011", "b100"])
        print(f"  [PASS] Test 1: FIFO count transitions")

    def test_2_fifo_wr_ptr_transitions(self):
        """Test 2: FIFO write-pointer transitions (previously failed)"""
        print("\n[Test 2] FIFO wr_ptr transitions")
        self._check_first_transitions("fifo_wr_ptr", ["b11", "b00", "b01"])
        print(f"  [PASS] Test 2: FIFO wr_ptr transitions")

    def test_3_src_data_transitions(self):
        """Test 3: source LFSR data transitions (previously failed)"""
        print("\n[Test 3] source data transitions")
        self._check_first_transitions(
            "src_data",
            ["b1000001010010101", "b0000010100101011", "b0000101001010111"],
        )
        print(f"  [PASS] Test 3: source data transitions")

    def _check_snapshot(self, time, expected):
        signals = [self.signals["fifo_count"], self.signals["src_id"]]
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

        print(f"  Snapshot values:")
        for key, expected_options in expected.items():
            signal = self.signals[key]
            actual_value = data.get(signal)
            print(f"    {signal} = {actual_value}")
            # Accept hex and binary radix
            self.assertIn(actual_value, expected_options,
                           f"Snapshot value mismatch for {signal}: expected one of {expected_options}, got {actual_value}")

    def test_4_snapshot_1050000(self):
        """Test 4: snapshot at 1050000 (previously failed)"""
        print("\n[Test 4] snapshot at 1050000")
        self._check_snapshot(1050000, {
            "fifo_count": ("h4", "b100"),
            "src_id": ("h11", "b00010001"),
        })
        print(f"  [PASS] Test 4: snapshot at 1050000")

    def test_5_snapshot_1099000(self):
        """Test 5: snapshot at 1099000 (previously failed)"""
        print("\n[Test 5] snapshot at 1099000")
        self._check_snapshot(1099000, {
            "fifo_count": ("h3", "b011"),
            "src_id": ("h12", "b00010010"),
        })
        print(f"  [PASS] Test 5: snapshot at 1099000")

    def test_6_bare_name_resolves_to_packed_vector(self):
        """Test 6: bare hierarchical names map to the packed-vector FSDB path"""
        print("\n[Test 6] bare name -> packed vector mapping")
        expected = {
            "fifo_count": fx.FIFO_COUNT + "[2:0]",
            "fifo_wr_ptr": fx.FIFO_WR_PTR + "[1:0]",
            "src_data": fx.SRC_DATA + "[15:0]",
            "src_id": fx.SRC_ID + "[7:0]",
        }
        for key, packed in expected.items():
            mapped = mcp_mod._map_signal_to_waveform(
                self.waveform_path, self.signals[key], wave_cli_bin=self.wave_cli_bin
            )
            print(f"  {self.signals[key]} -> {mapped}")
            self.assertEqual(mapped, packed)
        print(f"  [PASS] Test 6: bare name -> packed vector mapping")


if __name__ == "__main__":
    unittest.main(verbosity=2)
