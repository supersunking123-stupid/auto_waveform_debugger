# RTL Waveform Debugging — Mandatory Agent Instructions

These instructions apply whenever you are asked to debug, analyze, or investigate an RTL design in this project. They are not optional background reading. Follow them before taking any action.

---

## Step 0 — Before doing anything else

1. **Read `agent_debug_textbook/00_ROUTER.md`** and identify which playbook applies to your task.
2. **Open that playbook** and follow it as a numbered procedure from top to bottom. Do not skip steps. Do not substitute source-code reading for a tool call.
3. **Confirm the `EDA_for_agent` MCP server is available.** If it is not listed in your tools, report this immediately before proceeding.

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

---

## The prescribed debug workflow

```
Phase 0: Orient (timescale, hierarchy, clock domain, workspace)
    ↓
Phase 1: Observe symptom (get_snapshot, get_transitions, find_edge)
    ↓
Phase 2: Identify suspects (rank_cone_by_time, trace_with_snapshot)
    ↓
Phase 3: Explain causation layer by layer (explain_edge_cause, explain_signal_at_time)
    ↓
Phase 4: Verify and document (bookmarks, signal groups, causal chain)
```

This workflow is fully prescribed in `agent_debug_textbook/04_ROOT_CAUSE_ANALYSIS.md`. Use it as a checklist.
