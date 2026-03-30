# Test Inventory

This repository has three main test layers:

1. component-level regressions for `standalone_trace`, `waveform_explorer`, and `agent_debug_automation`
2. scenario-style regression cases under `test_cases/`
3. a few benchmark / smoke utilities that are useful for manual validation but are not the primary correctness suites

## Quick Run

From the repo root:

```bash
# standalone_trace CTest targets
ctest --test-dir standalone_trace/build --output-on-failure

# waveform_explorer + agent_debug_automation Python regressions
.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview
.venv/bin/python3 -m unittest agent_debug_automation.tests.test_cross_linking

# all scenario tests under test_cases/
cd test_cases && ./run_all_tests.sh

# cross-link-only scenario harness with reports
cd test_cases && .venv/bin/python3 run_cross_link_tests.py
```

## Component Tests

### `standalone_trace`

Primary entry points:

- `ctest --test-dir standalone_trace/build --output-on-failure`
- `.venv/bin/python3 standalone_trace/tests/semantic_regression.py --rtl-trace standalone_trace/build/rtl_trace --source-dir standalone_trace`
- `standalone_trace/build/assignment_utils_test`

Files:

- [standalone_trace/tests/semantic_regression.py](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/tests/semantic_regression.py)
  End-to-end semantic regression for the `rtl_trace` CLI.
  Covers:
  - compile smoke
  - driver tracing across submodule ports
  - load-side assignment context and LHS propagation
  - bit/range query filtering
  - traversal controls such as `--depth` and `--max-nodes`
  - `hier --show-source` hierarchy source-location reporting
  - `whereis-instance` module/source lookup
  - `find` typo suggestions
  - incremental compile cache hit behavior
  - invalid numeric CLI argument handling

- [standalone_trace/tests/assignment_utils_test.cc](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/tests/assignment_utils_test.cc)
  Small C++ unit test for assignment-LHS parsing.
  Covers:
  - nonblocking assignment parsing such as `flag <= hit`
  - blocking assignment parsing with comparisons on the RHS

- [standalone_trace/CMakeLists.txt](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/CMakeLists.txt)
  Registers the two `ctest` targets:
  - `rtl_trace_semantic_regression`
  - `assignment_utils_test`

- [standalone_trace/LOCALTEST.md](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/LOCALTEST.md)
  Local bring-up guide and manual validation notes.

Fixture:

- [standalone_trace/tests/fixtures/semantic_top.sv](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/tests/fixtures/semantic_top.sv)

### `waveform_explorer`

Primary entry point:

```bash
.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview
```

Files:

- [waveform_explorer/tests/test_signal_overview.py](/home/qsun/AI_PROJ/auto_waveform_debugger/waveform_explorer/tests/test_signal_overview.py)
  CLI-oriented regression suite for `wave_agent_cli`.
  Covers:
  - `list_signals` default top-module-only behavior
  - `list_signals` hierarchy wildcard filtering
  - `list_signals` multi-type filtering
  - single-bit overview segmentation
  - multi-bit overview segmentation
  - auto-resolution behavior
  - backward `find_condition`
  - malformed VCD timestamp handling without crashing

Supporting scripts:

- [waveform_explorer/perf_test.py](/home/qsun/AI_PROJ/auto_waveform_debugger/waveform_explorer/perf_test.py)
  Manual performance script, more benchmark than regression.

- [waveform_explorer/perf_test_daemon.py](/home/qsun/AI_PROJ/auto_waveform_debugger/waveform_explorer/perf_test_daemon.py)
  Daemon-mode benchmark helper.

### `agent_debug_automation`

Primary entry point:

```bash
.venv/bin/python3 -m unittest agent_debug_automation.tests.test_cross_linking
```

Files:

- [agent_debug_automation/tests/test_cross_linking.py](/home/qsun/AI_PROJ/auto_waveform_debugger/agent_debug_automation/tests/test_cross_linking.py)
  Main Python regression suite for the merged MCP layer.
  Covers:
  - default session creation
  - cursor / bookmark alias resolution
  - signal-group expansion
  - `list_signals` pattern and type forwarding through the MCP layer
  - session persistence across module reload
  - cross-link time alias handling
  - session-aware `get_signal_overview`
  - auto-resolution plumbing through the MCP layer
  - concurrent reuse of keyed `rtl_trace serve` sessions
  - concurrent reuse of waveform daemons
  - non-FSDB full-namespace fallback for internal waveform mapping
  - trace / explain / snapshot cross-link flows
  - optional real NVDLA FSDB integration workflow

Notes:

- some tests are pure local regressions using repo fixtures
- the NVDLA integration class depends on machine-specific external assets and is guarded accordingly inside the test file

## `test_cases/` Scenario Suites

Top-level harnesses:

- [test_cases/run_all_tests.sh](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/run_all_tests.sh)
  Runs every `tc*/run_test.sh` directory in version order and prints a pass/fail summary.

- [test_cases/run_cross_link_tests.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/run_cross_link_tests.py)
  Dedicated harness for the cross-link scenario suites.
  Runs `tc16` through `tc27`, parses unittest output, and generates:
  - markdown report
  - CSV summary
  - JSON bug list

- [test_cases/CROSS_LINK_TEST_SUITE.md](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/CROSS_LINK_TEST_SUITE.md)
  Describes the cross-link suite phases, known-good signals/times, and report outputs.

### Structural / standalone scenario cases

These are primarily `rtl_trace` scenario regressions. Each directory has a `run_test.sh`.

- `tc01_generate_loop`
- `tc02_multi_level_generate`
- `tc03_param_inst_different`
- `tc04_conditional_generate`
- `tc05_multi_dim_arrays`
- `tc06_structural_hierarchy`
- `tc07_mux_tree`
- `tc08_priority_encoder`
- `tc09_counter_chain`
- `tc10_clock_gating`
- `tc11_deep_soc`
- `tc12_multi_cone`
- `tc13_clock_reset_tree`
- `tc14_system_tasks`
- `tc15_mixed_signal_flow`

These cases exercise structural elaboration and trace behavior across common RTL patterns such as generate blocks, parameterized instances, arrays, muxes, counters, clock/reset structures, and mixed-flow designs.

### Cross-link scenario cases

These combine structural DB + waveform data + MCP-facing cross-link behavior. Each directory has `run_test.sh`, a Python test, and usually a `README.md`.

- [test_cases/tc16_cross_link_backend_sanity/test_tc16_backend_sanity.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc16_cross_link_backend_sanity/test_tc16_backend_sanity.py)
  Backend sanity.

- [test_cases/tc17_cross_link_smoke/test_tc17_smoke.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc17_cross_link_smoke/test_tc17_smoke.py)
  Basic smoke coverage for the cross-link tools.

- [test_cases/tc18_cross_link_edge_correctness/test_tc18_edge_correctness.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc18_cross_link_edge_correctness/test_tc18_edge_correctness.py)
  Exact-edge correctness checks.

- [test_cases/tc19_cross_link_direction_ranking/test_tc19_direction_ranking.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc19_cross_link_direction_ranking/test_tc19_direction_ranking.py)
  Direction-aware ranking.

- [test_cases/tc20_cross_link_closeness_ranking/test_tc20_closeness_ranking.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc20_cross_link_closeness_ranking/test_tc20_closeness_ranking.py)
  Closeness-first ranking.

- [test_cases/tc21_cross_link_stuck_classification/test_tc21_stuck_classification.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc21_cross_link_stuck_classification/test_tc21_stuck_classification.py)
  Stuck-signal classification.

- [test_cases/tc22_cross_link_snapshot_sampling/test_tc22_snapshot_sampling.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc22_cross_link_snapshot_sampling/test_tc22_snapshot_sampling.py)
  Snapshot and sampling behavior.

- [test_cases/tc23_cross_link_mapping_robustness/test_tc23_mapping_robustness.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc23_cross_link_mapping_robustness/test_tc23_mapping_robustness.py)
  Signal mapping robustness.

- [test_cases/tc24_cross_link_unmapped_handling/test_tc24_unmapped_handling.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc24_cross_link_unmapped_handling/test_tc24_unmapped_handling.py)
  Unmapped-signal handling.

- [test_cases/tc25_cross_link_performance/test_tc25_performance.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc25_cross_link_performance/test_tc25_performance.py)
  Performance checks and thresholds.

- [test_cases/tc26_cross_link_non_clock_active/test_tc26_non_clock_active.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc26_cross_link_non_clock_active/test_tc26_non_clock_active.py)
  Non-clock active signal coverage.

- [test_cases/tc27_history_failure_regression/test_tc27_history_failure.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc27_history_failure_regression/test_tc27_history_failure.py)
  Regression for a previously observed history-related failure mode.

## Practical Run Order

For fast local confidence:

1. `ctest --test-dir standalone_trace/build --output-on-failure`
2. `.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview`
3. `.venv/bin/python3 -m unittest agent_debug_automation.tests.test_cross_linking`

For broader end-to-end coverage:

1. `cd test_cases && ./run_all_tests.sh`
2. `cd test_cases && .venv/bin/python3 run_cross_link_tests.py`

## Notes

- `test_cases/` includes generated artifacts in some directories; the authoritative test entry point is still each directory’s `run_test.sh`.
- `run_cross_link_tests.py` currently targets `tc16` through `tc27`.
- some cross-link and FSDB-backed tests depend on machine-local assets such as NVDLA waveform and DB files; those are not guaranteed to exist on every machine.
- vendored dependency test trees also exist under [standalone_trace/third_party/slang/tests](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/third_party/slang/tests) and similar third-party paths, but they are upstream dependency tests rather than the primary project-maintained regression entry points listed above.
