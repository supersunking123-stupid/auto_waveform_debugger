# Test Case 25: Cross-Link Performance

## Phase 10: Performance And JSON Size

Measures cold and hot run times for cross-link tools.

## Test Configuration

- **Signal:** `top.hs_mon.ready_in`
- **Time:** `175000`

## Test Cases

### Case 10.1: trace_with_snapshot
### Case 10.2: explain_signal_at_time
### Case 10.3: rank_cone_by_time
### Case 10.4: explain_edge_cause (clock signal)

## Expected Performance Envelope

For the small fixture cone:
- **Cold:** roughly `0.01s` to `0.1s` (pass threshold: `< 10s`)
- **Hot:** roughly `0.001s` (pass threshold: `< 0.2s`)

## JSON Size Expectations

- Normal small-cone response: KB to tens of KB
- If response exceeds `200 KB`, mark `WARN`

## Measurements

For each case, record:
- Cold elapsed time
- Hot elapsed time
- JSON size in bytes
- Result label (PASS/WARN/FAIL)
