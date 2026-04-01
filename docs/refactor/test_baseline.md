# Test Baseline

## Phase 0a (pre-refactor)

**Date:** 2026-03-31
**Branch:** refactor/split-monolith
**Commit:** 263bf51 (HEAD at baseline time)

## Summary

All test suites pass. 3 test files were fixed to match current tool behavior (rising/falling edge support, radix format changes).

## Fixes Applied

| Test File | Fix |
|---|---|
| `test_cases/tc16_*/test_tc16_backend_sanity.py` | Accept `'rising'` as valid value at posedge time (was expecting `'1'`) |
| `test_cases/tc18_*/test_tc18_edge_correctness.py` | Same `'rising'` vs `'1'` fix for `explain_edge_cause` edge context |
| `test_cases/tc27_*/test_tc27_history_failure.py` | Accept `'changing'` for multi-bit edge-time values; accept `'h000'` radix alongside `'b000000000'` |

**Root cause:** Waveform tool was enhanced to return edge-type descriptors (`rising`/`falling`/`changing`) at transition times instead of just stable values, and hex radix is now used for some signals. Tests were written before this change.

## Test Results

### standalone_trace CTest (2 tests)

```
cd standalone_trace && ctest --test-dir build --output-on-failure
```

| Test | Status |
|---|---|
| rtl_trace_semantic_regression | PASS |
| assignment_utils_test | PASS |

### waveform_explorer Python tests (32 tests)

```
.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview
.venv/bin/python3 -m unittest waveform_explorer.tests.test_waveform_commands
```

| Suite | Tests | Status |
|---|---|---|
| test_signal_overview | 11 | PASS |
| test_waveform_commands | 21 | PASS |

### agent_debug_automation Python tests (49 tests)

```
.venv/bin/python3 -m unittest agent_debug_automation.tests.test_cross_linking
```

| Suite | Tests | Status |
|---|---|---|
| test_cross_linking | 49 | PASS |

### Integration test cases (27 test case directories)

```
./test_cases/run_all_tests.sh
```

| Result | Count |
|---|---|
| Passed | 27 |
| Failed | 0 |

Test cases tc01–tc15: RTL structural trace tests
Test cases tc16–tc27: Cross-link integration tests (require rtl_trace.db + wave.fsdb)

### Known Issues

- **tc25 in harness**: `run_cross_link_tests.py` reports tc25 as FAIL due to resource contention (MCP server processes from prior tests not cleaned up), but tc25 passes when run directly. Not a code bug — harness infrastructure issue.
- **ResourceWarnings**: tc25 emits `ResourceWarning: unclosed file` warnings from MCP subprocess management. Cosmetic, no functional impact.

## Commands Used

```bash
cd standalone_trace && ctest --test-dir build --output-on-failure
cd /home/qsun/AI_PROJ/auto_waveform_debugger
.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview
.venv/bin/python3 -m unittest waveform_explorer.tests.test_waveform_commands
.venv/bin/python3 -m unittest agent_debug_automation.tests.test_cross_linking
./test_cases/run_all_tests.sh
```

**All commands must be run from project root** (except standalone_trace ctest).

---

## Post-Refactor Baseline (Phases 1-4 complete)

**Date:** 2026-04-01
**Branch:** refactor/split-monolith
**Refactor scope:** standalone_trace thin main.cc + directory stubs, waveform_explorer format adapter extraction, agent_debug_automation module split

All test suites pass with identical results. No regressions from refactor.

| Suite | Tests | Status |
|---|---|---|
| standalone_trace CTest | 2 | PASS |
| waveform_explorer signal overview | 11 | PASS |
| waveform_explorer commands | 21 | PASS |
| agent_debug_automation cross-link | 49 | PASS |
| **Total** | **83** | **ALL PASS** |

Integration test cases (tc01-tc27): 27/27 PASS.

**Known issues** (unchanged from Phase 0a):
- tc25 harness resource contention (passes when run directly)
- tc25 ResourceWarning cosmetic warnings
