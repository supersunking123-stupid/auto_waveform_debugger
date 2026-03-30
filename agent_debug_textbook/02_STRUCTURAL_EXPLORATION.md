# Playbook 02 — Structural Exploration

**Role:** You are a design navigator. Your job is to understand how the RTL is structurally connected — what drives what, how instances are organized — without looking at waveform data.

**When to use:** You need to answer questions about RTL connectivity, signal fanin/fanout, or design hierarchy. No simulation values are involved.

---

## Tools available in this playbook

| Tool | Purpose |
|---|---|
| `rtl_trace` (compile) | Build the structural database from RTL source files |
| `rtl_trace` (find) | Search for signals by name or pattern |
| `rtl_trace` (hier) | Browse the instance hierarchy |
| `rtl_trace` (trace) | Trace structural drivers or loads of a signal |
| `rtl_trace_serve_start` | Start a persistent serve session for multiple queries |
| `rtl_trace_serve_query` | Send a query to a running serve session |
| `rtl_trace_serve_stop` | Stop a serve session |

---

## Prerequisite: the structural database

All structural exploration requires a compiled database. If one does not exist yet, compile first:

```python
rtl_trace(args=["compile", "--db", "rtl_trace.db", "--top", "top_module", "-f", "files.f"])
```

Common compile flags for tricky designs:
- `--incremental` — reuse previous compilation, only recompile changed files.
- `--relax-defparam` — tolerate non-standard defparam usage.
- `--mfcu` — multi-file compilation unit mode.
- `--low-mem` — reduce memory usage for very large designs.

**Do not proceed to other commands until compile succeeds.**

---

## Decision tree

```
What do you need to know?
│
├─ "What instances exist under module X?"
│   └─ rtl_trace(args=["hier", "--db", "rtl_trace.db", "--root", "top.x", "--depth", "2"])
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

### Sequence D — Multi-query session (serve mode)

When you need to run many queries against the same database, use serve mode to avoid repeated startup costs:

1. `rtl_trace_serve_start(serve_args=["--db", "rtl_trace.db"])` — returns a `session_id`.
2. `rtl_trace_serve_query(session_id="<sid>", command_line="find --query timeout --limit 5")`
3. `rtl_trace_serve_query(session_id="<sid>", command_line="trace --mode drivers --signal top.u0.timeout --format json")`
4. `rtl_trace_serve_query(session_id="<sid>", command_line="hier --root top.u0 --depth 2")`
5. `rtl_trace_serve_stop(session_id="<sid>")` — always clean up.

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

---

## Tips

- Use `--format json` when you plan to process results programmatically; use `--format text` (default) for human-readable output.
- Signal paths support bit-selects (`top.sig[3]`) and part-selects (`top.sig[31:16]`).
- `--cone-level N` in trace mode controls how many levels of combinational logic to expand. Use it to control granularity.
- `find` with `--regex` enables regular expression matching for complex signal name patterns.
- If `compile` fails, check the error log (`--compile-log compile.log`) before retrying with different flags.
