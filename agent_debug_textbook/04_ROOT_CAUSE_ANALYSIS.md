# Playbook 04 — Root-Cause Analysis

**Role:** You are a debugger. Your job is to find the root cause of a bug or unexpected behavior by systematically combining structural exploration, waveform observation, and signal investigation.

**When to use:** A failure, assertion, or unexpected behavior has been observed, and you need to trace it back to its origin. This is the most complex playbook and prescribes a specific multi-phase workflow.

**Prerequisites:** A compiled structural database and a waveform file must both be available. Before starting, determine whether a usable `rtl_trace.db` already exists and what path to use. If local context does not make that clear, ask the user. If no DB exists, ask the user for the exact command or flow used in this project to generate it rather than guessing compile flags. See Playbook 02.

---

## Tools used in this playbook

This playbook draws from all other playbooks. The tools are organized by the phase in which you use them.

**Phase 1 — Observe and anchor the failure:**
`get_signal_info`, `create_session`, `switch_session`, `set_cursor`, `create_bookmark`, `get_snapshot`, `create_signal_group`, `find_edge`, `get_transitions`, `find_condition`

**Phase 2 — Identify suspects:**
`rank_cone_by_time`, `trace_with_snapshot`, `rtl_trace` (trace), `get_snapshot`

**Phase 3 — Explain causation:**
`explain_signal_at_time`, `explain_edge_cause`, `rtl_trace_serve_start/query/stop`

**Phase 4 — Verify and document:**
`rtl_trace` (trace, find, hier, whereis-instance), `get_signal_overview`, `find_value_intervals`, `dump_waveform_data`

**Throughout — Workspace management:**
`create_session`, `switch_session`, `set_cursor`, `create_bookmark`, `create_signal_group`

**Optional efficiency chain:**
`create_signal_expression`, `update_signal_expression`, `delete_signal_expression`, `list_signal_expressions`

## Efficiency defaults

Use these defaults unless you have a specific reason not to:

- If you expect 3+ structural queries on the same DB, start `rtl_trace_serve_start` early and reuse it through the phase instead of paying one-shot startup cost repeatedly.
- If you need values for multiple signals at one time, use `get_snapshot` or `trace_with_snapshot`; do not loop over `get_value_at_time`.
- If a compound protocol event or handshake will be queried more than once, create a virtual signal once instead of repeating the same `find_condition` expression.
- If a transition stream is too long to inspect comfortably in MCP output, export it with `dump_waveform_data` and analyze the file with a short script.

---

## The debug workflow

### Phase 1 — Observe the symptom

**Goal:** Establish the facts. What is wrong, where, and when?

**Phase 1 watchlist** — verify before each step:
- **Structural DB preflight:** Before the first structural call, confirm the `rtl_trace.db` path you will use. If that is unknown, stop and ask the user instead of guessing.
- **Rule 12:** Call `get_signal_info` before any time argument — wrong units silently return empty or incorrect results (1ns/1ps precision means 100 ns → pass `100000`, not `100`).
- **Rule 9:** Normalize before comparing waveform values to logs or VIP messages — ARLEN is 0-based on the waveform (VIP `LENGTH` is 1-based); verify radix and byte vs. word addresses.
- **Rule 13:** Two consecutive empty or contradictory results → stop and diagnose (time unit? wrong signal path?) before making another call.
- **Rule 14:** EDA-vendor VIP protocol monitors/checkers are golden. Home-grown BFMs, memory models, monitors, scoreboards, and assertions are evidence sources, not automatically golden. Never question the vendor VIP/checker itself; trace the signal path that reached it and identify who drives it.

```
1.1  Identify the failing signal and the time of failure.
     If given an assertion failure or error message, extract the signal name and time.
     If given a vague description, use find_condition or find_value_intervals to locate it.
     If the failure description comes from a simulation log, do not dump the full
     log into context. Use targeted extraction (`grep`, `head`, `tail`, and small
     surrounding slices) to capture the failing message, nearby context, and final
     run status first.

1.2  Create the error-scenario anchor session — this is your home base for the
     entire debug. Every investigation branch starts here and must ultimately
     explain the signals captured in this snapshot.
     → create_session(waveform_path=<path>, session_name="error_scenario")
     → switch_session(session_name="error_scenario", waveform_path=<path>)
     → set_cursor(time=T_fail)
     → create_bookmark(bookmark_name="error_point", time=T_fail,
                        description="<failure desc + time precision note>")

1.3  Trace the structural driver chain of the failing signal BEFORE waveform
     browsing. This one call tells you whether the failing signal is driven by
     DUT logic or testbench logic — which determines the entire direction of
     the investigation.

     → rtl_trace(args=["trace", "--db", "rtl_trace.db", "--mode", "drivers",
                        "--signal", failing_signal, "--format", "json"])

     Read the result before continuing:
     - Driver is DUT RTL → proceed into the DUT in Phase 2.
     - Driver is testbench RTL → the investigation shifts to the testbench.
       Apply Phase 2–3 to the testbench driver chain, not the DUT.

     If you already expect 3+ structural queries on the same DB in this debug,
     start serve mode now and reuse it through Phases 2–4:
     → rtl_trace_serve_start(serve_args=["--db", "rtl_trace.db"])

1.4  Take a snapshot of the failing signal and ALL signals mentioned in the
     error message:
     → get_snapshot(signals=[failing_signal, related_signals...], time="Cursor")
     → create_signal_group(group_name="error_interface",
                            signals=[...the signals just snapshotted...])

1.5  Check recent activity around the failure:
     → get_transitions(path=failing_signal, start_time=T_fail-window, end_time=T_fail)
     → find_edge(path=failing_signal, edge_type="anyedge", start_time=T_fail, direction="backward")

1.6  Understand how the waveform correlates with the error message BEFORE
     moving on. For each signal in the error message, explain how its waveform
     value at T_fail produced the specific error text. If you cannot explain the
     correlation, you are not ready for Phase 2. If the correlation depends on a
     repeated compound event (for example a handshake or burst-complete pulse),
     create a virtual signal once and browse that signal instead of repeating the
     same `find_condition` expression.

1.7  Record your observations. The error-scenario snapshot is the contract:
     your final root cause must explain every value in it.
```

**Output of Phase 1:** You know *what* is wrong and *when* it went wrong. You have a bookmark at the failure time.

---

### Phase 2 — Identify suspects

**Goal:** Narrow down which signals in the driver cone are most likely responsible.

**Phase 2 watchlist** — verify before each step:
- **Rule 3:** If a FIFO, CDC synchronizer, or arbiter is in the suspect cone, check pointer/flag values explicitly — their failures are deferred and non-local, and waveform tools alone may not surface them.
- **Rule 4:** If you have made >10 calls on a single block's internal signals without a concrete root cause, stop and escalate to a local testbench (see `EDA_USE.md`).
- **Rule 11:** Do not stop at the first wrong signal — every wrong driver must be traced to its own root cause before declaring a conclusion.

```
2.1  Rank the driver cone by transition recency:
     → rank_cone_by_time(
           db_path="rtl_trace.db",
           signal=failing_signal,
           time="BM_error_point",
           mode="drivers",
           window_start=T_fail - search_window,
           window_end=T_fail
       )

2.2  Review the ranked results. The top-ranked signals (those that transitioned
     most recently before the failure) are your primary suspects.

2.3  Create a signal group for the suspects:
     → create_signal_group(group_name="suspects", signals=[suspect1, suspect2, ...])

2.4  Take a multi-time-point snapshot to see how suspects evolved:
     → trace_with_snapshot(
           db_path="rtl_trace.db",
           signal=failing_signal,
           time="BM_error_point",
           mode="drivers",
           clock_path="top.clk",
           cycle_offsets=[-3, -2, -1, 0]
       )

2.5  If the prime suspect is still unclear, navigate the driver cone with iterative
     structural tracing — but batch the waveform reads:

     1. Use `rtl_trace trace --mode drivers` (or `rtl_trace_serve_query` if serve
        mode is active) on the suspect signal to get a bounded driver list
     2. Sample those drivers in one batch:
        - `trace_with_snapshot(...)` if you want cone + values together, or
        - `get_snapshot(signals=[driver1, driver2, ...], time=T_fail)` if you
          already have the exact driver list
     3. Recurse only on drivers whose values are wrong or whose transitions are
        closest to the failure
     4. On the deepest wrong signal, call `get_transitions` or
        `dump_waveform_data` over a wide range to inspect its history in one place

     ⚠ Anti-pattern: do not call `get_value_at_time` once per driver or query each
     pipeline stage individually to build a manual "value map". That approach
     turns one hypothesis into N waveform calls, misses structural relationships,
     and does not scale. If you have made more than ~5 waveform queries on a
     signal without a structural trace guiding them, stop and switch to iterative
     structural tracing with batched reads.
```

**Output of Phase 2:** You have a short list of suspect signals and can see their values leading up to the failure.

---

### Phase 3 — Explain causation layer by layer

**Goal:** Build a complete causal chain from symptom to root cause by descending the driver cone one level at a time. **Do not stop as soon as you find a signal that looks wrong. Every wrong signal must be traced further until you reach an actual root cause.**

**Phase 3 watchlist** — verify before each step:
- **Rule 11:** Trace EVERY wrong branch to a stop criterion. A state machine in an unexpected state is not a root cause — ask why it entered that state and keep tracing.
- **Rule 5:** Each step must answer "why does this signal have this value?" not "what is this signal's value?" — distinguish observation (symptom) from explanation (cause).
- **Rule 4:** The >10 calls escalation trigger still applies. If a block remains opaque after 10 calls in Phase 3, spawn a local test rather than continuing passive observation.
- **Rule 13:** Contradictory driver values (same signal, two different values) → path mismatch or timescale error. Stop and verify before continuing.

```
3.1  For the top suspect, explain the causal chain:
     → explain_edge_cause(
           db_path="rtl_trace.db",
           signal=failing_signal,
           time="BM_error_point",
           edge_type=<the edge type that represents the failure>,
           direction="backward"
       )

3.2  Inspect every RHS driver returned in the result:
     a. Check the waveform value of each driver at the failure time.
     b. If a driver value is correct: the fault is in the logic between
        that driver and the signal above it — record this and stop this branch.
     c. If a driver value is also wrong: it is now the new signal of interest.
        Repeat step 3.1 on it, using the time at which it was wrong.

     → explain_signal_at_time(
           db_path="rtl_trace.db",
           signal=<wrong_driver>,
           time=<time_it_was_wrong>
       )

3.3  Repeat 3.2 on every wrong driver, one layer at a time, until EVERY branch
     of the cone terminates at one of these stop criteria:
     - A primary input or testbench stimulus that is provably wrong
     - A register whose stored value was loaded incorrectly in a prior cycle
       (go back to that cycle and repeat Phase 3 from there)
     - A combinational expression where all inputs are correct but the
       operator, bit-select, or polarity is wrong by design
     - A CDC crossing with a missing or broken synchronizer

     ⚠ A state machine in an unexpected state is NOT a root cause by itself.
       Ask: "Why did it enter that state?" and continue tracing.

3.4  Bookmark each key causal point as you confirm it:
     → create_bookmark(bookmark_name="cause_L1", time=T_cause1, description="...")
     → create_bookmark(bookmark_name="root_cause", time=T_root, description="...")
```

**Common mistake:** Finding that `signal B is 0 when it should be 1` and immediately concluding "the logic driving B is the bug — let me fix it." B's driver may itself be driven by a wrong signal. Check B's drivers before touching any code.

**When data sequences are long (>20 transitions):** Do not try to reason over raw JSON output mentally. First export the transition stream with `dump_waveform_data(..., output_path=...)`, then write a short Python script to process the file — iterate transitions, compare expected vs. actual, flag anomalies. See `06_SUPERVISED_DEBUG.md` for the script structure and review protocol. This prevents miscounting, value confusion, and prompt bloat from pasting large JSON blobs into the conversation.

**Backtracking to the error-scenario anchor:** If a branch terminates at a correct signal (dead end), return to the error-scenario session and pick a different signal from the `error_interface` group to trace:
```
switch_session(session_name="error_scenario")
get_snapshot(signals=["error_interface"], signals_are_groups=True, time="BM_error_point")
# Pick the next untried signal from the group and restart from Phase 2
```

**Output of Phase 3:** A complete causal chain, with every link supported by waveform evidence, ending at a root cause that satisfies one of the stop criteria above. The root cause must explain every signal value in the error-scenario snapshot created in Phase 1.

---

### Phase 4 — Verify and explore (if needed)

**Goal:** Confirm the root cause and understand the broader context.

**Phase 4 watchlist** — verify before concluding:
- **Completeness check:** Does your root cause explain every signal value in the Phase 1 error-scenario snapshot? If not, the investigation is incomplete — return to Phase 2 with the unexplained signal.
- **Rule 7:** Trust the waveform over your mental model. If the evidence contradicts your expectation of how the design should behave, the expectation is wrong.

```
4.1  If the root cause involves an unexpected state, check how the design got there:
     → get_signal_overview(path=state_signal, start_time=0, end_time=T_root)
     → find_value_intervals(path=state_signal, value=<unexpected_value>, start_time=0, end_time=T_root)

4.2  If you suspect a structural issue (wrong connection, missing logic):
     → rtl_trace(args=["trace", "--db", "rtl_trace.db", "--mode", "drivers",
                        "--signal", root_cause_signal, "--format", "json"])
     → rtl_trace(args=["hier", "--db", "rtl_trace.db", "--root", <relevant_module>])

     To read the RTL source of the guilty instance directly:
     → rtl_trace(args=["whereis-instance", "--db", "rtl_trace.db",
                        "--instance", <instance_path>, "--format", "json"])
     The result gives you the exact source file and line — read it to confirm
     the structural bug (wrong bit-select, missing condition, inverted polarity).

4.3  If you need to check whether the bug occurred elsewhere in the simulation:
     → find_condition(expression=<bug_condition>, start_time=0, direction="forward")

4.4  Document your findings with signal groups and bookmarks for future reference.
```

---

## Example: debugging a FIFO overflow

```
Symptom: top.u_fifo.overflow == 1 at time 320000

Phase 1:
  set_cursor(time=320000)
  create_bookmark(bookmark_name="overflow", time=320000)
  get_snapshot(signals=["top.u_fifo.overflow", "top.u_fifo.wr_en", "top.u_fifo.rd_en",
                        "top.u_fifo.count"], time="Cursor")
  → overflow=1, wr_en=1, rd_en=0, count=16 (full)

Phase 2:
  rank_cone_by_time(db_path="rtl_trace.db", signal="top.u_fifo.overflow",
                    time="BM_overflow", window_start=310000, window_end=320000)
  → top suspects: top.u_fifo.count, top.u_fifo.wr_en, top.u_arbiter.grant

  trace_with_snapshot(db_path="rtl_trace.db", signal="top.u_fifo.overflow",
                      time="BM_overflow", clock_path="top.clk", cycle_offsets=[-5,-4,-3,-2,-1,0])
  → count was 14 at -5 cycles, ramped to 16. rd_en was 0 the whole time.

Phase 3:
  explain_edge_cause(db_path="rtl_trace.db", signal="top.u_fifo.rd_en",
                     time="BM_overflow", edge_type="falling", direction="backward")
  → rd_en fell at 300000 because top.u_consumer.ready dropped.

  explain_edge_cause(db_path="rtl_trace.db", signal="top.u_consumer.ready",
                     time=300000, edge_type="falling", direction="backward")
  → ready dropped because top.u_consumer.state went to STALL.

  → Root cause: consumer entered STALL state at 300000, stopped reading, FIFO filled up.
  create_bookmark(bookmark_name="root_cause", time=300000)

Phase 4:
  get_signal_overview(path="top.u_consumer.state", start_time=0, end_time=320000)
  → state was IDLE until 250000, then ACTIVE, then STALL at 300000. Investigate why STALL.
```

---

## Tips

- **Always start with Phase 1.** Skipping observation leads to chasing the wrong signal.
- **Use bookmarks aggressively.** They let you reference times symbolically (`"BM_error_point"`, `"BM_root_cause"`) instead of memorizing numbers.
- **Use signal groups** to keep track of suspects and related signals across phases.
- **The debug loop is iterative.** Phase 3 often reveals a deeper cause that sends you back to Phase 2 with a new target signal. This is normal.
- **Stop when you reach a primary input or testbench stimulus.** That is the boundary of RTL responsibility.
- **Window sizing:** When using `rank_cone_by_time`, start with a window of 5–10 clock cycles before the failure. Too wide a window dilutes the ranking; too narrow might miss the cause.
- **Golden boundary (Rule 14):** Trust EDA-vendor VIP protocol checkers. Do not automatically treat home-grown assertions, scoreboards, memory models, or BFMs as golden. Trace the immediate driver of the flagged signal before deciding whether the source is DUT RTL or testbench RTL.
- **Error-scenario anchor:** Your final conclusion must explain every value in the Phase 1 error-scenario snapshot. If it doesn't, the investigation is incomplete.
