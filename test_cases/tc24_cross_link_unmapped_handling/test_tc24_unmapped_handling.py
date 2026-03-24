#!/usr/bin/env python3
"""
Test Case 24: Cross-Link Unmapped Signal Handling
Phase 9: Unmapped Signal Handling
"""

import os
import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class UnmappedSignalHandlingTests(unittest.TestCase):
    """Phase 9: Unmapped Signal Handling"""

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

    def test_9_1_unmapped_signal_handling(self):
        """Test 9.1: Unmapped signal reporting"""
        print("\n[Test 9.1] Unmapped signal handling")

        # Test trace_with_snapshot
        trace_result = mcp_mod.trace_with_snapshot(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            mode="drivers",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(trace_result.get("status"), "success")

        # Check unmapped_signals exists
        self.assertIn("unmapped_signals", trace_result,
                     "Missing unmapped_signals in trace_with_snapshot")
        unmapped = trace_result.get("unmapped_signals", [])
        print(f"  trace_with_snapshot unmapped_signals count: {len(unmapped)}")

        # Check structure of unmapped entries
        for entry in unmapped[:3]:
            self.assertIn("signal", entry, "Missing 'signal' in unmapped entry")
            self.assertIn("reason", entry, "Missing 'reason' in unmapped entry")
            print(f"    - {entry.get('signal')}: {entry.get('reason')}")

        # Test explain_signal_at_time
        explain_result = mcp_mod.explain_signal_at_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=self.signal,
            time=self.time,
            mode="drivers",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(explain_result.get("status"), "success")

        explain_unmapped = explain_result.get("unmapped_signals", [])
        print(f"  explain_signal_at_time unmapped_signals count: {len(explain_unmapped)}")

        # Verify tool still returns success even with unmapped signals
        print(f"  Tool returned success with {len(unmapped)} unmapped signals")

        if not unmapped:
            print("  [INFO] No unmapped signals found (cone may map completely)")

        print(f"  [PASS] Test 9.1: Unmapped signal handling")


if __name__ == "__main__":
    unittest.main(verbosity=2)
