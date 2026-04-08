# auto_waveform_debugger

An AI-native platform for automated RTL waveform debugging. Combines **structural RTL tracing** with **temporal waveform evidence** so AI agents can point to a failing signal at a failing time and get ranked upstream-cause candidates.

## Architecture

Three components, each independently buildable:

| Component | Binary / Entry | Language | Role |
|---|---|---|---|
| `standalone_trace/` | `build/rtl_trace` | C++ (slang) | Parse/elaborate RTL, build a binary graph DB of structural connectivity, answer trace/hier/find queries |
| `waveform_explorer/` | `build/wave_agent_cli` | C++ | High-performance temporal query engine over VCD/FST/FSDB waveforms; returns structured JSON |
| `agent_debug_automation/` | `agent_debug_automation_mcp.py` | Python (FastMCP) | MCP orchestration: merges both backends, adds sessions, exposes cross-link tools |

```
                         ┌─────────────────────────────┐
   AI Agent (MCP)  ────> │  agent_debug_automation_mcp  │
                         │  (session, cross-link, rank) │
                         └──────┬──────────────┬────────┘
                                │              │
                    structural  │              │  temporal
                    trace       v              v  waveform
                          ┌──────────┐  ┌────────────────┐
                          │rtl_trace │  │wave_agent_cli  │
                          │ (graph   │  │ (VCD/FST/FSDB) │
                          │  DB)     │  │                │
                          └──────────┘  └────────────────┘
```

## Module Structure

### `standalone_trace/` (C++)

```
standalone_trace/
  main.cc                  # thin arg parse + subcommand dispatch
  AssignmentUtils.cc       # assignment text inference utilities
  compile/
    Compiler.cc/.h         # slang elaboration -> graph DB compile flow
  db/
    EntryPoints.h          # shared public entry points
    GraphDb.cc             # DB read/write and shared graph logic
    GraphDbInternals.h     # internal helpers and session flags
    GraphDbTypes.h         # shared structs and type definitions
  query/
    TraceQuery.cc/.h       # trace subcommand implementation
    HierQuery.cc/.h        # hierarchy query implementation
    FindQuery.cc/.h        # signal search implementation
    WhereIsQuery.cc/.h     # instance-location query implementation
  serve/
    ServeLoop.cc/.h        # interactive serve loop
  third_party/             # slang, fmt
  tests/                   # semantic_regression.py (CTest)
```

### `waveform_explorer/` (C++)

```
waveform_explorer/
  src/
    main.cpp               # CLI/daemon entry
    WaveDatabase.h/.cpp    # core backend (delegates to FormatRegistry)
    AgentAPI.h/.cpp        # JSON query layer
    FormatAdapter.h        # abstract format adapter interface
    FormatRegistry.h/.cpp  # extension -> adapter factory dispatch
    vcd/                   # VCD format adapter
    fst/                   # FST format adapter
    fsdb/                  # FSDB format adapter
    cadence/               # placeholder
    siemens/               # placeholder
  waveform_mcp.py          # standalone waveform MCP
  tests/                   # test_signal_overview.py, test_waveform_commands.py
```

### `agent_debug_automation/` (Python)

```
agent_debug_automation/
  agent_debug_automation_mcp.py  # thin re-export wrapper (backward compat shim)
  __init__.py                    # submodule imports in dependency order
  server.py                      # FastMCP app init
  tools.py                       # all @mcp.tool handler functions
  clients.py                     # subprocess wrappers (wave_agent_cli, rtl_trace)
  sessions.py                    # session/cursor/bookmark/signal group persistence
  mapping.py                     # signal path normalization, FSDB prefix lookup
  ranking.py                     # heuristic scoring functions
  models.py                      # constants, type aliases, data models
  expression_parser.py           # Verilog expression tokenizer + recursive descent parser
  expression_evaluator.py        # 4-state logic evaluator (0/1/x/z, event-driven)
  virtual_signals.py             # virtual signal service (create/update/delete, caching)
  tests/                         # test_cross_linking.py, test_expression_*, test_virtual_signals
```

## Build

```bash
# standalone_trace
(cd standalone_trace && cmake -B build -GNinja . && ninja -C build)

# waveform_explorer (add -DVERDI_HOME=... -DENABLE_FSDB=ON for FSDB)
(cd waveform_explorer && cmake -B build . && cmake --build build -j$(nproc))

# Python environment (repo root)
python3 -m venv .venv
.venv/bin/python3 -m pip install --upgrade pip
.venv/bin/python3 -m pip install -r requirements.txt
```

`requirements.txt` is the supported Python environment for the repo. It includes
the MCP runtime (`fastmcp`) plus the other Python dependencies used by the
project tests and tooling.

## Run MCP Service

```bash
.venv/bin/python3 agent_debug_automation/agent_debug_automation_mcp.py
.venv/bin/python3 -m agent_debug_automation.agent_debug_automation_mcp
```

## Tests

All tests use the project venv:

```bash
# standalone_trace (C++ ctest)
(cd standalone_trace && ctest --test-dir build --output-on-failure)

# waveform_explorer
./.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview
./.venv/bin/python3 -m unittest waveform_explorer.tests.test_waveform_commands

# agent_debug_automation
./.venv/bin/python3 -m unittest agent_debug_automation.tests.test_cross_linking

# Integration test cases (tc01-tc27)
./test_cases/run_all_tests.sh
```

## Key Documentation

| File | Content |
|---|---|
| `docs/MCP_SIGNATURES.md` | MCP tool API contract (immutable) |
| `docs/PROJ_DESC.md` | Architecture overview and core concepts |
| `docs/TEST.md` | Test documentation |
| `docs/test_plan.md` | Test enhancement plan (completed) |
| `docs/failure_history.md` | Regression history for past bugs |
| `docs/MAIN_AGENT_PARALLEL_PROMPT.md` | Refactor execution plan |
| `agent_debug_textbook/` | AI agent coaching docs for RTL debug |
| `docs/refactor/` | Refactor artifacts (baseline, contracts, taxonomy) |

## Component Docs

| Component | Docs |
|---|---|
| `standalone_trace/` | `README.md`, `docs/Tech_Note.md`, `docs/LOCALTEST.md`, `docs/TODO.md` |
| `waveform_explorer/` | `README.md`, `docs/Tech_Note.md` |
| `agent_debug_automation/` | `README.md`, `docs/Tech_Note.md` |
