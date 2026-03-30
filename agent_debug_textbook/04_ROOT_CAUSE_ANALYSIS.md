# Playbook 04 — Root-Cause Analysis

**Role:** You are a debugger. Your job is to find the root cause of a bug or unexpected behavior by systematically combining structural exploration, waveform observation, and signal investigation.

**When to use:** A failure, assertion, or unexpected behavior has been observed, and you need to trace it back to its origin. This is the most complex playbook and prescribes a specific multi-phase workflow.

**Prerequisites:** A compiled structural database and a waveform file must both be available. If not, compile first (see Playbook 02).

---

## Tools used in this playbook

This playbook draws from all other playbooks. The tools are organized by the phase in which you use them.

**Phase 1 — Observe the symptom:**
`get_value_at_time`, `get_snapshot`, `find_edge`, `get_transitions`, `find_condition`

**Phase 2 — Identify suspects:**
`rank_cone_by_time`, `trace_with_snapshot`

**Phase 3 — Explain causation:**
`explain_signal_at_time`, `explain_edge_cause`

**Phase 4 — Trace deeper (if needed):**
`rtl_trace` (trace, find, hier), `get_signal_overview`, `find_value_intervals`

**Throughout — Workspace management:**
`set_cursor`, `create_bookmark`, `create_signal_group`

---

## The debug workflow

### Phase 1 — Observe the symptom

**Goal:** Establish the facts. What is wrong, where, and when?

```
1.1  Identify the failing signal and the time of failure.
     If given an assertion failure or error message, extract the signal name and time.
     If given a vague description, use find_condition or find_value_intervals to locate it.

1.2  Bookmark the failure point:
     → set_cursor(time=T_fail)
     → create_bookmark(bookmark_name="failure", time=T_fail)

1.3  Take a snapshot of the failing signal and its neighbors:
     → get_snapshot(signals=[failing_signal, related_signals...], time="Cursor")

1.4  Check recent activity around the failure:
     → get_transitions(path=failing_signal, start_time=T_fail-window, end_time=T_fail)
     → find_edge(path=failing_signal, edge_type="anyedge", start_time=T_fail, direction="backward")

1.5  Record your observations before moving on.
```

**Output of Phase 1:** You know *what* is wrong and *when* it went wrong. You have a bookmark at the failure time.

---

### Phase 2 — Identify suspects

**Goal:** Narrow down which signals in the driver cone are most likely responsible.

```
2.1  Rank the driver cone by transition recency:
     → rank_cone_by_time(
           db_path="rtl_trace.db",
           signal=failing_signal,
           time="BM_failure",
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
           time="BM_failure",
           mode="drivers",
           clock_path="top.clk",
           cycle_offsets=[-3, -2, -1, 0]
       )
```

**Output of Phase 2:** You have a short list of suspect signals and can see their values leading up to the failure.

---

### Phase 3 — Explain causation

**Goal:** Confirm *which* suspect actually caused the failure and *how*.

```
3.1  For the top suspect, explain the causal chain:
     → explain_edge_cause(
           db_path="rtl_trace.db",
           signal=failing_signal,
           time="BM_failure",
           edge_type=<the edge type that represents the failure>,
           direction="backward"
       )

3.2  If the cause is a signal deeper in the hierarchy, repeat:
     → explain_edge_cause(
           db_path="rtl_trace.db",
           signal=<the identified cause signal>,
           time=<when it changed>,
           direction="backward"
       )

3.3  Continue until you reach a root cause:
     - A primary input or testbench stimulus
     - A register whose value was set in a previous cycle
     - A state machine in an unexpected state

3.4  Bookmark the root cause:
     → create_bookmark(bookmark_name="root_cause", time=T_root)
```

**Output of Phase 3:** You have a causal chain from root cause to symptom, with bookmarks at key points.

---

### Phase 4 — Verify and explore (if needed)

**Goal:** Confirm the root cause and understand the broader context.

```
4.1  If the root cause involves an unexpected state, check how the design got there:
     → get_signal_overview(path=state_signal, start_time=0, end_time=T_root)
     → find_value_intervals(path=state_signal, value=<unexpected_value>, start_time=0, end_time=T_root)

4.2  If you suspect a structural issue (wrong connection, missing logic):
     → rtl_trace(args=["trace", "--db", "rtl_trace.db", "--mode", "drivers",
                        "--signal", root_cause_signal, "--format", "json"])
     → rtl_trace(args=["hier", "--db", "rtl_trace.db", "--root", <relevant_module>])

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
- **Use bookmarks aggressively.** They let you reference times symbolically (`"BM_failure"`, `"BM_root_cause"`) instead of memorizing numbers.
- **Use signal groups** to keep track of suspects and related signals across phases.
- **The debug loop is iterative.** Phase 3 often reveals a deeper cause that sends you back to Phase 2 with a new target signal. This is normal.
- **Stop when you reach a primary input or testbench stimulus.** That is the boundary of RTL responsibility.
- **Window sizing:** When using `rank_cone_by_time`, start with a window of 5–10 clock cycles before the failure. Too wide a window dilutes the ranking; too narrow might miss the cause.
