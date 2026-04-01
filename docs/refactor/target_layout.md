# Target Layout

## Current Layout (post-refactor, branch `refactor/split-monolith`)

```
auto_waveform_debugger/
  README.md                              # repo overview
  CLAUDE.md                              # AI agent instructions
  docs/
    MCP_SIGNATURES.md                    # MCP tool API contract (immutable)
    PROJ_DESC.md                         # architecture overview
    TEST.md                              # test documentation
    test_plan.md                         # test enhancement plan (completed)
    failure_history.md                   # regression history
    refactor/                            # refactor artifacts
  .gitignore
  clean_test_artifacts.sh

  standalone_trace/
    main.cc                              # thin arg parse + subcommand dispatch (52 lines)
    AssignmentUtils.cc                   # assignment text inference utilities
    db/
      GraphDb.h                          # public API declarations (6 Run* functions)
      GraphDbTypes.h                     # all type/struct/enum definitions (no slang AST deps)
      GraphDbInternals.h                 # shared internal function declarations (no slang AST deps)
      GraphDb.cc                         # shared infrastructure + compile-time indexing (~2450 lines)
    compile/
      Compiler.h / Compiler.cc           # RunCompile + MFCU helpers (~460 lines)
    query/
      TraceQuery.h / TraceQuery.cc       # RunTrace + trace-specific functions (~425 lines)
      HierQuery.h / HierQuery.cc         # RunHier + hier-specific functions
      FindQuery.h / FindQuery.cc         # RunFind + find-specific functions
      WhereIsQuery.h / WhereIsQuery.cc   # RunWhereInstance + whereis-specific functions
    serve/
      ServeLoop.h / ServeLoop.cc         # RunServe + SplitCommandLine
    CMakeLists.txt                       # compiles all 8 TUs
    third_party/                         # slang, fmt
    docs/
      Tech_Note.md                       # C++ internals: graph DB format, compile-time indexing
      LOCALTEST.md                       # local bring-up guide
      TODO.md                            # pending improvements
    tests/
      semantic_regression.py             # CTest-driven
      fixtures/
      assignment_utils_test.cc           # unit test

  waveform_explorer/
    src/
      main.cpp                           # CLI/daemon entry
      WaveDatabase.h / .cpp              # thin wrapper over FormatRegistry
      AgentAPI.h / .cpp                  # JSON query layer
      FormatAdapter.h                    # abstract format adapter interface
      FormatRegistry.h / .cpp            # extension -> adapter factory dispatch
      vcd/
        VcdAdapter.h / .cpp              # VCD format adapter
      fst/
        FstAdapter.h / .cpp              # FST format adapter
      fsdb/
        FsdbAdapter.h / .cpp             # FSDB format adapter
      cadence/
        CadenceAdapter.h                 # placeholder
      siemens/
        SiemensAdapter.h                 # placeholder
    CMakeLists.txt
    waveform_mcp.py                      # standalone waveform MCP
    docs/
      Tech_Note.md                       # C++ internals: WaveDatabase, AgentAPI, lazy FSDB loading
    tests/
      test_signal_overview.py
      test_waveform_commands.py

  agent_debug_automation/
    agent_debug_automation_mcp.py        # thin re-export wrapper with mock forwarding shim
    __init__.py                          # submodule imports in dependency order
    server.py                            # FastMCP app init
    tools.py                             # all @mcp.tool handler functions
    clients.py                           # wave_agent_cli and rtl_trace subprocess wrappers
    sessions.py                          # session/cursor/bookmark/signal group persistence
    mapping.py                           # signal path normalization, FSDB prefix lookup
    ranking.py                           # heuristic scoring functions
    models.py                            # constants, type aliases, data models
    docs/
      Tech_Note.md                       # internal design: process management, signal mapping, ranking
    tests/
      test_cross_linking.py              # 49 tests

  test_cases/                            # 27 integration test case dirs (tc01-tc27)
    run_all_tests.sh
    run_cross_link_tests.py
    CROSS_LINK_TEST_SUITE.md
    rtl_trace.db                         # pre-built structural DB (gitignored)
    wave.fsdb                            # pre-built waveform (gitignored)
    tc01_generate_loop/ ... tc27_history_failure_regression/

  docs/
    refactor/
      test_baseline.md                   # test baseline (post-refactor)
      target_layout.md                   # this file
      contracts.md                       # frozen external contracts
      test_taxonomy.md                   # test categories and run commands

  agent_debug_textbook/                  # AI agent coaching docs
    00_ROUTER.md
    01_WAVEFORM_BROWSING.md
    02_STRUCTURAL_EXPLORATION.md
    03_SIGNAL_INVESTIGATION.md
    04_ROOT_CAUSE_ANALYSIS.md
    05_SESSION_MANAGEMENT.md
    CASE_STUDY_01_nvdla_id_fifo.md
    CLAUDE.md
    EDA_USE.md
    rtl_debug_guide.md
```

## Key Changes Completed

| Component | Before | After |
|---|---|---|
| standalone_trace | 1 monolith `main.cc` (4,277 lines) | thin `main.cc` (52 lines) + 7 module TUs: db/GraphDb.cc, compile/Compiler.cc, query/*.cc, serve/ServeLoop.cc |
| waveform_explorer | 5 source files, format code mixed | FormatAdapter interface + FormatRegistry + per-format adapter folders (vcd/fst/fsdb/cadence/siemens) |
| agent_debug_automation | 1 monolith `agent_debug_automation_mcp.py` (2,347 lines) | 7 module files + backward-compat re-export shim with mock forwarding |

## Remaining Work

None — all phases complete.
