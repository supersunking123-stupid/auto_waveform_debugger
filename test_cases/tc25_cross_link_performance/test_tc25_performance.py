#!/usr/bin/env python3
"""
Test Case 25: Cross-Link Performance
Phase 10: Performance And JSON Size

Measures cold and hot run times for cross-link tools.
"""

import json
import os
import subprocess
import sys
import time
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class PerformanceTests(unittest.TestCase):
    """Phase 10: Performance And JSON Size"""

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
        cls.clock_signal = "top.mem0_rd_bw_mon.clk"

        # Performance thresholds
        cls.cold_threshold = 10.0  # seconds
        cls.hot_threshold = 0.2    # seconds
        cls.size_warn_threshold = 200 * 1024  # 200 KB

        for path, name in [(cls.db_path, "rtl_trace.db"), 
                           (cls.waveform_path, "wave.fsdb"),
                           (cls.rtl_trace_bin, "rtl_trace"),
                           (cls.wave_cli_bin, "wave_agent_cli")]:
            if not os.path.exists(path):
                raise FileNotFoundError(f"{name} not found: {path}")

    def setUp(self):
        # Don't clear caches here - we want to measure real hot performance
        # Caches will be cleared only before cold runs
        pass

    def _clear_caches(self):
        """Explicitly clear all caches for cold run."""
        mcp_mod.wave_signal_resolution_cache.clear()
        mcp_mod.wave_prefix_page_cache.clear()
        mcp_mod.wave_signal_cache.clear()
        # Also clear rtl_trace sessions
        mcp_mod.rtl_serve_sessions.clear()
        mcp_mod.rtl_session_ids_by_key.clear()
        mcp_mod.wave_daemons.clear()

    def _run_cold(self, func_name, kwargs):
        """Run a function in a fresh Python process (cold run)."""
        self._clear_caches()
        script = f"""
import sys
import json
import time
sys.path.insert(0, '{ROOT_DIR}')
from agent_debug_automation import agent_debug_automation_mcp as mcp_mod

start = time.perf_counter()
result = mcp_mod.{func_name}(**{json.dumps(kwargs)})
elapsed = time.perf_counter() - start

print(json.dumps({{
    'elapsed': elapsed,
    'status': result.get('status'),
    'size': len(json.dumps(result, sort_keys=True))
}}))
"""
        proc = subprocess.run(
            [sys.executable, "-c", script],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"Cold run failed: {proc.stderr}")
        return json.loads(proc.stdout.strip())

    def _measure_hot(self, func, kwargs):
        """Run a function in same process (hot run)."""
        start = time.perf_counter()
        result = func(**kwargs)
        elapsed = time.perf_counter() - start
        size = len(json.dumps(result, sort_keys=True))
        return {
            'elapsed': elapsed,
            'status': result.get('status'),
            'size': size
        }

    def _record_result(self, name, cold_data, hot_data):
        """Record and print test result."""
        cold_elapsed = cold_data.get('elapsed', 0)
        hot_elapsed = hot_data.get('elapsed', 0)
        cold_size = cold_data.get('size', 0)
        hot_size = hot_data.get('size', 0)

        result = "PASS"
        notes = []

        if cold_elapsed >= self.cold_threshold:
            result = "FAIL"
            notes.append(f"cold {cold_elapsed:.2f}s >= {self.cold_threshold}s")
        if hot_elapsed >= self.hot_threshold:
            result = "FAIL"
            notes.append(f"hot {hot_elapsed:.4f}s >= {self.hot_threshold}s")
        if cold_size > self.size_warn_threshold:
            if result == "PASS":
                result = "WARN"
            notes.append(f"size {cold_size/1024:.1f}KB > 200KB")

        print(f"\n  {name}:")
        print(f"    Cold: {cold_elapsed:.3f}s, {cold_size/1024:.1f}KB")
        print(f"    Hot:  {hot_elapsed:.4f}s, {hot_size/1024:.1f}KB")
        print(f"    Result: {result}" + (f" ({', '.join(notes)})" if notes else ""))

        return result

    def test_10_1_trace_with_snapshot_perf(self):
        """Test 10.1: trace_with_snapshot performance"""
        print("\n[Test 10.1] trace_with_snapshot performance")

        kwargs = {
            'db_path': self.db_path,
            'waveform_path': self.waveform_path,
            'signal': self.signal,
            'time': self.time,
            'mode': 'drivers',
            'rtl_trace_bin': self.rtl_trace_bin,
            'wave_cli_bin': self.wave_cli_bin,
        }

        # Cold run: clear caches first, then run
        self._clear_caches()
        cold_data = self._measure_hot(mcp_mod.trace_with_snapshot, kwargs)
        self.assertEqual(cold_data.get('status'), 'success')

        # Hot run: same call, caches now warm
        hot_data = self._measure_hot(mcp_mod.trace_with_snapshot, kwargs)
        self.assertEqual(hot_data.get('status'), 'success')

        result = self._record_result("trace_with_snapshot", cold_data, hot_data)
        self.assertIn(result, ['PASS', 'WARN'])

    def test_10_2_explain_signal_at_time_perf(self):
        """Test 10.2: explain_signal_at_time performance"""
        print("\n[Test 10.2] explain_signal_at_time performance")

        kwargs = {
            'db_path': self.db_path,
            'waveform_path': self.waveform_path,
            'signal': self.signal,
            'time': self.time,
            'mode': 'drivers',
            'rtl_trace_bin': self.rtl_trace_bin,
            'wave_cli_bin': self.wave_cli_bin,
        }

        # Cold run: clear caches first, then run
        self._clear_caches()
        cold_data = self._measure_hot(mcp_mod.explain_signal_at_time, kwargs)
        self.assertEqual(cold_data.get('status'), 'success')

        # Hot run: same call, caches now warm
        hot_data = self._measure_hot(mcp_mod.explain_signal_at_time, kwargs)
        self.assertEqual(hot_data.get('status'), 'success')

        result = self._record_result("explain_signal_at_time", cold_data, hot_data)
        self.assertIn(result, ['PASS', 'WARN'])

    def test_10_3_rank_cone_by_time_perf(self):
        """Test 10.3: rank_cone_by_time performance"""
        print("\n[Test 10.3] rank_cone_by_time performance")

        kwargs = {
            'db_path': self.db_path,
            'waveform_path': self.waveform_path,
            'signal': self.signal,
            'time': self.time,
            'mode': 'drivers',
            'rtl_trace_bin': self.rtl_trace_bin,
            'wave_cli_bin': self.wave_cli_bin,
        }

        # Cold run: clear caches first, then run
        self._clear_caches()
        cold_data = self._measure_hot(mcp_mod.rank_cone_by_time, kwargs)
        self.assertEqual(cold_data.get('status'), 'success')

        # Hot run: same call, caches now warm
        hot_data = self._measure_hot(mcp_mod.rank_cone_by_time, kwargs)
        self.assertEqual(hot_data.get('status'), 'success')

        result = self._record_result("rank_cone_by_time", cold_data, hot_data)
        self.assertIn(result, ['PASS', 'WARN'])

    def test_10_4_explain_edge_cause_perf(self):
        """Test 10.4: explain_edge_cause performance"""
        print("\n[Test 10.4] explain_edge_cause performance")

        kwargs = {
            'db_path': self.db_path,
            'waveform_path': self.waveform_path,
            'signal': self.clock_signal,
            'time': self.time,
            'edge_type': 'posedge',
            'direction': 'backward',
            'mode': 'drivers',
            'rtl_trace_bin': self.rtl_trace_bin,
            'wave_cli_bin': self.wave_cli_bin,
        }

        # Cold run: clear caches first, then run
        self._clear_caches()
        cold_data = self._measure_hot(mcp_mod.explain_edge_cause, kwargs)
        self.assertEqual(cold_data.get('status'), 'success')

        # Hot run: same call, caches now warm
        hot_data = self._measure_hot(mcp_mod.explain_edge_cause, kwargs)
        self.assertEqual(hot_data.get('status'), 'success')

        result = self._record_result("explain_edge_cause", cold_data, hot_data)
        self.assertIn(result, ['PASS', 'WARN'])


if __name__ == "__main__":
    unittest.main(verbosity=2)
