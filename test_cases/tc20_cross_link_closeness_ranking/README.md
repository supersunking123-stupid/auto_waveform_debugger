# Test Case 20: Cross-Link Closeness-First Ranking

## Phase 5: Closeness-First Ranking Validation

Verifies that signals flipping at or nearest to T outrank signals with only generic activity.

## Test Configuration

- **Signal:** `top.hs_mon.clk`
- **Time:** `175000`
- **Mode:** `drivers`

## Test 5.1: Closeness-First Ranking

Tool: `rank_cone_by_time(...)`

Inspect first 5 entries of:
- `ranking.all_signals`
- `ranking.most_active_near_time`

Expected:
- Entries with transition exactly at T or nearest to T rank highest
- `most_active_near_time` ordering is dominated by `closeness_score`
- Raw toggle count alone does not override exact closeness

The test asserts the ranking is non-empty and that `closeness_score` is
non-increasing over `ranking.all_signals` (first entry has the maximum).
