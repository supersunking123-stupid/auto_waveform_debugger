# standalone_trace

`rtl_trace` is a slimmed-down RTL trace tool extracted from simview. It keeps only:

- reading and elaborating Verilog / SystemVerilog designs (based on `slang`)
- building drivers / loads indexes per instance body during `compile`, with cache reuse to avoid repeated whole-AST walks for each signal
- tracing `drivers` / `loads` for a specified hierarchical signal
- recursively continuing traces across instance ports (`input` and `output` ports both keep expanding)
- preferring logic-expression locations in trace results (expressions inside assignments / `always` blocks) instead of stopping at port boundaries
- under `--mode loads`, printing the assignment LHS hierarchical path when the hit is a logic expression
- under `--mode drivers`, fast backtracking for interface-member / bind cases (such as `if0.sig` or `mon_if.master_if[0].rlast`) using an assignment-LHS reverse index, without degrading to a full-DB scan
- multi-hop tracing controls: `--depth`, `--max-nodes`, `--stop-at`
- path filters: `--include`, `--exclude`
- structured output: `--format json`
- signal search: `find --query ...` (with fuzzy suggestions)
- automatic `--timescale 1ns/1ps` insertion by default if the command line does not explicitly provide `--timescale`

This directory already vendors the required dependencies (`third_party/slang`, `third_party/fmt`).
Copying the entire `standalone_trace/` directory is enough to build it independently, without depending on the parent simview project.

For local bring-up and testing (based on `Cores-VeeR-EH1`), see:
- [LOCALTEST.md](./LOCALTEST.md)

## Build

This is a standalone project. Build it directly under `standalone_trace`:

```bash
cd standalone_trace
cmake -B build -GNinja .
ninja -C build
```

## Usage

```bash
build/rtl_trace compile --db rtl_trace.db --top <top_module> [--incremental] [--relax-defparam] [--mfcu] [--low-mem] [--partition-budget N] [--compile-log <file>] [slang source args...]
build/rtl_trace trace --db rtl_trace.db --mode drivers --signal top.u0.sig[3] \
  [--cone-level N] [--prefer-port-hop] [--depth N] [--max-nodes N] [--include RE] [--exclude RE] [--stop-at RE] [--format text|json]
build/rtl_trace trace --db rtl_trace.db --mode loads --signal top.u0.sig[7:4] \
  [--cone-level N] [--prefer-port-hop] [--depth N] [--max-nodes N] [--include RE] [--exclude RE] [--stop-at RE] [--format text|json]
build/rtl_trace hier --db rtl_trace.db [--root top.u0] [--depth N] [--max-nodes N] [--format text|json] [--show-source]
build/rtl_trace whereis-instance --db rtl_trace.db --instance top.u0 [--format text|json]
build/rtl_trace find --db rtl_trace.db --query "foo.bar" [--regex] [--limit N] [--format text|json]
build/rtl_trace serve [--db rtl_trace.db]
```

Notes:
- The `compile` phase parses / elaborates RTL and generates a binary graph DB (default `rtl_trace.db`).
- `--top <top_module>` is required. The tool reports an error and exits if it is missing.
- `compile --incremental` reuses an existing DB based on input fingerprints and skips recompilation on a cache hit.
- `compile --relax-defparam` relaxes defparam-related errors (for example, unresolved cross-hierarchy defparams) so a usable DB can still be generated.
- `compile --mfcu` uses grouped MFCU mode: source files from each `-f` filelist are merged into one compilation unit, and source files passed directly on the command line are merged into another compilation unit, instead of merging all inputs into one unit.
- `compile --low-mem` enables low-memory mode: during `SaveGraphDb`, `TraceCompileCache` is actively pruned once every 200 module bodies. On large designs such as NVDLA this can save about **1.1 GB** of peak memory, at the cost of about **10-15s** of additional wall time due to cache-miss re-parsing.
- `compile --partition-budget N` partitions the DB build by instance-tree budget. The log prints the partitioning result and per-partition progress.
- `compile --compile-log <file>` writes key compile-stage steps and partition information into a log file while still printing to the console. During DB generation it also records `save_graph_db` sub-step timings such as `build_graph` and `write_file`.
- The `trace` phase only queries the DB and does not parse RTL again.
- The current DB format is a binary graph DB.
- The current graph DB file version is `v3`.
- The old V7 / V8 text DB format is no longer supported. Re-run `compile` to generate the current graph DB.
- If you previously generated an old graph DB (`v1`), you also need to re-run `compile`, because the new version persists assignment-LHS reverse references to accelerate `drivers` queries on interface members.
- `serve` is intended for interactive debugging on large designs: the DB is loaded once, and later `find` / `trace` / `hier` commands reuse the resident session.
- For high-fanout clock / reset networks, `compile` compresses fanout into a dedicated compact table to reduce normal `loads` detail storage. `trace` queries on those networks consult the compact table first.
- For interface members / bind assignments, `compile` also persists a reverse-reference table from assignment LHS to source signals, so `drivers` queries such as `mon_if.master_if[0].rlast` work directly even in a fresh standalone CLI process, without a query-time full scan.
- While generating the DB, `compile` caches each signal's resolved result and reuses it across both the string-collection and final-write stages, avoiding repeated resolution of the same signal.
- If you do not pass `--timescale` during `compile`, the tool automatically uses `1ns/1ps` to avoid mixed / missing timescale errors.
- If `--timescale` is explicitly provided, the user-supplied value is used.
- If you want the latest cross-port recursive results and `loads` `lhs` information, re-run `compile` to generate a new DB.

Additional `trace` output details:
- `mode=drivers`: if an expression can be resolved to an assignment, the tool prints:
  - `assign <assignment_text>`
  - `lhs <hierarchical_path>` (when matched through the assignment-LHS reverse index)
  - `rhs <hierarchical_path>` (RHS signal list)
- `mode=loads`: if an expression can be associated with an assignment, the tool prints:
  - `lhs <hierarchical_path>` (LHS signal list)
- `--format json`: output includes `summary`, `endpoints`, and `stops`, which is convenient for agents / scripts.
- Bit-select queries are supported: `--signal top.sig[3]`, `--signal top.sig[7:4]`.
- `hier`: prints the instance hierarchy tree (supports `--root`, `--depth`, `--max-nodes`, `--format json`, `--show-source`).
- `hier --show-source`: also prints the instantiated module definition file and line when available in the DB.
- `whereis-instance`: quick lookup for one hierarchy node; prints the instance path, module type, and definition source location when available.
- `--cone-level N`: automatic logic-cone expansion depth (default 1; `drivers` walks backward along RHS, `loads` walks forward along LHS).
- `--prefer-port-hop`: when the hit is a port-binding expression and there is no expandable RHS / LHS, prefer continuing the trace across the port bridge.
- `serve`: starts the interactive backend. Commands are entered one line at a time, and each response ends with `<<END>>`. Interactive mode supports `whereis-instance` too.

## TODO

- Support full cross-hierarchy `defparam` semantics. The current implementation only provides `--relax-defparam` as a compatibility relaxation mode.

## Automated Semantic Regression

The project includes CTest semantic regression cases covering cross-port tracing, `loads` context LHS, bit filtering, depth / node stops, `find` suggestions, incremental cache hits, optional hierarchy-source reporting, and `whereis-instance` lookup:

```bash
cd standalone_trace
cmake -B build -GNinja .
ninja -C build
ctest --test-dir build --output-on-failure
```

Example (under `example/simview_trace`):

```bash
/path/to/standalone_trace/build/rtl_trace compile \
  --db trace.db \
  --top timer_tb \
  -f simview.f

/path/to/standalone_trace/build/rtl_trace trace \
  --db trace.db \
  --mode drivers \
  --signal timer_tb.timeout
```
