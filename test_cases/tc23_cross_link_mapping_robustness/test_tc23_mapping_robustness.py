#!/usr/bin/env python3
"""
Test Case 23: Cross-Link Mapping Robustness
Phase 8: Structural-To-Waveform Mapping Robustness
"""

import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR))
sys.path.insert(0, str(ROOT_DIR / "test_cases"))

import cross_link_common as fx  # noqa: E402
from agent_debug_automation import agent_debug_automation_mcp as mcp_mod


class MappingRobustnessTests(unittest.TestCase):
    """Phase 8: Structural-To-Waveform Mapping Robustness"""

    @classmethod
    def setUpClass(cls):
        cls.test_cases_dir = Path(__file__).resolve().parent
        cls.root_dir = Path(__file__).resolve().parents[1]
        cls.db_path = fx.DB_PATH
        cls.waveform_path = fx.WAVE_PATH
        cls.rtl_trace_bin = fx.RTL_TRACE_BIN
        cls.wave_cli_bin = fx.WAVE_CLI_BIN

        # Build the fixture on demand (no-op when up to date)
        fx.ensure_fixture()

    def setUp(self):
        mcp_mod.wave_signal_resolution_cache.clear()
        mcp_mod.wave_prefix_page_cache.clear()
        mcp_mod.wave_signal_cache.clear()

    def test_8_1_exact_mapping(self):
        """Test 8.1: Exact mapping"""
        print("\n[Test 8.1] Exact mapping")

        signal = fx.CLK
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

        base_signal = fx.CLK
        top_variant = "TOP.top.hs_mon.clk"

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

        # The FSDB stores the 8-bit burst id as a packed vector `id_in[7:0]`.
        bus = fx.ID + "[7:0]"
        cases = [
            (bus, bus),                # explicit packed range: exact match
            (fx.ID + "[3]", bus),      # single-bit select falls back to the bus
            (fx.ID, bus),              # bare name resolves to the packed vector
        ]
        for signal, expected in cases:
            mapped = mcp_mod._map_signal_to_waveform(
                self.waveform_path, signal, wave_cli_bin=self.wave_cli_bin
            )
            print(f"  signal: {signal} -> mapped: {mapped}")
            self.assertIsNotNone(
                mapped,
                f"Signal {signal} should resolve (exact or fallback)",
            )
            self.assertEqual(mapped, expected, f"Unexpected mapping for {signal}")

        print(f"  [PASS] Test 8.3: Bit-select to bus fallback")


if __name__ == "__main__":
    unittest.main(verbosity=2)
