# Test Enhancement Plan

## Overview

This plan addresses all blind spots identified in the test coverage audit across three components. Each section targets a specific component with concrete test additions.

---

## Part 1: `standalone_trace` Test Enhancements

### 1.1 Enhance `semantic_regression.py` — New Test Cases

**Test 9 — Trace with `--cone-level 2`**
- Compile the existing fixture
- Trace `semantic_top.u_cons.in_bus` with `--cone-level 2 --mode drivers`
- Assert: result contains endpoints from deeper traversal (e.g., the counter logic in `u_prod` reaching through to intermediate signals)
- Assert: endpoint count is greater than cone-level 1 result

**Test 10 — Trace with `--prefer-port-hop`**
- Trace `semantic_top.u_cons.in_bus` with `--prefer-port-hop --mode drivers`
- Assert: returns valid JSON with endpoints
- Assert: endpoint count > 0

**Test 11 — Trace with `--include` regex filter**
- Trace `semantic_top.u_cons.in_bus` with `--include "u_prod.*" --mode drivers`
- Assert: all endpoint paths match the include regex
- Trace with `--include "nonexistent.*"` and assert: zero endpoints

**Test 12 — Trace with `--exclude` regex filter**
- Trace `semantic_top.u_cons.in_bus` with `--exclude "u_prod.*" --mode drivers`
- Assert: no endpoint paths match the excluded pattern

**Test 13 — Trace with `--stop-at` regex**
- Trace `semantic_top.data` with `--stop-at "u_prod.*" --mode drivers --depth 10`
- Assert: traversal stops at matching signals (check stop reason)

**Test 14 — `hier` command basics**
- Run `hier --db <db> --root semantic_top --depth 1 --format json`
- Assert: returns valid JSON with children of semantic_top
- Run `hier --db <db> --depth 0`
- Assert: returns only the root node

**Test 15 — `find` with `--regex`**
- Run `find --db <db> --query "u_prod\\.data" --regex --format json`
- Assert: returns matches containing `u_prod.data`
- Run `find --db <db> --query "zzzzz" --regex`
- Assert: returns zero matches without crashing

**Test 16 — `find` JSON output format**
- Run `find --db <db> --query "data" --format json`
- Assert: valid JSON array, each entry has expected fields

**Test 17 — Invalid signal in trace**
- Run `trace --db <db> --mode drivers --signal semantic_top.nonexistent`
- Assert: exit code != 0 or output contains error/suggestion

**Test 18 — Missing DB file**
- Run `trace --db /tmp/nonexistent_db.db --mode drivers --signal top.sig`
- Assert: exit code != 0, stderr or stdout contains "Failed to read DB"

**Test 19 — `--format text` output validation**
- Run `trace --db <db> --mode drivers --signal semantic_top.u_cons.in_bus`
- Assert: stdout contains "signals:" or "endpoints" section
- Assert: stdout contains path references

**Test 20 — Incremental compile cache miss**
- Compile with `--incremental`
- Touch/modify the source file (append a comment)
- Compile again with `--incremental`
- Assert: second output does NOT contain "incremental-cache-hit"

### 1.2 Enhance `assignment_utils_test.cc`

**Test 3 — Empty assignment text**
- `InferAssignmentLhsPathsFromText("", "")` → empty set

**Test 4 — LHS starting with "top."**
- `InferAssignmentLhsPathsFromText("x", "top.flag <= x")` → `{"top.flag"}`

**Test 5 — Blocking assignment with `==` in nested expression**
- `InferAssignmentLhsPathsFromText("top.out", "assign out = (a == b) ? c : d")` → `{"top.out"}`

### 1.3 New Fixture: `interface_top.sv`

Create a new RTL fixture exercising:
- SystemVerilog interface with modport
- `always_comb` block
- Multi-driver wire (two `assign` to same wire via `ifdef`)
- `inout` port

Add a test case that compiles and traces through interface boundaries.

### 1.4 Add Content Assertions to tc01-tc15

For each of the 15 scenario directories, update `run_test.sh` to:
- After `trace --mode drivers`, validate the JSON output contains endpoints (count > 0)
- After `trace --mode loads`, validate the JSON output contains endpoints (count > 0)
- After `find`, validate the output contains matches
- After `compile`, validate the DB file was created and is non-empty

---

## Part 2: `waveform_explorer` Test Enhancements

### 2.1 New Test File: `waveform_explorer/tests/test_waveform_commands.py`

This file covers all currently-untested waveform CLI commands using the existing `timer_tb.vcd` fixture.

**Test: `get_signal_info`**
- Query `tb.sig` — assert width=1, verify type/direction fields
- Query `tb.bus[3:0]` — assert width=4
- Query non-existent signal — assert error status

**Test: `get_snapshot` single and multi-signal**
- Snapshot `["tb.sig"]` at t=10 — assert value
- Snapshot `["tb.sig", "tb.bus[3:0]"]` at t=10 — assert both values present
- Snapshot with `radix="bin"` — assert binary format
- Snapshot with `radix="dec"` — assert decimal format
- Snapshot at t=0 — assert initial values
- Snapshot with empty signal list — assert error or empty result

**Test: `get_value_at_time`**
- `tb.sig` at t=0 — assert initial value
- `tb.sig` at t=25 — assert toggled value
- `tb.bus[3:0]` at various times — assert correct hex/dec values
- With `radix="bin"` and `radix="dec"`
- Non-existent signal — assert error

**Test: `find_edge`**
- `tb.sig` posedge forward from t=0 — assert edge time found
- `tb.sig` negedge forward from t=0 — assert different edge time
- `tb.sig` anyedge backward from t=40 — assert edge time
- `tb.sig` posedge when no posedge exists in range — assert behavior
- Non-existent signal — assert error

**Test: `find_value_intervals`**
- `tb.sig` value=1 over full range — assert intervals where sig==1
- `tb.bus[3:0]` value="h3" — assert correct interval
- Value that never appears — assert empty result
- With `radix="dec"`

**Test: `get_transitions`**
- `tb.sig` over full range — assert transition list, verify count
- With `max_limit=2` — assert truncated to 2 transitions
- Signal with no transitions (constant) — assert empty or single initial value

**Test: `analyze_pattern`**
- On a clock-like signal (if fixture has one) — assert clock detection
- On a constant signal — assert static classification
- On `tb.sig` — verify pattern classification output

### 2.2 Enhance `test_signal_overview.py`

**Test: `find_condition` forward direction**
- `find_condition` expression="tb.sig==0" direction="forward" from t=30
- Assert finds next time where sig==0

**Test: `find_condition` complex expression**
- Multi-signal condition (if supported by fixture)

**Test: `get_signal_overview` with `radix="bin"` and `radix="dec"`**
- Assert value format matches requested radix

---

## Part 3: `agent_debug_automation` Test Enhancements

### 3.1 Enhance `test_cross_linking.py` — New Test Cases

**Test: `test_move_cursor`**
- `set_cursor(100)`, then `move_cursor(delta=50)` — assert cursor == 150
- `move_cursor(delta=-200)` — assert cursor clamped to 0

**Test: `test_get_cursor`**
- `set_cursor(42)`, `get_cursor()` — assert cursor == 42

**Test: `test_list_bookmarks`**
- Create 2 bookmarks, `list_bookmarks()` — assert both appear

**Test: `test_list_signal_groups`**
- Create a signal group, `list_signal_groups()` — assert it appears

**Test: `test_update_signal_group`**
- Create group with signals A, B
- `update_signal_group(name, signals=[A, C])` — assert group now has A, C
- `update_signal_group(name, description="new desc")` — assert description updated

**Test: `test_delete_bookmark`**
- Create bookmark, delete it, `list_bookmarks()` — assert it's gone

**Test: `test_delete_signal_group`**
- Create group, delete it, `list_signal_groups()` — assert it's gone

**Test: `test_delete_default_session_guard`**
- `delete_session("Default_Session")` — assert error response

**Test: `test_create_duplicate_session`**
- Create session "dup", create session "dup" again — assert error

**Test: `test_session_name_validation`**
- `create_session` with name containing spaces/slashes — assert sanitized or error

**Test: `test_bookmark_name_validation`**
- `create_bookmark` with empty name — assert error
- `create_bookmark` with name "Cursor" — assert error

**Test: `test_invalid_time_references`**
- `get_value_at_time(time="BM_nonexistent")` — assert error
- `get_value_at_time(time=-1)` — assert error or clamped

**Test: `test_find_value_intervals_mcp`**
- `find_value_intervals` with known value — assert correct intervals

**Test: `test_get_transitions_mcp`**
- `get_transitions` with `max_limit=2` — assert truncated

**Test: `test_analyze_pattern_mcp`**
- `analyze_pattern` on known signal — assert classification output

**Test: `test_rtl_trace_wrapper_compile`**
- `rtl_trace(args=["compile", ...])` — assert success, DB created

**Test: `test_rtl_trace_wrapper_trace`**
- `rtl_trace(args=["trace", ...])` — assert JSON output with endpoints

**Test: `test_rtl_trace_wrapper_find`**
- `rtl_trace(args=["find", ...])` — assert matches returned

**Test: `test_rtl_trace_wrapper_hier`**
- `rtl_trace(args=["hier", ...])` — assert hierarchy output

**Test: `test_rtl_trace_serve_lifecycle`**
- `rtl_trace_serve_start`, query, `rtl_trace_serve_stop` — full lifecycle

**Test: `test_rtl_trace_rejects_serve_through_wrapper`**
- `rtl_trace(args=["serve", ...])` — assert error

**Test: `test_cross_link_loads_mode`**
- `explain_signal_at_time(mode="loads")` — assert different results from mode="drivers"

**Test: `test_trace_with_snapshot_trace_options`**
- `trace_with_snapshot(trace_options={"cone_level": 2})` — assert deeper cone

**Test: `test_rank_cone_by_time_with_window`**
- `rank_cone_by_time(window_start=T-500, window_end=T+500)` — assert ranking respects window

**Test: `test_explain_edge_cause_negedge`**
- `explain_edge_cause(edge_type="negedge")` — assert finds falling edge

**Test: `test_explain_edge_cause_forward`**
- `explain_edge_cause(direction="forward")` — assert finds next future edge

### 3.2 Tighten Cross-Link Scenario Assertions (tc16-tc27)

For each weak test, add hard assertions:

**tc20 (Closeness Ranking):**
- Assert `all_signals[0]` has highest `closeness_score`
- Assert `closeness_score` values are monotonically non-increasing

**tc19 test_4_3 (Direction Ranking):**
- Assert `drivers_summary != loads_summary`

**tc23 test 8.2 (Mapping Robustness):**
- Assert TOP.-normalized signal resolves to same value as non-TOP. form

**tc24 (Unmapped Handling):**
- Use a signal that is known to have unmapped cone entries; assert `len(unmapped_signals) > 0`

**tc26 (Non-Clock Active):**
- Pre-select a known-active non-clock signal instead of dynamic discovery with fallback

### 3.3 Fix Harness Issues

**`run_cross_link_tests.py`:**
- Fix phase loop to cover range(1, 13) to include tc27

**`CROSS_LINK_TEST_SUITE.md`:**
- Update timestamp constraint to reflect actual test values
- Add tc27 documentation

---

## Implementation Order

1. `standalone_trace/tests/semantic_regression.py` — add tests 9-20
2. `standalone_trace/tests/assignment_utils_test.cc` — add tests 3-5
3. `standalone_trace/tests/fixtures/interface_top.sv` — new fixture
4. `waveform_explorer/tests/test_waveform_commands.py` — new file, all waveform command tests
5. `waveform_explorer/tests/test_signal_overview.py` — add 3 new tests
6. `agent_debug_automation/tests/test_cross_linking.py` — add ~25 new tests
7. tc16-tc27 scenario tests — tighten assertions
8. `test_cases/run_cross_link_tests.py` — fix phase loop
9. `test_cases/CROSS_LINK_TEST_SUITE.md` — update documentation
10. Update `TEST.md` to reflect all changes
