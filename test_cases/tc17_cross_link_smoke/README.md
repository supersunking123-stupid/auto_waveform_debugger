# Test Case 17: Cross-Link Smoke Tests

## Phase 2: Cross-Link Smoke Tests

This test case validates the four main cross-link tools with basic smoke tests.

## Test Assets

- RTL DB: `/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/rtl_trace.db`
- FSDB: `/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb`

## Focus Point

- **Signal:** `top.mem0_rd_bw_mon.ready_in`
- **Time:** `399970000`
- **Mode:** `drivers`

## Tests

### Test 2.1: trace_with_snapshot

Validates structural trace with waveform snapshot.

Expected keys:
- `status`, `target`, `time_context`, `structure`, `waveform`, `ranking`, `unmapped_signals`, `warnings`
- `structure.trace` exists
- `structure.cone_signals` is non-empty
- `waveform.focus_samples` exists

### Test 2.2: explain_signal_at_time

Validates explanation generation.

Expected:
- All common keys from Test 2.1
- `explanations.candidate_paths` exists
- `explanations.top_candidate` exists or is `null` with reason
- `explanations.top_summary` exists when candidates present

### Test 2.3: rank_cone_by_time

Validates signal ranking.

Expected:
- `ranking.all_signals`
- `ranking.most_active_near_time`
- `ranking.most_stuck_in_window`
- `ranking.unchanged_candidates`

### Test 2.4: explain_edge_cause

Validates edge cause explanation.

Expected:
- `time_context.requested_time`
- `time_context.resolved_edge_time`
- `waveform.edge_context.value_before_edge`
- `waveform.edge_context.value_at_edge`

## Known Constraints

- Never use timestamps > `200010000`
