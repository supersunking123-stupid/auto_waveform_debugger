# Test Case 23: Cross-Link Mapping Robustness

## Phase 8: Structural-To-Waveform Mapping Robustness

Verifies exact match, `TOP.` normalization, and bit/bus fallback.

## Test Assets

- RTL DB: `/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/rtl_trace.db`
- FSDB: `/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb`

## Tests

### Test 8.1: Exact mapping

**Signal:** `top.mem0_rd_bw_mon.clk`

Expected:
- Waveform lookup succeeds directly
- No failure due to namespace mismatch

### Test 8.2: Leading `TOP.` variant

Run waveform-side metadata queries using:
- `top.mem0_rd_bw_mon.clk`
- `TOP.top.mem0_rd_bw_mon.clk` (if supported)

Expected:
- One of the normalized forms resolves correctly

### Test 8.3: Bit-select to bus fallback

**Signal:** `top.nvdla_top.nvdla_core2cvsram_ar_arid[7:0]`

Expected:
- Mapping resolves to waveform signal (exact match for bit-select)
- Or signal is listed in `unmapped_signals` with clear reason
