# Test Case 21: Cross-Link Stuck Classification

## Phase 6: Stuck Classification Validation

Verifies `stuck_to_1`, `stuck_to_0`, and `stuck_other` classification.

## Test Configuration

- **Signal:** `top.hs_mon.ready_in`
- **Time:** `2505000` (`T_STUCK`, inside the stall window 2005000-3005000)
- **Mode:** `drivers`

## Test 6.1: Stuck Classification

Tool: `rank_cone_by_time(...)`

Inspect `ranking.most_stuck_in_window`.

Expected per entry:
- `is_constant_in_window`
- `stuck_class`
- `stuck_score`

Expected in the fixture cone at `T_STUCK`:
- `top.hs_mon.ready_in`: `stuck_to_0` (FIFO full, sink stalled)
- `top.dut.u_fifo.full`: `stuck_to_1`
- `top.dut.u_fifo.cfg_en`: `stuck_to_1` (tied high in the testbench)
- `top.dut.u_fifo.flush`: `stuck_to_0` (tied low in the testbench)

Expected ranking policy:
- `stuck_to_1` > `stuck_to_0` > `stuck_other` (entries sorted by `stuck_score` descending)

At the reference time `175000` the handshake signal toggles, so `ready_in` must
not be reported stuck there.
