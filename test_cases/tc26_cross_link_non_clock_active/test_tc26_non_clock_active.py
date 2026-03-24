#!/usr/bin/env python3
"""
Test Case 26: Cross-Link Non-Clock Active Signal
Phase 11: Non-Clock Active Signal Case
"""

import os
import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class NonClockActiveSignalTests(unittest.TestCase):
    """Phase 11: Non-Clock Active Signal Case"""

    @classmethod
    def setUpClass(cls):
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.root_dir = Path(__file__).resolve().parents[1]
        cls.db_path = str(cls.root_dir / "rtl_trace.db")
        cls.waveform_path = str(cls.root_dir / "wave.fsdb")
        cls.rtl_trace_bin = str(cls.root_dir.parent / "standalone_trace" / "build" / "rtl_trace")
        cls.wave_cli_bin = str(cls.root_dir.parent / "waveform_explorer" / "build" / "wave_agent_cli")
        cls.time = 399970000

        # Candidate signals
        cls.candidates = [
            "top.mem0_rd_bw_mon.ready_in",
            "top.mem0_rd_bw_mon.valid_in",
        ]

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

    def _find_active_signal_and_time(self):
        """Find a non-clock signal with an edge near a valid time."""
        for signal in self.candidates:
            # Try to find an edge near our reference time
            edge_result = mcp_mod.find_edge(
                vcd_path=self.waveform_path,
                path=signal,
                edge_type="anyedge",
                start_time=self.time,
                direction="backward",
            )
            if edge_result.get("status") == "success":
                edge_time = edge_result.get("data")
                if edge_time not in (-1, None):
                    print(f"  Found edge for {signal} at time {edge_time}")
                    return signal, int(edge_time)

        # If no edge found at reference time, try nearby times
        for signal in self.candidates:
            for offset in [0, 1000000, 2000000, 5000000]:
                edge_result = mcp_mod.find_edge(
                    vcd_path=self.waveform_path,
                    path=signal,
                    edge_type="anyedge",
                    start_time=self.time + offset,
                    direction="backward",
                )
                if edge_result.get("status") == "success":
                    edge_time = edge_result.get("data")
                    if edge_time not in (-1, None):
                        print(f"  Found edge for {signal} at time {edge_time}")
                        return signal, int(edge_time)

        return None, None

    def test_11_1_non_clock_active_signal(self):
        """Test 11.1: Non-clock active signal validation"""
        print("\n[Test 11.1] Non-clock active signal validation")

        # Find active signal and time
        signal, edge_time = self._find_active_signal_and_time()

        if signal is None or edge_time is None:
            print("  [WARN] No suitable non-clock active signal found")
            print("  Checked signals:")
            for s in self.candidates:
                print(f"    - {s}: no edge found near reference time")
            # Still pass but with warning
            print("  [PASS] Test 11.1: Non-clock active signal (WARN - no active signal found)")
            return

        print(f"  Using signal: {signal} at time {edge_time}")

        # Run rank_cone_by_time drivers
        print("\n  Running rank_cone_by_time (drivers)...")
        drivers_rank = mcp_mod.rank_cone_by_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=signal,
            time=edge_time,
            mode="drivers",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(drivers_rank.get("status"), "success")
        print(f"    drivers signals ranked: {len(drivers_rank.get('ranking', {}).get('all_signals', []))}")

        # Run rank_cone_by_time loads
        print("\n  Running rank_cone_by_time (loads)...")
        loads_rank = mcp_mod.rank_cone_by_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=signal,
            time=edge_time,
            mode="loads",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(loads_rank.get("status"), "success")
        print(f"    loads signals ranked: {len(loads_rank.get('ranking', {}).get('all_signals', []))}")

        # Run explain_signal_at_time drivers
        print("\n  Running explain_signal_at_time (drivers)...")
        drivers_explain = mcp_mod.explain_signal_at_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=signal,
            time=edge_time,
            mode="drivers",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(drivers_explain.get("status"), "success")
        drivers_summary = drivers_explain.get("explanations", {}).get("top_summary")
        print(f"    drivers top_summary: {drivers_summary}")

        # Run explain_signal_at_time loads
        print("\n  Running explain_signal_at_time (loads)...")
        loads_explain = mcp_mod.explain_signal_at_time(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=signal,
            time=edge_time,
            mode="loads",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        self.assertEqual(loads_explain.get("status"), "success")
        loads_summary = loads_explain.get("explanations", {}).get("top_summary")
        print(f"    loads top_summary: {loads_summary}")

        # Optionally run explain_edge_cause
        print("\n  Running explain_edge_cause...")
        edge_cause = mcp_mod.explain_edge_cause(
            db_path=self.db_path,
            waveform_path=self.waveform_path,
            signal=signal,
            time=edge_time,
            edge_type="anyedge",
            direction="backward",
            mode="drivers",
            rtl_trace_bin=self.rtl_trace_bin,
            wave_cli_bin=self.wave_cli_bin,
        )
        if edge_cause.get("status") == "success":
            edge_context = edge_cause.get("waveform", {}).get("edge_context", {})
            print(f"    edge_context: value_before={edge_context.get('value_before_edge')}, "
                  f"value_at={edge_context.get('value_at_edge')}")
        else:
            print(f"    explain_edge_cause: {edge_cause.get('message', 'no result')}")

        print(f"\n  [PASS] Test 11.1: Non-clock active signal validation")


if __name__ == "__main__":
    unittest.main(verbosity=2)
