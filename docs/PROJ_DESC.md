# auto_waveform_debugger

An AI-native platform for automated RTL waveform debugging. It combines **structural RTL tracing** with **temporal waveform evidence** so that AI agents can point to a failing signal at a failing time and get ranked upstream-cause candidates.

## Architecture

Three components, each independently buildable:

| Component | Binary / Entry | Language | Role |
|---|---|---|---|
| `standalone_trace/` | `build/rtl_trace` | C++ (slang) | Parse/elaborate RTL, build a binary graph DB of structural connectivity (drivers/loads), answer offline trace queries |
| `waveform_explorer/` | `build/wave_agent_cli` | C++ | High-performance temporal query engine over VCD/FST/FSDB waveforms; returns structured JSON, not visual waves |
| `agent_debug_automation/` | `agent_debug_automation_mcp.py` | Python (FastMCP) | MCP orchestration layer: merges both backends, adds session model, and exposes cross-link tools that combine structural cone + waveform evidence |

```
                         ┌─────────────────────────────┐
   AI Agent (MCP)  ────▶ │  agent_debug_automation_mcp  │
                         │  (session, cross-link, rank) │
                         └──────┬──────────────┬────────┘
                                │              │
                    structural  │              │  temporal
                    trace       ▼              ▼  waveform
                          ┌──────────┐  ┌────────────────┐
                          │rtl_trace │  │wave_agent_cli  │
                          │ (graph   │  │ (VCD/FST/FSDB) │
                          │  DB)     │  │                │
                          └──────────┘  └────────────────┘
```

## Build

```bash
# standalone_trace
cd standalone_trace && cmake -B build -GNinja . && ninja -C build

# waveform_explorer (add -DVERDI_HOME=... -DENABLE_FSDB=ON for FSDB)
cd waveform_explorer && cmake -B build . && cmake --build build -j$(nproc)

# Python runtime
uv pip install fastmcp
```

## Run MCP Service

```bash
python -m agent_debug_automation.agent_debug_automation_mcp
```

## Tests

```bash
python3 -m unittest waveform_explorer.tests.test_signal_overview
python3 -m unittest agent_debug_automation.tests.test_cross_linking
cd standalone_trace && ctest --test-dir build --output-on-failure
```

## Key Documentation

| File | Content |
|---|---|
| `MCP_SIGNATURES.md` | Complete MCP tool signatures (the API contract) |
| `agent_debug_automation/README.md` | Orchestration layer usage, session model, cross-link tool semantics |
| `agent_debug_automation/Tech_Note.md` | Internal design: process management, signal mapping, ranking heuristics, where to change behavior |
| `waveform_explorer/README.md` | Waveform query commands, FSDB notes, build/run instructions |
| `waveform_explorer/Tech_Note.md` | C++ internals: WaveDatabase, AgentAPI, lazy FSDB loading, path normalization |
| `standalone_trace/README.md` | `rtl_trace` CLI usage: compile, trace, find, hier, serve |
| `standalone_trace/Tech_Note.md` | C++ internals: graph DB format, compile-time indexing, memory optimization |
| `standalone_trace/LOCALTEST.md` | Local bring-up guide with Cores-VeeR-EH1 example |
| `failure_history.md` | Regression history for past FSDB/waveform bugs |

## Key Source Files

### `agent_debug_automation/`

| File | Purpose |
|---|---|
| `agent_debug_automation_mcp.py` | Single-file MCP server: session persistence, backend process management, signal mapping, cross-link tools, ranking |
| `tests/test_cross_linking.py` | Cross-link regression (timer_tb VCD + optional NVDLA FSDB) |

### `waveform_explorer/`

| File | Purpose |
|---|---|
| `src/WaveDatabase.h` / `.cpp` | Core waveform backend: load VCD/FST/FSDB, per-signal transition store, value-at-time lookup, path normalization |
| `src/AgentAPI.h` / `.cpp` | JSON query layer: snapshot, edge search, transitions, overview, pattern analysis, condition search |
| `src/main.cpp` | CLI/daemon entry: one-shot or persistent stdin-line protocol |
| `waveform_mcp.py` | Standalone waveform MCP (subset of the merged MCP) |
| `tests/test_signal_overview.py` | Signal overview regression tests |

### `standalone_trace/`

| File | Purpose |
|---|---|
| `main.cc` | Monolithic source: compile (slang elaboration + graph DB build), trace/find/hier/serve queries |
| `tests/semantic_regression.py` | CTest-driven semantic regression |
| `tests/fixtures/` | Small RTL test fixtures |

## Core Concepts

- **Graph DB** (`rtl_trace.db`): Binary connectivity database built from RTL. Stores drivers, loads, hierarchy, and assignment reverse references.
- **Waveform Daemon**: Long-lived `wave_agent_cli` process for one waveform; JSON query per line.
- **Session**: Persisted waveform view (cursor, bookmarks, signal groups) bound to one waveform file.
- **Cross-link tools**: `trace_with_snapshot`, `explain_signal_at_time`, `rank_cone_by_time`, `explain_edge_cause` — combine structural trace with waveform evidence at a specific time.
- **Signal mapping**: Structural paths (from graph DB) are mapped to waveform paths via exact match, `TOP.` variant, FSDB prefix-scoped page search, or packed-vector fallback.
- **Ranking**: Heuristic scoring by closeness-to-T, activity, and stuckness. Not symbolic Boolean solving.
