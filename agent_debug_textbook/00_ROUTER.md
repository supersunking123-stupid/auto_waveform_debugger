# Agent Playbook Router

This document helps you decide **which playbook to follow** based on your current task. Read the task descriptions below, pick the best match, and follow that playbook. If your task spans multiple categories, chain the playbooks in the order listed.

---

## Step 1 — Identify your task type

| Task type | You are trying to… | Playbook |
|---|---|---|
| **Waveform Browsing** | Observe signal values, transitions, patterns, or intervals in a waveform file | `01_WAVEFORM_BROWSING.md` |
| **Structural Exploration** | Understand how signals are connected in the RTL — drivers, loads, hierarchy | `02_STRUCTURAL_EXPLORATION.md` |
| **Signal Investigation** | Explain *why* a signal has a particular value at a particular time | `03_SIGNAL_INVESTIGATION.md` |
| **Root-Cause Analysis** | Find the source of a bug by combining structural and waveform analysis | `04_ROOT_CAUSE_ANALYSIS.md` |
| **Session Management** | Set up, organize, or switch debugging workspaces (cursors, bookmarks, signal groups) | `05_SESSION_MANAGEMENT.md` |

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

## Step 3 — Chaining playbooks

Some tasks require chaining. Common chains:

- **Triage a failure** → Session Management (set up workspace) → Waveform Browsing (observe symptoms) → Signal Investigation (explain the anomaly)
- **Full root-cause debug** → Session Management → Root-Cause Analysis (which internally chains Structural Exploration + Waveform Browsing)
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

---

## Important conventions (apply to all playbooks)

- **`TimeReference`** accepts an integer, `"Cursor"`, or `"BM_<bookmark_name>"`.
- **`radix`** accepts `"hex"`, `"bin"`, or `"dec"`.
- **`EdgeType`** accepts `"rise"`, `"rising"`, `"fall"`, `"falling"`, `"edge"`, `"any"`, or `"anyedge"`.
- **`Direction`** is `"forward"` or `"backward"`.
- **`TraceMode`** is `"drivers"` or `"loads"`.
- If `vcd_path` is omitted in a session-aware tool, the active Session's waveform is used.
- If no session exists yet, a `Default_Session` is created on first session-aware use.

---

## Advanced: `wave_agent_query`

`wave_agent_query(vcd_path, cmd, args)` is a low-level passthrough to the waveform CLI. **Do not use it unless a specific operation is not available through the higher-level tools listed above.** The named tools (`get_snapshot`, `find_edge`, etc.) are wrappers around this passthrough with better ergonomics, session awareness, and error handling. If you find yourself reaching for `wave_agent_query`, check whether one of the Waveform Browsing tools already does what you need.
