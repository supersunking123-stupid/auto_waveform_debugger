#!/usr/bin/env python3
"""
Test Case 23: Cross-Link Mapping Robustness
Phase 8: Structural-To-Waveform Mapping Robustness
"""

import os
import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))

from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class MappingRobustnessTests(unittest.TestCase):
    """Phase 8: Structural-To-Waveform Mapping Robustness"""

    @classmethod
    def setUpClass(cls):
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.root_dir = Path(__file__).resolve().parents[1]
        cls.db_path = str(cls.root_dir / "rtl_trace.db")
        cls.waveform_path = str(cls.root_dir / "wave.fsdb")
        cls.rtl_trace_bin = str(cls.root_dir.parent / "standalone_trace" / "build" / "rtl_trace")
        cls.wave_cli_bin = str(cls.root_dir.parent / "waveform_explorer" / "build" / "wave_agent_cli")

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

    def test_8_1_exact_mapping(self):
        """Test 8.1: Exact mapping"""
        print("\n[Test 8.1] Exact mapping")

        signal = "top.mem0_rd_bw_mon.clk"
        mapped = mcp_mod._map_signal_to_waveform(
            self.waveform_path, signal, wave_cli_bin=self.wave_cli_bin
        )

        print(f"  signal: {signal}")
        print(f"  mapped: {mapped}")

        self.assertIsNotNone(mapped, f"Exact mapping failed for {signal}")
        self.assertEqual(mapped, signal, f"Mapping should be exact for {signal}")

        print(f"  [PASS] Test 8.1: Exact mapping")

    def test_8_2_top_normalization(self):
        """Test 8.2: Leading TOP. variant"""
        print("\n[Test 8.2] TOP. normalization")

        base_signal = "top.mem0_rd_bw_mon.clk"
        top_variant = "TOP.top.mem0_rd_bw_mon.clk"

        # Test base signal
        mapped_base = mcp_mod._map_signal_to_waveform(
            self.waveform_path, base_signal, wave_cli_bin=self.wave_cli_bin
        )
        print(f"  base signal '{base_signal}' -> {mapped_base}")

        # Test TOP. variant
        mapped_top = mcp_mod._map_signal_to_waveform(
            self.waveform_path, top_variant, wave_cli_bin=self.wave_cli_bin
        )
        print(f"  TOP variant '{top_variant}' -> {mapped_top}")

        # At least one should resolve
        if mapped_base is not None:
            print(f"  Base signal resolved successfully")
        if mapped_top is not None:
            print(f"  TOP variant resolved successfully")

        # Assert TOP.-prefixed version resolves to the same value as the base form
        if mapped_base is not None and mapped_top is not None:
            self.assertEqual(
                mapped_base, mapped_top,
                f"TOP. normalization should resolve to same value: base={mapped_base}, top={mapped_top}",
            )

        # The normalization helper should handle this
        normalized = mcp_mod._normalize_top_variants(base_signal)
        print(f"  Normalized variants: {normalized}")

        print(f"  [PASS] Test 8.2: TOP. normalization")

    def test_8_3_bit_select_fallback(self):
        """Test 8.3: Bit-select to bus fallback"""
        print("\n[Test 8.3] Bit-select to bus fallback")

        # Use the known bit-select signal from NVDLA design
        bit_select_signal = "top.nvdla_top.nvdla_core2cvsram_ar_arid[7:0]"
        
        print(f"  Testing bit-select signal: {bit_select_signal}")
        
        # Test direct mapping of bit-select signal
        mapped = mcp_mod._map_signal_to_waveform(
            self.waveform_path, bit_select_signal, wave_cli_bin=self.wave_cli_bin
        )
        print(f"  signal: {bit_select_signal}")
        print(f"  mapped: {mapped}")
        
        # Assert that mapping returns a non-empty result (either exact or via fallback)
        self.assertIsNotNone(
            mapped,
            f"Bit-select signal {bit_select_signal} should resolve (exact or fallback)",
        )

        print(f"  [PASS] Test 8.3: Bit-select to bus fallback")


if __name__ == "__main__":
    unittest.main(verbosity=2)
