# Agent Playbook Router

## Mandatory pre-flight — complete before any tool call

This is not background reading. Work through this checklist first.

- [ ] **1. Confirm the `EDA_for_agent` MCP server is available.** If it is not in your tool list, report this and stop — do not attempt RTL debugging without it.
- [ ] **2. Check the waveform time precision.** Call `get_signal_info` on any signal in the waveform and record the time unit. All time arguments must be converted to that unit before use (Rule 12).
- [ ] **3. Identify your task type** using the table below and open the matching playbook.
- [ ] **4. Follow that playbook as a step-by-step procedure.** Do not skip steps. Do not substitute source-code reading for a tool call.

If at any point two consecutive tool results are empty or nonsensical, stop and apply the "When results are untrustworthy" protocol below before continuing.

---

## Step 1 — Identify your task type

| Task type | You are trying to… | Playbook |
|---|---|---|
| **Waveform Browsing** | Observe signal values, transitions, patterns, or intervals in a waveform file | `01_WAVEFORM_BROWSING.md` |
| **Structural Exploration** | Understand how signals are connected in the RTL — drivers, loads, hierarchy | `02_STRUCTURAL_EXPLORATION.md` |
| **Signal Investigation** | Explain *why* a signal has a particular value at a particular time | `03_SIGNAL_INVESTIGATION.md` |
| **Root-Cause Analysis** | Find the source of a bug by combining structural and waveform analysis | `04_ROOT_CAUSE_ANALYSIS.md` |
| **Session Management** | Set up, organize, or switch debugging workspaces (cursors, bookmarks, signal groups) | `05_SESSION_MANAGEMENT.md` |
| **Supervised Debug** | Debug with a two-agent setup (Debugger + Supervisor) when single-agent attempts have failed or the model is prone to carelessness/hallucination | `06_SUPERVISED_DEBUG.md` |

## Step 2 — Routing rules

Follow these rules to pick the right playbook:

1. **"What value does signal X have at time T?"** → Waveform Browsing
2. **"Show me transitions / edges / patterns on signal X"** → Waveform Browsing
3. **"What drives signal X?" / "What does signal X fan out to?"** → Structural Exploration
4. **"What is the hierarchy under module Y?"** → Structural Exploration
5. **"Why does signal X have value V at time T?"** → Signal Investigation
6. **"Why did signal X change at time T?"** → Signal Investigation
7. **"What caused this bug / failure / assertion?"** → Root-Cause Analysis
8. **"Set a bookmark / create a signal group / switch session"** → Session Management
9. **"Debug failed with single agent" / "Agent keeps making mistakes" / "Use supervised mode"** → Supervised Debug

## Step 3 — Chaining playbooks

Some tasks require chaining. Common chains:

- **Triage a failure** → Session Management (set up workspace) → Waveform Browsing (observe symptoms) → Signal Investigation (explain the anomaly)
- **Full root-cause debug** → Session Management → Root-Cause Analysis (which internally chains Structural Exploration + Waveform Browsing)
- **Supervised root-cause debug** → Supervised Debug (wraps Root-Cause Analysis with a Supervisor agent reviewing each phase)
- **Design review / connectivity audit** → Structural Exploration → Waveform Browsing (verify structural understanding against simulation)

## Step 4 — Tool inventory by playbook

Quick reference of which tools belong to which playbook. **Only use the tools listed in your active playbook** unless you are explicitly chaining.

| Playbook | Primary tools |
|---|---|
| Waveform Browsing | `get_snapshot`, `get_value_at_time`, `find_edge`, `find_value_intervals`, `find_condition`, `get_transitions`, `get_signal_overview`, `analyze_pattern`, `list_signals`, `get_signal_info` |
| Structural Exploration | `rtl_trace` (`compile`, `trace`, `find`, `hier`), `rtl_trace_serve_start/query/stop` |
| Signal Investigation | `explain_signal_at_time`, `explain_edge_cause`, `trace_with_snapshot`, `rank_cone_by_time` |
| Root-Cause Analysis | All Signal Investigation tools + Waveform Browsing tools + Structural Exploration tools (used in a prescribed sequence) |
| Session Management | `create_session`, `list_sessions`, `get_session`, `switch_session`, `delete_session`, `set_cursor`, `move_cursor`, `get_cursor`, `create_bookmark`, `delete_bookmark`, `list_bookmarks`, `create_signal_group`, `update_signal_group`, `delete_signal_group`, `list_signal_groups` |
| Supervised Debug | All tools from Root-Cause Analysis, used by the Debugger agent; Supervisor uses no MCP tools (review only) |

---

## Important conventions (apply to all playbooks)

- **`TimeReference`** accepts an integer, `"Cursor"`, or `"BM_<bookmark_name>"`.
- **`radix`** accepts `"hex"`, `"bin"`, or `"dec"`.
- **`EdgeType`** accepts `"rise"`, `"rising"`, `"fall"`, `"falling"`, `"edge"`, `"any"`, or `"anyedge"`.
- **`Direction`** is `"forward"` or `"backward"`.
- **`TraceMode`** is `"drivers"` or `"loads"`.
- If `vcd_path` is omitted in a session-aware tool, the active Session's waveform is used.
- If no session exists yet, a `Default_Session` is created on first session-aware use.
- **Time values are in units of the waveform's time precision — not nanoseconds.** Before passing any integer time to any tool, call `get_signal_info` on any signal and check the reported timescale. For the common `` `timescale 1ns/1ps `` setup, precision is 1 ps, so 100 ns must be passed as `100000`. Guessing the wrong unit silently queries the wrong time. See Rule 12 in `rtl_debug_guide.md` for the full explanation.

---

## When results are empty or untrustworthy — stop, do not guess

If two or more consecutive tool calls return empty results, zero transitions, or values that contradict each other, **halt the current line of investigation before making another call**. Do not fill the gap with assumptions and continue as if the results were valid — a wrong assumption propagated through subsequent calls creates a false narrative that is harder to undo than the original problem.

Immediate actions:
1. Re-check the time precision (`get_signal_info`) — the most common cause of empty waveform results is a unit error (Rule 12).
2. Verify the signal path exists (`list_signals`, `rtl_trace find`).
3. Confirm the time range has simulation data (`get_signal_overview` with a wide range).

If two recovery attempts fail, stop and report: state what was called, what was returned, and why it is suspicious. See Rule 13 in `rtl_debug_guide.md` for the full stop-trigger table and protocol.

---

## When a tool fails

MCP tool failures are expected from time to time (wrong path, stale DB, signal name mismatch). The correct response is to **report and recover**, not to abandon the MCP layer.

| Symptom | First recovery step |
|---|---|
| `DB not found` | Compile the structural DB: `rtl_trace(args=["compile", ...])` |
| `Signal not found in waveform` | Use `rtl_trace find` to get the DB path; use `list_signals` to check the waveform namespace |
| `Waveform file not found` | Verify the path and extension (`.vcd`, `.fsdb`, `.fst`) |
| `Timeout` on `rtl_trace(...)` | Switch to serve mode: `rtl_trace_serve_start/query/stop` |
| Empty or truncated result | Try `--format json` and/or reduce `--depth` / `--max-nodes` |

**If a tool still fails after one targeted recovery attempt:** report the tool name, arguments, and full error message, then continue with other MCP tools rather than falling back to manual source reading. See Rule 10 in `rtl_debug_guide.md` for the full protocol.

---

## Advanced: `wave_agent_query`

`wave_agent_query(vcd_path, cmd, args)` is a low-level passthrough to the waveform CLI. **Do not use it unless a specific operation is not available through the higher-level tools listed above.** The named tools (`get_snapshot`, `find_edge`, etc.) are wrappers around this passthrough with better ergonomics, session awareness, and error handling. If you find yourself reaching for `wave_agent_query`, check whether one of the Waveform Browsing tools already does what you need.
