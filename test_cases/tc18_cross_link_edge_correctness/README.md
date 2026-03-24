# Test Case 18: Cross-Link Exact-Edge Correctness Regression

## Phase 3: Exact-Edge Correctness Regression

This test case is mandatory. It validates exact edge resolution for the known-good clock signal.

## Test Assets

- RTL DB: `/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/rtl_trace.db`
- FSDB: `/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb`

## Test Configuration

- **Signal:** `top.mem0_rd_bw_mon.clk`
- **Time:** `399970000`
- **Edge Type:** `posedge`
- **Direction:** `backward`
- **Mode:** `drivers`

## Test 3.1: explain_edge_cause Exact-Edge Check

Tool call:
```
explain_edge_cause(
    signal="top.mem0_rd_bw_mon.clk",
    time=399970000,
    edge_type="posedge",
    direction="backward",
    mode="drivers"
)
```

### Expected Results

- `time_context.resolved_edge_time == 399970000`
- `waveform.edge_context.value_before_edge == "0"`
- `waveform.edge_context.value_at_edge == "1"`

**Mark FAIL if any of these are wrong.**

## Known Constraints

- Never use timestamps > `200010000`
- This is a mandatory correctness check
