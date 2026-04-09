# Agent Playbook Router

## Mandatory pre-flight — complete before any tool call

This is not background reading. Work through this checklist first.

- [ ] **1. Confirm the RTL debug MCP tool surface is available.** The server may be exposed as `EDA_for_agent`, `agent_debug_automation`, or another equivalent name, but the required tools must be present. At minimum, verify that you can see `get_signal_info`, `rtl_trace`, and `explain_signal_at_time`. If this tool surface is not available, report this and stop — do not attempt RTL debugging without it.
- [ ] **2. Verify that all EDA tools are pre-approved in the project allow list.** A tool that appears in the MCP tool list can still be silently denied at runtime if it is missing from `settings.local.json`. A mid-session denial forces inferior workarounds that inflate tool-call counts and reduce analysis quality. Before any debug session: confirm all EDA_for_agent tools are in the project allow list. If starting a new project, add all tools up front rather than building the allow list incrementally.
- [ ] **3. Determine whether a compiled structural DB already exists and what path to use.** For any task that needs structural tracing or root-cause analysis, confirm the `rtl_trace.db` path before issuing structural calls. If local context does not clearly reveal an existing DB, ask the user to confirm whether one has already been generated. If no DB exists, ask the user for the exact command or flow used in this project to generate it. Do not guess compile flags or invent a filelist/top-module flow when the project-specific compile recipe is unknown.
- [ ] **4. Check the waveform time precision.** Call `get_signal_info` on any signal in the waveform and record the time unit. All time arguments must be converted to that unit before use (Rule 12).
- [ ] **5. Identify your task type** using the table below and open the matching playbook.
- [ ] **6. Follow that playbook as a step-by-step procedure.** Do not skip steps. Do not substitute source-code reading for a tool call.

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
| **Virtual Signals** | Create derived signals (compound conditions, bus slices/concatenations) so that browsing and search tools can operate on them directly | `07_VIRTUAL_SIGNALS.md` |

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
10. **"Create a handshake / condition / error-flag signal" / "Slice or reassemble a bus" / "Define a derived observable"** → Virtual Signals

## Step 3 — Chaining playbooks

Some tasks require chaining. Common chains:

- **Triage a failure** → Session Management (set up workspace) → Waveform Browsing (observe symptoms) → Signal Investigation (explain the anomaly)
- **Full root-cause debug** → Session Management → Root-Cause Analysis (which internally chains Structural Exploration + Waveform Browsing, and may chain Virtual Signals for reusable protocol events)
- **Supervised root-cause debug** → Supervised Debug (wraps Root-Cause Analysis with a Supervisor agent reviewing each phase)
- **Design review / connectivity audit** → Structural Exploration → Waveform Browsing (verify structural understanding against simulation)
- **Protocol or condition search** → Virtual Signals (define the handshake or condition as a named signal) → Waveform Browsing (search and count occurrences) → Signal Investigation (trace the real drivers of an anomalous event)

## Step 4 — Tool inventory by playbook

Quick reference of which tools belong to which playbook. Use the tools listed in your active playbook by default. `Session Management` is always allowed when you need cursor/bookmark/group state, and `Root-Cause Analysis` may chain `Virtual Signals` for reusable compound events.

| Playbook | Primary tools |
|---|---|
| Waveform Browsing | `get_snapshot`, `get_value_at_time`, `find_edge`, `find_value_intervals`, `find_condition`, `get_transitions`, `count_transitions`, `dump_waveform_data`, `get_signal_overview`, `analyze_pattern`, `list_signals`, `get_signal_info` |
| Structural Exploration | `rtl_trace` (`compile`, `trace`, `find`, `hier`, `whereis-instance`), `rtl_trace_serve_start/query/stop` |
| Signal Investigation | `explain_signal_at_time`, `explain_edge_cause`, `trace_with_snapshot`, `rank_cone_by_time` |
| Root-Cause Analysis | All Signal Investigation tools + Waveform Browsing tools + Structural Exploration tools + Session Management tools (used in a prescribed sequence); may also chain Virtual Signals for reusable compound events |
| Session Management | `create_session`, `list_sessions`, `get_session`, `switch_session`, `delete_session`, `set_cursor`, `move_cursor`, `get_cursor`, `create_bookmark`, `delete_bookmark`, `list_bookmarks`, `create_signal_group`, `update_signal_group`, `delete_signal_group`, `list_signal_groups` |
| Virtual Signals | `create_signal_expression`, `update_signal_expression`, `delete_signal_expression`, `list_signal_expressions`, `create_bus_concat`, `create_bus_slice`, `create_bus_slices`, `create_reversed_bus` — plus all Waveform Browsing tools (they accept virtual signal names transparently) |
| Supervised Debug | All tools from Root-Cause Analysis, used by the Debugger agent; Supervisor uses no MCP tools (review only) |

---

## Important conventions (apply to all playbooks)

- **`TimeReference`** accepts an integer, `"Cursor"`, or `"BM_<bookmark_name>"`.
- **`radix`** accepts `"hex"`, `"bin"`, or `"dec"`.
- **`EdgeType`** accepts `"posedge"` / `"rising"` / `"rise"` (rising edge), `"negedge"` / `"falling"` / `"fall"` (falling edge), `"edge"` / `"any"` / `"anyedge"` (any change). The `posedge`/`negedge` forms and `rising`/`falling` forms are interchangeable aliases.
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

## When reviewing simulation logs

Do not dump full log files (`test.log`, `sim.log`, `comp.log`, etc.) into context. Start with targeted extraction:

- `grep` for the failing error, assertion text, `UVM_ERROR`, `UVM_FATAL`, or test name
- `tail` for the end-of-run status and final failure summary
- `head` for setup/configuration context when relevant

Only widen to a small surrounding slice if the first extraction is insufficient. Summarize the important lines instead of carrying the whole log forward.

---

## Advanced: `wave_agent_query`

`wave_agent_query(vcd_path, cmd, args)` is a low-level passthrough to the waveform CLI. **Do not use it unless a specific operation is not available through the higher-level tools listed above.** The named tools (`get_snapshot`, `find_edge`, etc.) are wrappers around this passthrough with better ergonomics, session awareness, and error handling. If you find yourself reaching for `wave_agent_query`, check whether one of the Waveform Browsing tools already does what you need.
