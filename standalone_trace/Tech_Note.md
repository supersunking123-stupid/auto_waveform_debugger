# standalone_trace Tech Note

## Purpose

`standalone_trace` builds a structural RTL connectivity database from Verilog/SystemVerilog and answers offline queries about:
- who drives a signal
- what a signal drives
- where a signal sits in the instance hierarchy
- fuzzy signal-name lookup

The implementation is concentrated in `standalone_trace/main.cc`.

## High-level architecture

There are two phases:

1. Compile phase
- Parse and elaborate RTL with `slang`.
- Collect traceable signals and instance hierarchy.
- For each signal, derive driver and load endpoint records.
- Serialize the result into a compact binary graph DB.

2. Query phase
- Load the graph DB into a `TraceSession`.
- Answer `trace`, `find`, `hier`, and `serve` commands without re-elaborating RTL.

The source code is intentionally monolithic. Most important behavior lives in one translation unit.

## Important data types

### Compile-time trace representation

- `ExprTraceResult`
  - Temporary representation of an assignment-side trace result.
  - Carries the matched named value expression, assignment node, selectors, and contextual LHS information.

- `TraceResult`
  - `std::variant<const PortSymbol*, ExprTraceResult>`
  - One trace hit is either a port hop or an expression endpoint.

- `EndpointRecord`
  - User-visible endpoint payload for a trace result.
  - Key fields:
    - `kind`: `port` or `expr`
    - `path`, `file`, `line`
    - `direction`
    - `assignment_text`
    - `bit_map`, `bit_map_approximate`
    - `lhs_signals`, `rhs_signals`

- `SignalRecord`
  - Holds `drivers` and `loads` for one signal.

- `BodyTraceIndex`
  - Per-instance-body cache used during compile.
  - Maps a symbol to precomputed `drivers` and `loads` trace hits inside that body.

- `TraceCompileCache`
  - Memoization layer for LHS/RHS extraction and body trace indexes.
  - This is important because compile would otherwise revisit the same AST subtrees repeatedly.

### Database types

- `TraceDb`
  - In-memory compatibility DB keyed by string paths.
  - Holds `signals`, `hierarchy`, `global_nets`, and string pools.
  - Useful for query logic and source-text recovery.

- `GraphDb`
  - Compact binary graph form used for persisted DB files.
  - Stores interned strings plus flat arrays for signals, endpoints, hierarchy, reverse references, and global nets.

- `GraphSignalRecord`, `GraphEndpointRecord`, `GraphHierarchyRecord`, `GraphGlobalNetRecord`
  - Flat storage records for the binary DB.
  - The code converts rich compile-time records into these dense arrays in `SaveGraphDb(...)`.

- `TraceSession`
  - Runtime query session.
  - Owns the loaded DB and session caches such as:
    - `graph`
    - `signal_name_to_id`
    - `materialized_signal_records`
    - `source_file_cache`
  - This is the main object reused by `serve`.

### Query option/result types

- `TraceOptions`
  - Parsed options for `trace`.
  - Includes `mode`, target signal, cone expansion, regex filters, depth/node limits, and output format.

- `TraceRunResult`
  - Trace result payload with `endpoints`, `stops`, and `visited_count`.

- `TraceStop`
  - Records why traversal stopped for a path, for example depth limit or missing signal.

- `HierOptions`, `HierRunResult`, `HierTreeNode`
  - Hierarchy query configuration and result payloads.

- `FindOptions`
  - Name-search options for `find`.

## Major working flows

### 1. `compile`

Main entry: `RunCompile(...)`

Working flow:
- Parse command-line flags such as `--db`, `--incremental`, `--mfcu`, `--partition-budget`, `--compile-log`.
- Require `--top` in passthrough slang args.
- Optionally rewrite filelist inputs for grouped MFCU handling.
- Compute a compile fingerprint from filelists, source files, and mtimes.
- If `--incremental` and fingerprint matches `.meta`, return early.
- Use `slang::driver::Driver` to parse and elaborate all RTL.
- Collect traceable symbols with `CollectTraceableSymbols(...)`.
- Collect instance hierarchy with `CollectInstanceHierarchy(...)`.
- Plan partitions if requested.
- Emit the binary graph DB with `SaveGraphDb(...)`.
- Write the fingerprint to `<db>.meta`.

Important supporting functions:
- `BuildMfcuArgs(...)`
- `ComputeCompileFingerprint(...)`
- `BuildSignalRecord(...)`
- `SaveGraphDb(...)`
- `LoadGraphDb(...)`

### 2. Driver/load extraction during compile

The compile phase derives structure from `slang` AST nodes.

Important functions:
- `CollectRhsSignals(...)`
- `CollectLhsSignals(...)`
- `CollectLhsSignalsFromStatement(...)`
- `GetCachedLhsSignals(...)`
- `GetCachedRhsSignals(...)`
- `ResolveTraceResult(...)`
- `BuildSignalRecord(...)`

Important mechanism:
- `BodyTraceIndexBuilder<DRIVERS>` visits each instance body and caches how assignments and port connections relate symbols.
- `ComputeIndexedTraceResults<...>` reuses that per-body index instead of walking the AST from scratch for every signal.

This is the core compile-time optimization. Without it, DB construction would not scale.

### 3. `trace`

Main entry: `RunTrace(...)` -> `RunTraceWithSession(...)` -> `RunTraceQuery(...)`

Working flow:
- Parse `TraceOptions`.
- Open or reuse a `TraceSession` backed by a graph DB.
- Resolve the target signal and optional bit/range selection.
- Try the global-net fast path for common clock/reset patterns.
- Traverse signal references recursively according to `mode`:
  - `drivers`: walk backward through RHS/source-side relationships
  - `loads`: walk forward through LHS/sink-side relationships
- Apply query controls:
  - `--cone-level`
  - `--prefer-port-hop`
  - `--depth`
  - `--max-nodes`
  - `--include`, `--exclude`, `--stop-at`
- Materialize assignment source snippets on demand.
- Emit text or JSON.

Important implementation details:
- `MergeEndpointBitRanges(...)` merges adjacent exact bit slices into compact ranges.
- `MaterializeAssignmentTexts(...)` uses cached source files and source offsets, so the DB can stay compact and assignment text can still be recovered on demand.
- `TraceStop` records are first-class output and are part of how bounded traversal is debugged.

### 4. `hier`

Main entry: `RunHier(...)` -> `RunHierWithSession(...)`

Working flow:
- Build or load hierarchy from the DB.
- Walk subtree from an optional root.
- Enforce depth and node limits.
- Emit either text or JSON tree form.

Relevant helpers:
- `CollectInstanceHierarchy(...)`
- `BuildHierarchyFromSignals(...)`

### 5. `find`

Main entry: `RunFind(...)` -> `RunFindWithSession(...)`

Working flow:
- Search signal names in the loaded session.
- Support substring or regex mode.
- Limit output count.
- Return text or JSON.

If an exact trace target is missing, `TopSuggestions(...)` is used to produce edit-distance-based alternatives.

### 6. `serve`

Main entry: `RunServe(...)`

Purpose:
- Keep one DB loaded in memory.
- Accept line-oriented commands for repeated `trace`, `find`, and `hier` queries.
- Terminate each response with `<<END>>`.

This is the preferred mode for large designs because it avoids reloading the graph DB per query.

## Important implementation ideas

### Binary graph DB instead of raw JSON DB

`SaveGraphDb(...)` writes a compact binary format with a `GraphDbFileHeader` and flat arrays. This is the main scaling choice in the tool:
- smaller on disk
- faster to load
- string interning reduces duplication
- reverse-reference ranges make traversal faster

### Lazy materialization in query sessions

The runtime does not eagerly inflate every `SignalRecord` from the graph DB. `TraceSession` materializes only the pieces touched by the current query and caches them in `materialized_signal_records`.

### Global-net shortcut

Clock/reset-like fanout can be compacted into `global_nets` instead of storing massive repeated endpoint sets. `TryRunGlobalNetFastPath(...)` uses that compressed representation for common trace cases.

### Source slicing is offset-based

For assignment text, compile stores source offsets instead of full source text. Query-time recovery uses:
- `assignment_start`
- `assignment_end`
- `source_file_cache`

This keeps DB size under control.

## Where to start if you need to change behavior

- Change compile-time connectivity extraction:
  - `BodyTraceIndexBuilder`
  - `CollectLhsSignals*`
  - `CollectRhsSignals*`
  - `ResolveTraceResult(...)`

- Change persisted DB shape:
  - `Graph*Record` structs
  - `SaveGraphDb(...)`
  - `LoadGraphDb(...)`
  - `GraphDbFileHeader`

- Change runtime trace traversal:
  - `RunTraceQuery(...)`
  - signal/ref lookup helpers around `TraceSession`

- Change CLI or serve behavior:
  - `RunCompile(...)`, `RunTrace(...)`, `RunHier(...)`, `RunFind(...)`, `RunServe(...)`, `main(...)`

## Practical mental model

Think of `standalone_trace` as:
- `slang` elaboration front-end
- AST-to-connectivity extraction pass
- compact graph DB serializer
- query engine over that graph DB

The compile phase is where semantic meaning is discovered. The query phase is mostly graph traversal and formatting.
