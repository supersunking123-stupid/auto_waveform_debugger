# Playbook 02 — Structural Exploration

**Role:** You are a design navigator. Your job is to understand how the RTL is structurally connected — what drives what, how instances are organized — without looking at waveform data.

**When to use:** You need to answer questions about RTL connectivity, signal fanin/fanout, or design hierarchy. No simulation values are involved.

**Boundary:** This playbook is for targeted structural questions. If what you really need is a persistent architecture map of the full design or of a complex subsystem, switch to `08_DESIGN_MAPPING.md` and use the `rtl-crawler-multi-agent` skill instead of manually expanding the hierarchy forever.

---

## Tools available in this playbook

| Tool | Purpose |
|---|---|
| `rtl_trace` (compile) | Build the structural database from RTL source files |
| `rtl_trace` (find) | Search for signals by name or pattern |
| `rtl_trace` (hier) | Browse the instance hierarchy; `--show-source` annotates each node with its RTL definition file and line |
| `rtl_trace` (trace) | Trace structural drivers or loads of a signal |
| `rtl_trace` (whereis-instance) | Quick lookup: given one instance path, return its module type and RTL source file/line |
| `rtl_trace_serve_start` | Start a persistent serve session for multiple queries |
| `rtl_trace_serve_query` | Send a query to a running serve session |
| `rtl_trace_serve_stop` | Stop a serve session |

---

## Prerequisite: the structural database

All structural exploration requires a compiled database.

Before compiling, determine whether a usable DB already exists and what path to use:

1. Check local context for an existing `rtl_trace.db` path.
2. If local context is unclear, ask the user whether a DB has already been generated and which path to use.
3. If no DB exists yet, ask the user for the exact command or flow used in this project to generate it.

Do **not** guess the project's compile recipe when the filelist / top-module / required flags are unknown.

If a DB truly does not exist yet and you have the correct project-specific compile flow, compile first:

```python
rtl_trace(args=["compile", "--db", "rtl_trace.db", "--top", "top_module", "-f", "files.f"])
```

Common compile flags for tricky designs:
- `--incremental` — reuse previous compilation, only recompile changed files.
- `--relax-defparam` — tolerate non-standard defparam usage.
- `--mfcu` — multi-file compilation unit mode.
- `--low-mem` — reduce memory usage for very large designs.
- `--compat vcs` — match VCS-style compatibility for flows that depend on vendor or legacy source behavior.
- `-Wno-duplicate-definition` — needed in some vendor-library flows that intentionally duplicate helper modules across cells.

**Do not proceed to other structural commands until you know the correct DB path or compile succeeds.**

---

## Decision tree

```
What do you need to know?
│
├─ "What instances exist under module X?"
│   └─ rtl_trace(args=["hier", "--db", "rtl_trace.db", "--root", "top.x", "--depth", "2"])
│
├─ "What instances exist AND which source files define them?"
│   └─ rtl_trace(args=["hier", "--db", "rtl_trace.db", "--root", "top.x", "--depth", "2",
│                       "--show-source", "--format", "json"])
│
├─ "Which source file defines instance top.x.u_foo? What module is it?"
│   └─ rtl_trace(args=["whereis-instance", "--db", "rtl_trace.db",
│                       "--instance", "top.x.u_foo", "--format", "json"])
│
├─ "Does a signal named 'foo' exist? Where?"
│   └─ rtl_trace(args=["find", "--db", "rtl_trace.db", "--query", "foo"])
│
├─ "What drives signal X?" (structural fanin)
│   └─ rtl_trace(args=["trace", "--db", "rtl_trace.db", "--mode", "drivers", "--signal", "top.x"])
│
├─ "What does signal X drive?" (structural fanout)
│   └─ rtl_trace(args=["trace", "--db", "rtl_trace.db", "--mode", "loads", "--signal", "top.x"])
│
└─ "I need to run many structural queries interactively"
    └─ Use serve mode (see below)
```

---

## Common sequences

### Sequence A — Understand an unfamiliar module

1. `rtl_trace(args=["hier", "--db", "rtl_trace.db", "--root", "top", "--depth", "2", "--format", "json"])` — see the top-level structure.
2. Drill into interesting submodules: `rtl_trace(args=["hier", "--db", "rtl_trace.db", "--root", "top.u_interesting", "--depth", "2"])`.
3. `rtl_trace(args=["find", "--db", "rtl_trace.db", "--query", "interesting_signal", "--limit", "20"])` — locate specific signals.

If you still cannot state the major child blocks, interfaces, and boundaries after a few targeted hierarchy queries, stop and switch to `08_DESIGN_MAPPING.md`.

### Sequence B — Trace a signal's connectivity

1. `rtl_trace(args=["trace", "--db", "rtl_trace.db", "--mode", "drivers", "--signal", "top.u0.data_out"])` — what feeds this signal?
2. If the cone is too large, constrain it:
   - `--depth 3` — limit traversal depth.
   - `--max-nodes 50` — cap the result size.
   - `--stop-at "clk|reset"` — stop tracing at clock/reset boundaries.
   - `--include "u_arbiter"` — only show nodes matching this regex.
   - `--exclude "u_debug"` — hide debug logic from results.
3. To see fanout instead: change `--mode drivers` to `--mode loads`.

### Sequence C — Trace across port boundaries

Use `--prefer-port-hop` to follow signals through module port connections rather than stopping at them:

```python
rtl_trace(args=["trace", "--db", "rtl_trace.db", "--mode", "drivers",
                "--signal", "top.u0.data_in", "--prefer-port-hop", "--depth", "5"])
```

### Sequence E — Jump to the RTL source of a suspicious instance

When a trace or hierarchy browse identifies an instance you want to examine at the source level:

```python
rtl_trace(args=["whereis-instance", "--db", "rtl_trace.db",
                "--instance", "top.u_dma.u_fifo", "--format", "json"])
```

The result includes:
- `instance` — the full hierarchical path
- `module` — the module type name
- `file` — absolute path to the `.sv`/`.v` file where the module is defined
- `line` — the line number of the module declaration

Read that file to understand the RTL logic directly, without searching the directory tree.

### Sequence F — Collect source files for a targeted local test

When you need to build a filelist for a local simulation (see `EDA_USE.md`), use `hier --show-source` to extract all source files for a subtree in one call, rather than hunting manually:

```python
# 1. Dump the subtree with source annotations
rtl_trace(args=["hier", "--db", "rtl_trace.db",
                "--root", "top.u_dma", "--depth", "10",
                "--show-source", "--format", "json"])
```

The JSON result contains a `source_file` field for each node. Collect the unique values — those are the RTL files you need. Then write them to `files.f`:

```
# files.f (assembled from hier --show-source output)
/path/to/rtl/dma_top.sv
/path/to/rtl/dma_engine.sv
/path/to/rtl/fifo.sv
/path/to/rtl/arbiter.sv
tb_top.sv
```

**Important:** `hier --show-source` returns the definition file for each *module*, not the instance. If multiple instances share the same module (e.g., three `fifo` instances), the file appears once per instance in the output but you only need to include it once in the filelist. Deduplicate before writing `files.f`.

### Sequence D — Multi-query session (serve mode)

When you need to run many queries against the same database, use serve mode to avoid repeated startup costs:

1. `rtl_trace_serve_start(serve_args=["--db", "rtl_trace.db"])` — returns a `session_id`.
2. `rtl_trace_serve_query(session_id="<sid>", command_line="find --query timeout --limit 5")`
3. `rtl_trace_serve_query(session_id="<sid>", command_line="trace --mode drivers --signal top.u0.timeout --format json")`
4. `rtl_trace_serve_query(session_id="<sid>", command_line="hier --root top.u0 --depth 2 --show-source")`
5. `rtl_trace_serve_query(session_id="<sid>", command_line="whereis-instance --instance top.u0.u_fifo --format json")`
6. `rtl_trace_serve_stop(session_id="<sid>")` — always clean up.

**Rules for serve mode:**
- Always stop the session when you are done.
- Do not pass `--db` in individual `serve_query` commands; the database is set at `serve_start`.
- Do not call `rtl_trace(args=["serve", ...])` directly. Always use the serve start/query/stop tools.

---

## Choosing one-shot vs. serve mode

| Scenario | Use |
|---|---|
| 1–2 quick queries | `rtl_trace(...)` one-shot calls |
| 3+ queries on the same DB | `rtl_trace_serve_start/query/stop` |
| Exploratory browsing (unknown number of queries) | Serve mode |

### Sequence G — Switch from ad hoc browsing to crawler mapping

Use this when hierarchy browsing stops being targeted and starts turning into documentation work:

1. You have already used several `hier` / `find` / `trace` calls and still cannot explain the subsystem boundary.
2. The same subsystem keeps reappearing in your traces, but you do not know its major child blocks or interfaces.
3. Stop this playbook and route to `08_DESIGN_MAPPING.md`.
4. Generate or read the subsystem architecture doc, then come back here only for targeted follow-up questions.

---

## Tips

- Use `--format json` when you plan to process results programmatically; use `--format text` (default) for human-readable output.
- Signal paths support bit-selects (`top.sig[3]`) and part-selects (`top.sig[31:16]`).
- `--cone-level N` in trace mode controls how many levels of combinational logic to expand. Use it to control granularity.
- `find` with `--regex` enables regular expression matching for complex signal name patterns.
- If `compile` fails, check the error log (`--compile-log compile.log`) before retrying with different flags.
- **`whereis-instance` vs `hier --show-source`:** use `whereis-instance` for a single known instance (fast point lookup); use `hier --show-source` when you need source files for an entire subtree (filelist generation or design-wide review).
- `--show-source` output may omit the `source_file` field for instances whose module definition was not resolved during compilation (e.g., black-boxed IP). Handle missing fields gracefully when parsing the JSON.
