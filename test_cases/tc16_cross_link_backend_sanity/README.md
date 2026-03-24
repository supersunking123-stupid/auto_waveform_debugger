# Test Case 16: Cross-Link Backend Sanity Tests

## Phase 1: Backend Sanity

This test case validates the backend infrastructure before running cross-link tests.

## Test Assets

- RTL DB: `/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/rtl_trace.db`
- FSDB: `/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb`
- Binaries:
  - `standalone_trace/build/rtl_trace`
  - `waveform_explorer/build/wave_agent_cli`

## Tests

### Test 1.1: Exact-edge waveform check

Validates waveform backend can find exact edges.

**Signal:** `top.mem0_rd_bw_mon.clk`
**Time:** `399970000`

Expected:
- `find_edge(..., posedge, 399970000, backward)` → `399970000`
- `get_value_at_time(..., 10009999)` → `"0"`
- `get_value_at_time(..., 399970000)` → `"1"`

### Test 1.2: Structural trace sanity

Validates rtl_trace backend returns valid structural data.

**Signal:** `top.mem0_rd_bw_mon.clk`
**Mode:** `drivers`

Expected:
- `status` = `success`
- Contains `target`, `mode`, `summary`, `endpoints`

### Test 1.3: FSDB signal-info sanity

Validates waveform signal metadata lookup.

**Signal:** `top.mem0_rd_bw_mon.clk`

Expected:
- `status` = `success`
- Contains `path`, `width`, `type`, `timescale`

## Known Constraints

- Never use timestamps > `200010000`
- Use known-good clock edge at `top.mem0_rd_bw_mon.clk @ 399970000`
