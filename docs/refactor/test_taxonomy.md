# Test Taxonomy

## Test Categories

### 1. Component-Local Tests

Tests that exercise a single component in isolation.

#### standalone_trace CTest
- **Location:** `standalone_trace/tests/semantic_regression.py`
- **Runner:** CTest drives `semantic_regression.py` which invokes `rtl_trace` CLI
- **What it covers:** Cross-port tracing, loads context LHS, bit filtering, depth/node stops, find suggestions, incremental cache hits, hierarchy-source reporting, whereis-instance lookup, assignment utils
- **Run command:**
  ```bash
  cd standalone_trace && ctest --test-dir build --output-on-failure
  ```

#### waveform_explorer Python tests
- **Location:** `waveform_explorer/tests/test_signal_overview.py`, `waveform_explorer/tests/test_waveform_commands.py`
- **Runner:** Python unittest
- **What it covers:** Signal overview generation, waveform query commands (snapshot, edge, transitions, etc.)
- **Run commands:**
  ```bash
  cd /home/qsun/AI_PROJ/auto_waveform_debugger
  .venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview
  .venv/bin/python3 -m unittest waveform_explorer.tests.test_waveform_commands
  ```

#### agent_debug_automation cross-link tests
- **Location:** `agent_debug_automation/tests/test_cross_linking.py`
- **Runner:** Python unittest
- **What it covers:** Cross-link tool behavior (trace_with_snapshot, explain_signal_at_time, rank_cone_by_time, explain_edge_cause), signal mapping, ranking heuristics
- **Run command:**
  ```bash
  cd /home/qsun/AI_PROJ/auto_waveform_debugger
  .venv/bin/python3 -m unittest agent_debug_automation.tests.test_cross_linking
  ```

### 2. Integration Test Cases

Tests that exercise end-to-end workflows across multiple components.

- **Location:** `test_cases/tc01/` through `test_cases/tc27/`
- **Structure:** Each directory has `run_test.sh` + test-specific Python files
- **Assets:** Shared `test_cases/rtl_trace.db` and `test_cases/wave.fsdb`

#### Structural trace test cases (tc01–tc15)
- Compile RTL, build DB, run trace/hier/find queries
- Verify structural connectivity, hierarchy, and signal search

#### Cross-link integration test cases (tc16–tc27)
- Require both `rtl_trace.db` and `wave.fsdb`
- Test cross-linked structural + waveform analysis
- tc16: Backend sanity
- tc17: Smoke test
- tc18: Edge correctness
- tc19: Direction ranking
- tc20: Closeness ranking
- tc21: Stuck classification
- tc22: Snapshot sampling
- tc23: Mapping robustness
- tc24: Unmapped handling
- tc25: Performance
- tc26: Non-clock active
- tc27: History failure regression

#### Run commands
```bash
# All 27 test cases
cd /home/qsun/AI_PROJ/auto_waveform_debugger
./test_cases/run_all_tests.sh

# Cross-link tests only (tc16-tc27)
cd /home/qsun/AI_PROJ/auto_waveform_debugger
.venv/bin/python3 test_cases/run_cross_link_tests.py

# Single test case
cd /home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc16_cross_link_backend_sanity
./run_test.sh
```

## Full Regression Command

Run all test suites from project root:

```bash
cd /home/qsun/AI_PROJ/auto_waveform_debugger

# 1. standalone_trace CTest
(cd standalone_trace && ctest --test-dir build --output-on-failure)

# 2. waveform_explorer Python tests
.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview
.venv/bin/python3 -m unittest waveform_explorer.tests.test_waveform_commands

# 3. agent_debug_automation cross-link tests
.venv/bin/python3 -m unittest agent_debug_automation.tests.test_cross_linking

# 4. Integration test cases
./test_cases/run_all_tests.sh
```

## Test Counts (baseline 2026-03-31)

| Suite | Tests | Status |
|---|---|---|
| standalone_trace CTest | 2 | PASS |
| waveform_explorer signal overview | 11 | PASS |
| waveform_explorer commands | 21 | PASS |
| agent_debug_automation cross-link | 49 | PASS |
| Integration test cases | 27 dirs | 27 PASS |

## Known Issues

- `test_cases/run_cross_link_tests.py` reports tc25 as FAIL due to harness resource contention, but tc25 passes when run directly. Not a code bug.
- tc25 emits `ResourceWarning: unclosed file` from MCP subprocess management. Cosmetic only.
