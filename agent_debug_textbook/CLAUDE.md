# RTL Waveform Debugging — Mandatory Agent Instructions

These instructions apply whenever you are asked to debug, analyze, or investigate an RTL design in this project. They are not optional background reading. Follow them before taking any action.

---

## Step 0 — Before doing anything else

1. **Read `agent_debug_textbook/00_ROUTER.md`** and identify which playbook applies to your task.
2. **Open that playbook** and follow it as a numbered procedure from top to bottom. Do not skip steps. Do not substitute source-code reading for a tool call.
3. **Confirm the `EDA_for_agent` MCP server is available.** If it is not listed in your tools, report this immediately before proceeding.
4. **For any task that needs structural tracing or RCA, determine the `rtl_trace.db` path before making structural calls.** If local context does not clearly show whether a DB already exists, ask the user. If no DB exists, ask the user for the exact project command or flow used to generate it.
5. **Check whether a sufficient architecture document already exists for the relevant debug scope.** A sufficient doc must cover the relevant design or subsystem and identify its major hierarchy boundaries, interfaces/peers, clock/reset domains, and control/dataflow landmarks. If no sufficient doc exists, route to `agent_debug_textbook/08_DESIGN_MAPPING.md` and use the crawler flow before active waveform debugging. Use `rtl-crawler` by default; use `rtl-crawler-multi-agent` only when delegation is explicitly authorized. Do not start deep signal tracing in a subsystem whose major child blocks and boundaries you cannot name.

---

## Hard rules — no exceptions

### On source files
**NEVER open a `.v` or `.sv` file to trace a signal, understand connectivity, or determine why a signal has a value.** That is exactly what `explain_signal_at_time`, `trace_with_snapshot`, `rtl_trace trace`, and `explain_edge_cause` exist for. They answer the same question faster and with simulation evidence attached.

Source file reading is permitted in exactly two situations:
- After MCP tools have already identified the guilty instance and you need to read the RTL logic to confirm the bug (use `whereis-instance` to find the file).
- When writing or modifying RTL as part of a fix.

In all other cases: use the tools first. If a tool fails, report the failure (Rule 10). Do not silently fall back to reading source.

### On time values
**NEVER pass a time integer to any waveform tool without first calling `get_signal_info` to confirm the time precision.** The most common setup (`1ns/1ps`) means precision is 1 ps — passing `100` when you mean 100 ns queries 100 ps instead, 1000× too early, and will silently return empty or wrong results. See Rule 12 in `rtl_debug_guide.md`.

### On empty or nonsensical results
**NEVER continue making tool calls when two consecutive results are empty, all-zero, or contradictory.** Stop, diagnose (wrong time unit? wrong signal path? no simulation data in that range?), and report. See Rule 13 in `rtl_debug_guide.md`.

### On guessing
**NEVER state an assumption as a fact or use "probably" / "likely" as the basis for the next tool call.** If you do not know the value of a signal, query it. If a tool is failing and you cannot recover, report the failure rather than speculating past it.

### On simulation logs
**NEVER dump an entire simulation log into your context window.** When reviewing `test.log`, `sim.log`, `comp.log`, or similar files, extract only the useful parts with targeted commands such as `head`, `tail`, and `grep`. Pull the failing error message, nearby lines, summary counts, and the final status first; only widen the slice if those do not answer the question.

---

## The prescribed debug workflow

```
Phase 0: Orient and map (architecture doc check → crawl if missing, then timescale, clock domain, workspace)
    ↓
Phase 1: Observe symptom (get_snapshot, get_transitions, find_edge)
    ↓
Phase 2: Identify suspects (rank_cone_by_time, trace_with_snapshot)
    ↓
Phase 3: Explain causation layer by layer (explain_edge_cause, explain_signal_at_time)
    ↓
Phase 4: Verify and document (bookmarks, signal groups, causal chain)
```

Phase 0 is mandatory. Before tracing any signal, you must either have a sufficient architecture document for the debug scope or generate one via `agent_debug_textbook/08_DESIGN_MAPPING.md`. The full Phase 0 procedure is in `agent_debug_textbook/04_ROOT_CAUSE_ANALYSIS.md`. Use it as a checklist.

**For failures involving unknown (`X`) values**, route through `agent_debug_textbook/09_X_TRACING.md` to isolate the likely creator block via hierarchy-boundary walking before applying the standard Phase 1–4 flow.

Efficiency defaults:
- If you expect 3+ structural queries on the same DB, use `rtl_trace_serve_start/query/stop`.
- If you need values for several signals at one time, use `get_snapshot` or `trace_with_snapshot`, not repeated `get_value_at_time`.
- If a compound event will be queried more than once, create a virtual signal once and reuse it.
- If a transition stream is long, export it with `dump_waveform_data` before writing a script.

## When to use supervised mode

If a single-agent debug attempt has failed — the agent drifted from playbooks, made careless tool calls, or reached conclusions based on unverified assumptions — retry with the two-agent **Supervised Debug** architecture described in `agent_debug_textbook/06_SUPERVISED_DEBUG.md`.

Key elements:
- A **Supervisor** agent reviews each phase or compact batch and the resulting conclusions before the Debugger advances to the next phase.
- **Phase 0 gate:** the Supervisor must block Phase 1 if the Debugger has not verified architecture-document sufficiency or routed through `08_DESIGN_MAPPING.md` when no sufficient doc exists.
- An **error-scenario anchor session** serves as the starting point and backtracking target for every investigation branch.
- **Python scripts** replace manual inspection of long waveform data sequences (>20 transitions).
- **Golden boundary rule** (Rule 14): trust EDA-vendor VIP protocol checkers, but do not automatically treat home-grown memory models, BFMs, scoreboards, or assertions as golden.
- **Two-step escalation (Rule 4):** at ~8 investigation calls inside one subsystem without progress, pause and run Playbook 08 to refresh the design map; only after one bounded post-crawl pass, escalate to a local testbench at ~10 calls.
