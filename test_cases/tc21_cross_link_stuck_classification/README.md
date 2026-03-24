# Test Case 21: Cross-Link Stuck Classification

## Phase 6: Stuck Classification Validation

Verifies `stuck_to_1`, `stuck_to_0`, and `stuck_other` classification.

## Test Configuration

- **Signal:** `top.mem0_rd_bw_mon.ready_in`
- **Time:** `399970000`
- **Mode:** `drivers`

## Test 6.1: Stuck Classification

Tool: `rank_cone_by_time(...)`

Inspect `ranking.most_stuck_in_window`.

Expected per entry:
- `is_constant_in_window`
- `stuck_class`
- `stuck_score`

Expected ranking policy:
- `stuck_to_1` > `stuck_to_0` > `stuck_other`

If cone doesn't expose enough variation, rerun on `top.mem0_rd_bw_mon.valid_in`.
If still insufficient, mark WARN.
