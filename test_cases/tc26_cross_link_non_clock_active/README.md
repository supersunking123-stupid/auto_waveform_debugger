# Test Case 26: Cross-Link Non-Clock Active Signal

## Phase 11: Non-Clock Active Signal Case

The clock case is required but not sufficient. Validates cross-link on non-clock active control/data signal.

## Test Configuration

Start candidates:
- `top.mem0_rd_bw_mon.ready_in`
- `top.mem0_rd_bw_mon.valid_in`

## Test 11.1: Non-Clock Active Signal

Procedure:
1. Use waveform queries to determine if either signal has actual edge near candidate time (0..200010000)
2. If not, inspect structural cone for nearby active non-clock signal
3. Choose one in-range real edge time
4. Run:
   - `rank_cone_by_time(..., mode="drivers")`
   - `rank_cone_by_time(..., mode="loads")`
   - `explain_signal_at_time(..., mode="drivers")`
   - `explain_signal_at_time(..., mode="loads")`
   - Optionally `explain_edge_cause(...)`

Expected:
- Direction-aware ranking behaves sensibly
- Explanations are usable on control/data path

If no suitable non-clock active signal found, mark WARN.
