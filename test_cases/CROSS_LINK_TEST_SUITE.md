# Cross-Link Test Suite (tc16-tc27)

This test suite validates the cross-linking features implemented in `agent_debug_automation` against a small,
regenerable RTL DB and FSDB waveform (the "cross-link fixture"). The fixture replaces the large external design
the suite used to depend on.

## Test Cases Overview

| ID | Phase | Name | Signal | Time (ps) |
|----|-------|------|--------|-----------|
| tc16 | 1 | Backend Sanity | top.hs_mon.clk | 175000 |
| tc17 | 2 | Cross-Link Smoke | top.hs_mon.ready_in | 175000 |
| tc18 | 3 | Edge Correctness | top.hs_mon.clk | 175000 |
| tc19 | 4 | Direction Ranking | top.hs_mon.clk | 175000 |
| tc20 | 5 | Closeness Ranking | top.hs_mon.clk | 175000 |
| tc21 | 6 | Stuck Classification | top.hs_mon.ready_in | 2505000 (stall window) |
| tc22 | 7 | Snapshot Sampling | top.hs_mon.ready_in | 175000 |
| tc23 | 8 | Mapping Robustness | top.hs_mon.clk / id_in | n/a |
| tc24 | 9 | Unmapped Handling | top.dut.u_fifo.rd_data | 175000 |
| tc25 | 10 | Performance | top.hs_mon.ready_in | 175000 |
| tc26 | 11 | Non-Clock Active | top.hs_mon.ready_in / valid_in | 175000 |
| tc27 | 12 | History Failure Regression | packed-vector signals (fifo count/wr_ptr, src data/id) | 1000000-1100000 |

## Fixture

`test_cases/cross_link_fixture/src` holds a small source -> FIFO -> sink design (`xl_src`, `xl_fifo`, `xl_sink`,
wrapped by `xl_dut`), a passive handshake monitor (`xl_hs_mon`) and a testbench whose top module is named `top`.
Hierarchy: `top` -> `dut` (`u_src`, `u_fifo`, `u_sink`) and `hs_mon`.

Timeline (FSDB timescale 1ps, clock period 10 ns, posedges at 5 ns + k * 10 ns):

| Interval (ps) | Behaviour |
|---------------|-----------|
| 0 - 33000 | reset |
| 33000 - 2005000 | normal traffic; sink accepts 2 of 3 cycles so `ready_in` toggles |
| 2005000 - 3005000 | sink stalled: FIFO full, `ready_in` stuck at 0 (`full`, `cfg_en` stuck at 1; `flush` stuck at 0) |
| 3005000 - 4000000 | normal traffic |

At `175000` (the reference posedge, `T_REF`) `ready_in`, `valid_in` and `last_in` all fall. The FIFO memory array
`top.dut.u_fifo.mem` is intentionally not dumped to the FSDB, which gives tc24 a genuinely unmapped signal.
All shared constants live in `test_cases/cross_link_common.py`.

Regenerate with one command (each `run_test.sh` and Python test also does this automatically when outputs are
missing or stale):

```bash
test_cases/make_fixture.sh           # build if missing/stale
test_cases/make_fixture.sh --force   # always rebuild
```

It runs `standalone_trace/build/rtl_trace compile --top top` and VCS + the Verdi FSDB PLI (override with
`RTL_TRACE`, `VCS_BIN`, `VCS_HOME`, `VERDI_HOME`). Outputs are gitignored; build products go in
`test_cases/cross_link_fixture/build/`.

## Test Assets

- **RTL DB:** `test_cases/rtl_trace.db` (generated)
- **FSDB:** `test_cases/wave.fsdb` (generated)
- **Binaries:**
  - `standalone_trace/build/rtl_trace`
  - `waveform_explorer/build/wave_agent_cli`

## Running Tests

### Run All Cross-Link Tests

```bash
cd test_cases
../.venv/bin/python3 run_cross_link_tests.py
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
../../.venv/bin/python3 test_tc16_backend_sanity.py
```

## Critical Constraints

1. **Time Range:** The fixture simulation ends at `4000000` ps; stall window is `2005000`-`3005000`
2. **Known-Good Edge:** `top.hs_mon.clk @ 175000` (posedge; `ready_in`/`valid_in`/`last_in` fall there)
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

### Phase 12: History Failure Regression
Regression tests for previously identified signal-path resolution failures (bare hierarchical names vs.
packed-vector FSDB paths). Re-targeted to the fixture's packed-vector signals in the 1000000-1100000 ps window.

## Deliverables

1. **Markdown Test Report** - Per-phase results with evidence
2. **Summary Table (CSV)** - All test cases with metrics
3. **Bug List (JSON)** - Failures with reproduction info

## Related Files

- Test Plan: `agent_debug_automation/Test_Plan.md`
- Implementation: `agent_debug_automation/agent_debug_automation_mcp.py`
- Existing Tests: `agent_debug_automation/tests/test_cross_linking.py`
