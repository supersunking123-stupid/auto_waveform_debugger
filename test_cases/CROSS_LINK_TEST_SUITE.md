# Cross-Link Test Suite (tc16-tc26)

This test suite validates the cross-linking features implemented in `agent_debug_automation` against the NVDLA RTL DB and FSDB waveform.

## Test Cases Overview

| ID | Phase | Name | Signal | Time |
|----|-------|------|--------|------|
| tc16 | 1 | Backend Sanity | top.mem0_rd_bw_mon.clk | 399970000 |
| tc17 | 2 | Cross-Link Smoke | top.mem0_rd_bw_mon.ready_in | 399970000 |
| tc18 | 3 | Edge Correctness | top.mem0_rd_bw_mon.clk | 399970000 |
| tc19 | 4 | Direction Ranking | top.mem0_rd_bw_mon.clk | 399970000 |
| tc20 | 5 | Closeness Ranking | top.mem0_rd_bw_mon.clk | 399970000 |
| tc21 | 6 | Stuck Classification | top.mem0_rd_bw_mon.ready_in | 399970000 |
| tc22 | 7 | Snapshot Sampling | top.mem0_rd_bw_mon.clk | 399970000 |
| tc23 | 8 | Mapping Robustness | top.mem0_rd_bw_mon.clk | 399970000 |
| tc24 | 9 | Unmapped Handling | top.mem0_rd_bw_mon.ready_in | 399970000 |
| tc25 | 10 | Performance | top.mem0_rd_bw_mon.ready_in | 399970000 |
| tc26 | 11 | Non-Clock Active | varies | varies |

## Test Assets

- **RTL DB:** `test_cases/rtl_trace.db`
- **FSDB:** `test_cases/wave.fsdb`
- **Binaries:**
  - `standalone_trace/build/rtl_trace`
  - `waveform_explorer/build/wave_agent_cli`

## Running Tests

### Run All Cross-Link Tests

```bash
cd test_cases
python3 run_cross_link_tests.py
```

This generates:
- `cross_link_test_report.md` - Full markdown report
- `cross_link_test_summary.csv` - Summary table
- `cross_link_bug_list.json` - Bug list for failures

### Run Individual Test Cases

```bash
cd test_cases/tc16_cross_link_backend_sanity
./run_test.sh
```

Or run Python tests directly:

```bash
cd test_cases/tc16_cross_link_backend_sanity
python3 test_tc16_backend_sanity.py
```

## Critical Constraints

1. **Time Range:** Never use timestamps > `200010000`
2. **Known-Good Edge:** `top.mem0_rd_bw_mon.clk @ 399970000`
3. **Performance Thresholds:**
   - Cold run: < 10s
   - Hot run: < 0.2s
   - JSON size: < 200KB (WARN if exceeded)

## Result Labels

- **PASS:** All checks passed
- **WARN:** Completed with warnings or ambiguous results
- **FAIL:** Test failed (wrong values, missing fields, crash)

## Test Phases

### Phase 1: Backend Sanity
Validates waveform and structural trace backends work correctly.

### Phase 2: Cross-Link Smoke
Basic smoke tests for all four cross-link tools.

### Phase 3: Exact-Edge Correctness
Mandatory regression check for known-good clock edge.

### Phase 4: Direction-Aware Ranking
Validates drivers vs loads produce different rankings.

### Phase 5: Closeness-First Ranking
Verifies signals flipping at/near T outrank generic activity.

### Phase 6: Stuck Classification
Validates stuck_to_1, stuck_to_0, stuck_other classification.

### Phase 7: Snapshot and Cycle Sampling
Tests absolute and cycle-relative sampling.

### Phase 8: Mapping Robustness
Validates exact match, TOP normalization, bit/bus fallback.

### Phase 9: Unmapped Signal Handling
Verifies unmapped signals are reported with reasons.

### Phase 10: Performance
Measures cold/hot times and JSON sizes.

### Phase 11: Non-Clock Active
Validates cross-link on non-clock control/data signals.

## Deliverables

1. **Markdown Test Report** - Per-phase results with evidence
2. **Summary Table (CSV)** - All test cases with metrics
3. **Bug List (JSON)** - Failures with reproduction info

## Related Files

- Test Plan: `agent_debug_automation/Test_Plan.md`
- Implementation: `agent_debug_automation/agent_debug_automation_mcp.py`
- Existing Tests: `agent_debug_automation/tests/test_cross_linking.py`
