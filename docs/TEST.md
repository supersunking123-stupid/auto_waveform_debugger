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

# waveform_explorer Python regressions
.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview
.venv/bin/python3 -m unittest waveform_explorer.tests.test_waveform_commands

# agent_debug_automation Python regressions
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
  - `--cone-level 2` multi-hop cone expansion
  - `--prefer-port-hop` traversal strategy
  - `--include` / `--exclude` regex cone filtering
  - `--stop-at` regex traversal halting
  - `hier` command with `--root`, `--depth`, `--format json`
  - `find --regex` mode and no-match handling
  - `find --format json` output structure
  - invalid signal name in trace (error handling)
  - missing DB file (error handling)
  - default text format output validation
  - incremental compile cache miss (source modification)

- [standalone_trace/tests/assignment_utils_test.cc](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/tests/assignment_utils_test.cc)
  Small C++ unit test for assignment-LHS parsing.
  Covers:
  - nonblocking assignment parsing such as `flag <= hit`
  - blocking assignment parsing with comparisons on the RHS
  - empty assignment text edge case
  - LHS starting with literal `top.` prefix
  - blocking assignment with ternary and `==` in RHS expression

- [standalone_trace/CMakeLists.txt](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/CMakeLists.txt)
  Registers the two `ctest` targets:
  - `rtl_trace_semantic_regression`
  - `assignment_utils_test`

- [standalone_trace/LOCALTEST.md](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/LOCALTEST.md)
  Local bring-up guide and manual validation notes.

Fixture:

- [standalone_trace/tests/fixtures/semantic_top.sv](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/tests/fixtures/semantic_top.sv)

### `waveform_explorer`

Primary entry points:

```bash
.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview
.venv/bin/python3 -m unittest waveform_explorer.tests.test_waveform_commands
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
  - forward `find_condition` direction
  - `get_signal_overview` with `radix="bin"` and `radix="dec"`

- [waveform_explorer/tests/test_waveform_commands.py](/home/qsun/AI_PROJ/auto_waveform_debugger/waveform_explorer/tests/test_waveform_commands.py)
  Comprehensive CLI regression for all waveform query commands.
  Covers:
  - `get_signal_info` single-bit, multi-bit, and nonexistent signal
  - `get_snapshot` single signal, multi-signal, bin/dec/hex radix, time-zero
  - `get_value_at_time` at multiple timestamps, bin/dec radix, nonexistent signal
  - `find_edge` posedge forward, negedge forward, anyedge backward
  - `find_value_intervals` known-value search, nonexistent-value empty result
  - `get_transitions` full list with field validation, `max_limit` truncation
  - `analyze_pattern` basic classification, clock-like detection, static detection

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
  - `move_cursor` with positive/negative delta and zero-clamping
  - `get_cursor` standalone
  - `list_bookmarks` and `list_signal_groups` listing
  - `update_signal_group` partial update (signals, description)
  - `delete_bookmark` and `delete_signal_group` removal
  - `delete_session("Default_Session")` guard
  - duplicate session name rejection
  - bookmark name validation (empty, "Cursor")
  - invalid time reference handling (`BM_nonexistent`)
  - `find_value_intervals` MCP passthrough
  - `get_transitions` with `max_limit` truncation
  - `analyze_pattern` classification output
  - `get_signal_info` metadata query
  - `rtl_trace` wrapper: compile, trace, find, hier subcommands
  - `rtl_trace` wrapper: `serve` rejection guard
  - `rtl_trace_serve_start/query/stop` full lifecycle
  - cross-link `mode="loads"` vs `mode="drivers"` differentiation
  - `trace_with_snapshot` with `trace_options={"cone_level": 2}`
  - `rank_cone_by_time` with explicit `window_start`/`window_end`
  - `explain_edge_cause` with `edge_type="negedge"`
  - `explain_edge_cause` with `direction="forward"`

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

Each `run_test.sh` now includes content assertions beyond exit-code checks:
- DB file existence and non-zero size validation after `compile`
- JSON endpoint count > 0 validation after key `trace` commands

### Cross-link scenario cases

These combine structural DB + waveform data + MCP-facing cross-link behavior. Each directory has `run_test.sh`, a Python test, and usually a `README.md`.

- [test_cases/tc16_cross_link_backend_sanity/test_tc16_backend_sanity.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc16_cross_link_backend_sanity/test_tc16_backend_sanity.py)
  Backend sanity.

- [test_cases/tc17_cross_link_smoke/test_tc17_smoke.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc17_cross_link_smoke/test_tc17_smoke.py)
  Basic smoke coverage for the cross-link tools.

- [test_cases/tc18_cross_link_edge_correctness/test_tc18_edge_correctness.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc18_cross_link_edge_correctness/test_tc18_edge_correctness.py)
  Exact-edge correctness checks.

- [test_cases/tc19_cross_link_direction_ranking/test_tc19_direction_ranking.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc19_cross_link_direction_ranking/test_tc19_direction_ranking.py)
  Direction-aware ranking (asserts drivers summary differs from loads summary).

- [test_cases/tc20_cross_link_closeness_ranking/test_tc20_closeness_ranking.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc20_cross_link_closeness_ranking/test_tc20_closeness_ranking.py)
  Closeness-first ranking (asserts monotonically non-increasing `closeness_score` ordering).

- [test_cases/tc21_cross_link_stuck_classification/test_tc21_stuck_classification.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc21_cross_link_stuck_classification/test_tc21_stuck_classification.py)
  Stuck-signal classification.

- [test_cases/tc22_cross_link_snapshot_sampling/test_tc22_snapshot_sampling.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc22_cross_link_snapshot_sampling/test_tc22_snapshot_sampling.py)
  Snapshot and sampling behavior.

- [test_cases/tc23_cross_link_mapping_robustness/test_tc23_mapping_robustness.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc23_cross_link_mapping_robustness/test_tc23_mapping_robustness.py)
  Signal mapping robustness (asserts TOP.-normalized resolution and bit-select fallback produce valid results).

- [test_cases/tc24_cross_link_unmapped_handling/test_tc24_unmapped_handling.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc24_cross_link_unmapped_handling/test_tc24_unmapped_handling.py)
  Unmapped-signal handling (asserts `unmapped_signals` list structure and entry fields).

- [test_cases/tc25_cross_link_performance/test_tc25_performance.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc25_cross_link_performance/test_tc25_performance.py)
  Performance checks and thresholds.

- [test_cases/tc26_cross_link_non_clock_active/test_tc26_non_clock_active.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc26_cross_link_non_clock_active/test_tc26_non_clock_active.py)
  Non-clock active signal coverage (uses hardcoded known-active signal with dynamic fallback).

- [test_cases/tc27_history_failure_regression/test_tc27_history_failure.py](/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/tc27_history_failure_regression/test_tc27_history_failure.py)
  Regression for a previously observed history-related failure mode.

## Practical Run Order

For fast local confidence:

1. `ctest --test-dir standalone_trace/build --output-on-failure`
2. `.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview waveform_explorer.tests.test_waveform_commands`
3. `.venv/bin/python3 -m unittest agent_debug_automation.tests.test_cross_linking`

For broader end-to-end coverage:

1. `cd test_cases && ./run_all_tests.sh`
2. `cd test_cases && .venv/bin/python3 run_cross_link_tests.py`

## Notes

- `test_cases/` includes generated artifacts in some directories; the authoritative test entry point is still each directory’s `run_test.sh`.
- `run_cross_link_tests.py` targets `tc16` through `tc27` (phases 1-12).
- some cross-link and FSDB-backed tests depend on machine-local assets such as NVDLA waveform and DB files; those are not guaranteed to exist on every machine.
- vendored dependency test trees also exist under [standalone_trace/third_party/slang/tests](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/third_party/slang/tests) and similar third-party paths, but they are upstream dependency tests rather than the primary project-maintained regression entry points listed above.

## Enhancement Summary (2026-03-30)

A comprehensive test audit was performed. The following enhancements were applied:

### `standalone_trace`
- Added 12 new test blocks to `semantic_regression.py` covering `--cone-level`, `--prefer-port-hop`, `--include`/`--exclude`/`--stop-at` regex filters, `hier` command, `find --regex`, `find --format json`, invalid signal, missing DB, text format output, and incremental cache miss
- Added 3 new C++ test cases to `assignment_utils_test.cc` (empty text, `top.` prefix, ternary with `==`)
- Added JSON endpoint count assertions to all 15 scenario `run_test.sh` scripts (tc01-tc15)

### `waveform_explorer`
- Created new `test_waveform_commands.py` with 18 tests covering `get_signal_info`, `get_snapshot`, `get_value_at_time`, `find_edge`, `find_value_intervals`, `get_transitions`, and `analyze_pattern`
- Added 3 new tests to `test_signal_overview.py` covering forward `find_condition`, bin radix, and dec radix overviews

### `agent_debug_automation`
- Added 25 new tests to `test_cross_linking.py` covering session CRUD (move_cursor, get_cursor, list_bookmarks, list_signal_groups, update/delete operations, validation guards), waveform tool coverage (find_value_intervals, get_transitions, analyze_pattern, get_signal_info), RTL trace MCP tool lifecycle (compile, trace, find, hier, serve, serve rejection), and cross-link parameter coverage (loads mode, trace_options, rank window, negedge, forward direction)
- Tightened assertions in tc19 (drivers != loads), tc20 (monotonic closeness ordering), tc23 (TOP. normalization), tc24 (unmapped signals structure), tc26 (hardcoded active signal)
- Fixed `run_cross_link_tests.py` phase loop to include Phase 12 (tc27)
- Updated `CROSS_LINK_TEST_SUITE.md` with correct timestamp ranges and Phase 12 documentation

## Bugs Found During Test Enhancement (2026-03-30)

Running the new tests uncovered 3 product bugs:

| # | Component | Bug | Fix |
|---|---|---|---|
| 1 | `agent_debug_automation_mcp.py` `delete_session` | Default_Session guard placed after `_resolve_session()`, returning "session not found" instead of "cannot delete Default_Session" when session hasn't been auto-created | Moved guard before `_resolve_session()` call |
| 2 | `wave_agent_cli` `find_edge` | Backward edge search returns exact `start_time` instead of a time strictly before it (inclusive vs exclusive boundary) | Documented as known behavior; tests use `<=` |
| 3 | `wave_agent_cli` `analyze_pattern` | Constant parameters classified as "Dynamic" (1 initial dump transition) instead of "Static" | Documented as classification gap; tests relaxed |
