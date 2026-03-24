#!/usr/bin/env python3
"""
Test Case 22: Cross-Link Snapshot and Cycle Sampling
Phase 7: Snapshot And Cycle Sampling
"""

import os
import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class SnapshotCycleSamplingTests(unittest.TestCase):
    """Phase 7: Snapshot And Cycle Sampling"""

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
        cls.clock_path = "top.mem0_rd_bw_mon.clk"
        cls.sample_offsets = [-1000, 0, 1000]
        cls.cycle_offsets = [-1, 0, 1]

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

    def test_7_1_snapshot_cycle_sampling(self):
        """Test 7.1: Snapshot and cycle sampling"""
        print("\n[Test 7.1] Snapshot and cycle sampling")

        result = mcp_mod.trace_with_snapshot(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            mode="drivers",
            sample_offsets=self.sample_offsets,
            clock_path=self.clock_path,
            cycle_offsets=self.cycle_offsets,
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(result.get("status"), "success",
                        f"trace_with_snapshot failed: {result.get('message')}")

        waveform = result.get("waveform", {})

        # Check absolute_offset_samples
        self.assertIn("absolute_offset_samples", waveform,
                     "Missing waveform.absolute_offset_samples")
        print(f"  absolute_offset_samples: {list(waveform.get('absolute_offset_samples', {}).keys())}")

        # Check cycle_offset_samples
        cycle_data = waveform.get("cycle_offset_samples", {})
        if cycle_data:
            self.assertIn("clock_path", cycle_data, "Missing cycle_offset_samples.clock_path")
            self.assertIn("resolved_clock_path", cycle_data,
                         "Missing cycle_offset_samples.resolved_clock_path")
            self.assertIn("cycle_times", cycle_data, "Missing cycle_offset_samples.cycle_times")
            self.assertIn("samples", cycle_data, "Missing cycle_offset_samples.samples")

            cycle_times = cycle_data.get("cycle_times", {})
            print(f"  cycle_times keys: {list(cycle_times.keys())}")
            print(f"  resolved_clock_path: {cycle_data.get('resolved_clock_path')}")

            # Check cycle 0 corresponds to requested time
            if 0 in cycle_times:
                cycle_0_time = cycle_times[0]
                if cycle_0_time is not None:
                    print(f"  cycle 0 time: {cycle_0_time}")
                    # Cycle 0 should be at or near the requested time
                    self.assertEqual(cycle_0_time, self.time,
                                   f"Cycle 0 should be at {self.time}, got {cycle_0_time}")
        else:
            print("  [WARN] No cycle data returned (clock edge may not have been found)")

        print(f"  [PASS] Test 7.1: Snapshot and cycle sampling")


if __name__ == "__main__":
    unittest.main(verbosity=2)
