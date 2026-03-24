# Test Case 19: Cross-Link Direction-Aware Ranking

## Phase 4: Direction-Aware Ranking

Validates that `drivers` and `loads` modes produce different directional priorities.

## Test Assets

- RTL DB: `/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/rtl_trace.db`
- FSDB: `/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb`

## Test Configuration

- **Signal:** `top.mem0_rd_bw_mon.clk`
- **Time:** `399970000`

## Tests

### Test 4.1: rank_cone_by_time drivers

**Mode:** `drivers`

Expected ranking fields per entry:
- `closest_transition_time`
- `closest_transition_distance`
- `closest_transition_direction`
- `preferred_transition_side`
- `used_preferred_transition_side`
- `closeness_score`

Expected:
- `preferred_transition_side == "at_or_before"` for ranked summaries
- Top-ranked entries are upstream-looking

### Test 4.2: rank_cone_by_time loads

**Mode:** `loads`

Expected:
- `preferred_transition_side == "at_or_after"`
- Ranking differs from drivers
- Top-ranked entries are more load-side or immediate-fanout oriented

### Test 4.3: explain_signal_at_time drivers vs loads

Run both modes and compare.

Expected:
- `top_summary` for `drivers` and `loads` is not identical
- `drivers` prefers upstream structure
- `loads` stays closer to target or fanout side

Use WARN if cone is too small to make distinction strong.
