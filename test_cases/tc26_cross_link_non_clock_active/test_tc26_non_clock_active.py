#!/usr/bin/env python3
"""
Test Case 26: Cross-Link Non-Clock Active Signal
Phase 11: Non-Clock Active Signal Case
"""

import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))
sys.path.insert(0, str(ROOT_DIR / "test_cases"))

import cross_link_common as fx  # noqa: E402
from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class NonClockActiveSignalTests(unittest.TestCase):
    """Phase 11: Non-Clock Active Signal Case"""

    @classmethod
    def setUpClass(cls):
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.root_dir = Path(__file__).resolve().parents[1]
        cls.db_path = fx.DB_PATH
        cls.waveform_path = fx.WAVE_PATH
        cls.rtl_trace_bin = fx.RTL_TRACE_BIN
        cls.wave_cli_bin = fx.WAVE_CLI_BIN
        cls.time = fx.T_REF

        # Candidate signals
        cls.candidates = [
            fx.READY,
            fx.VALID,
        ]

        # Build the fixture on demand (no-op when up to date)
        fx.ensure_fixture()

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

        # First try the hardcoded known-active signal
        hardcoded_signal = fx.READY
        signal = None
        edge_time = None

        edge_result = mcp_mod.find_edge(
            vcd_path=self.waveform_path,
            path=hardcoded_signal,
            edge_type="anyedge",
            start_time=self.time,
            direction="backward",
        )
        if edge_result.get("status") == "success":
            edge_data = edge_result.get("data")
            if edge_data not in (-1, None):
                signal = hardcoded_signal
                edge_time = int(edge_data)
                print(f"  Hardcoded signal {hardcoded_signal} found active at time {edge_time}")

        # Fall back to dynamic discovery only if hardcoded signal fails
        if signal is None:
            signal, edge_time = self._find_active_signal_and_time()

        if signal is None or edge_time is None:
            self.fail(
                "No non-clock active signal found near the fixture reference time; "
                "regenerate the fixture with test_cases/make_fixture.sh --force"
            )

        self.assertEqual(edge_time, self.time, "ready_in should have an edge at the fixture reference time")
        self.assertIsNotNone(signal, "Expected to find a known-active non-clock signal")
        self.assertIsNotNone(edge_time, "Expected to find an edge time for the active signal")

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
        self.assertTrue(drivers_rank.get("ranking", {}).get("all_signals"), "drivers ranking is empty")
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
        self.assertTrue(loads_rank.get("ranking", {}).get("all_signals"), "loads ranking is empty")
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
        self.assertTrue(drivers_summary, "drivers explanation should have a top_summary")
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
        self.assertTrue(loads_summary, "loads explanation should have a top_summary")
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
        self.assertEqual(edge_cause.get("status"), "success", edge_cause.get("message"))
        edge_context = edge_cause.get("waveform", {}).get("edge_context", {})
        print(f"    edge_context: value_before={edge_context.get('value_before_edge')}, "
              f"value_at={edge_context.get('value_at_edge')}")
        self.assertEqual(edge_context.get("value_before_edge"), "1")
        self.assertEqual(edge_context.get("value_at_edge"), "falling")

        print(f"\n  [PASS] Test 11.1: Non-clock active signal validation")


if __name__ == "__main__":
    unittest.main(verbosity=2)
