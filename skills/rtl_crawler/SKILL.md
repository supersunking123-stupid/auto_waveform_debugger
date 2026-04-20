---
name: rtl-crawler
description: "Single-agent RTL Crawler. Use this skill whenever the user asks to 'crawl', 'explore', 'document', or 'map' an RTL design, or when they mention 'RTL Crawler'. Also trigger when the user wants to generate architecture docs from a compiled rtl_trace database, or asks you to understand the structure of a chip/SoC/ASIC/FPGA design before debugging. This skill explores the structural hierarchy with rtl_trace and generates wrapper-aware per-subsystem markdown architecture docs plus a design index. If the user explicitly asks for subagents, delegation, or parallel agent work, use the sibling `rtl-crawler-multi-agent` skill instead."
---

# RTL Crawler

You are running the single-agent RTL crawler flow. You own the full
crawl: compile if needed, discover the real subsystem tree, deep-crawl
each subsystem sequentially, and write the final wrapper-aware docs.

This skill is **single-agent only**. If the user explicitly authorizes
subagents, delegation, or parallel agent work for the crawl, stop and
use the sibling `rtl-crawler-multi-agent` skill instead.

Keep the docs concise and factual. Prefer structure over speculation.
When a block's purpose is uncertain, say so.

---

## Before you start

Gather these from the user if they were not provided:

| Parameter | Example | Notes |
|-----------|---------|-------|
| `db_path` | `rtl_trace.db` | Path to compiled structural DB |
| `top_module` | `top` | Top-level instance name |
| `max_depth` | `4` | Deep-crawl depth per subsystem (default 4) |
| `output_dir` | `./rtl_docs` | Where to write markdown files |
| `filelist` | `files.f` | Needed only if the DB must be compiled |
| `compile_args` | `--mfcu` | Extra compile flags (optional) |

If the user gives you a compiled DB, skip Phase 1.

---

## Phase 1 — Compile (skip if DB exists)

```python
rtl_trace(args=["compile", "--db", db_path, "--top", top_module, "-f", filelist, ...compile_args])
```

If this fails, report the error and stop.

---

## Phase 2 — Recursive subsystem discovery

Your first job is to discover the **real functional subsystem tree**,
not just the top module's immediate children. Designs often hide real
subsystems under wrapper layers.

### 2a. Start a serve session

Use serve mode for the whole crawl.

```python
rtl_trace_serve_start(serve_args=["--db", db_path])
# Save session_id and reuse it for every query.
```

### 2b. Discovery loop

Start with a discovery queue containing only `top_module`. Keep a
visited set keyed by full instance path.

For each queued node:

1. Expand one level:
   ```python
   rtl_trace_serve_query(session_id, "hier --root {node} --depth 1 --format json --show-source")
   ```
2. For each child, identify it:
   ```python
   rtl_trace_serve_query(session_id, "whereis-instance --instance {child} --format json")
   ```
3. Classify the child using the rules below.
4. Record a manifest entry with:
   - `category`: `subsystem`, `wrapper`, `instance_array`, or `leaf_ip`
   - `instance_path`
   - `module_type`
   - `source_file`
   - `parent`
   - `count` for arrays when applicable
5. If the child is a wrapper, push it back onto the discovery queue.

The loop ends when the queue is empty.

### Classification rules

Apply these in order:

**1. Hard leaf IP**

Classify as `leaf_ip` if either condition holds:
- module type matches a hard-leaf pattern from the shared list below
- `hier --depth 1` shows zero children

Do not go deeper.

**2. Instance-array candidate**

Collapse siblings to one `instance_array` representative only if all of
these match:
- same indexed naming pattern
- same module type
- same source file
- same immediate-child module fingerprint from `hier --depth 1`

If any differ, keep separate manifest entries.

**3. Wrapper**

Classify as `wrapper` if the instance looks like a hierarchical
container rather than one functional block. Typical wrapper signs:
- it contains multiple children that are themselves substantial blocks
- it has few direct local signals beyond clocks, resets, and pass-through plumbing
- the children look functionally independent from one another

To confirm a suspected wrapper, use regex-scoped direct-signal probes.
`rtl_trace find` is literal substring match unless `--regex` is passed,
so always pass `--regex` when using anchors or wildcards. Also escape
the literal instance path before embedding it in a regex.

Because DBs may expose direct struct-member signals such as
`req.aw.valid`, probe both flat direct signals and direct struct-member
fields. First collect the immediate child instance names from the
`hier --depth 1` result. Ignore any `find` match whose first token after
`child.` is one of those child names; that match belongs to a descendant
child block, not to the current instance's own local logic.

```python
flat_prefix = "^" + escape_regex(child) + r"\.[^.]*"
struct_prefix = "^" + escape_regex(child) + r"\.[^.]+(\.[^.]+){0,2}"
rtl_trace_serve_query(
    session_id,
    f"find --query '{flat_prefix}(valid|ready|req|gnt)$' --regex --limit 5 --format json",
)
rtl_trace_serve_query(
    session_id,
    f"find --query '{struct_prefix}\\.(valid|ready|req|gnt)$' --regex --limit 5 --format json",
)
rtl_trace_serve_query(
    session_id,
    f"find --query '{flat_prefix}(fsm|state|mode|sel)$' --regex --limit 5 --format json",
)
rtl_trace_serve_query(
    session_id,
    f"find --query '{struct_prefix}\\.(fsm|state|mode|sel)$' --regex --limit 5 --format json",
)
```

If the instance has multiple substantial children but few or no direct
signals from those probes, it is likely a wrapper.

If you are unsure whether a block is a wrapper or a subsystem, choose
`subsystem`. False wrapper classifications are more damaging than
slightly over-documenting one block.

**4. Subsystem**

Everything else is a `subsystem`. This includes doc-worthy IP blocks
such as PCIe, DDR/HBM controllers, CPUs or clusters, DMA engines, NoCs,
peripheral fabrics, and similar major functional units.

### Discovery depth limit

To prevent pathological wrapper chains from running away, limit wrapper
nesting discovery to 4 levels. If you hit that limit, treat the
remaining node as a `subsystem`.

### 2c. Build a coverage-dedup map

After the manifest is complete, build a best-effort map of repeated
internal blocks so later deep crawls can cross-reference instead of
re-exploring structurally equivalent utility blocks.

Use this fingerprint:

`fingerprint = module_type + source_file + sorted(immediate_child_module_types)`

You do not need a full pre-crawl. A quick `hier --depth 2` on each
subsystem root is enough to identify the most common duplicates.

When a deep crawl later sees a matching internal block with the same
quick fingerprint, note it in the doc and cross-reference the earlier
subsystem doc instead of fully re-crawling it.

---

## Phase 3 — Sequential deep crawl and doc writing

Process manifest entries sequentially. Keep one in-memory summary per
doc: clock domains, resets, external interfaces, top-level children,
and debugging notes.

Wrappers do **not** get the same deep internal exploration as
subsystems, but they still get their own wrapper doc using the shared
template.

### 3a. Deep-crawl one subsystem

For each `subsystem` or `instance_array` representative:

1. Expand the hierarchy level by level:
   ```python
   rtl_trace_serve_query(session_id, "hier --root {subsystem} --depth 2 --format json --show-source")
   ```
2. Classify children during the crawl:
   - `leaf_ip`: hard leaf pattern or zero children
   - `instance_array`: same type, source, and immediate-child fingerprint
   - `interior node`: queue for later expansion
3. Stop when any of these holds:
   - current depth from subsystem root >= `max_depth`
   - all remaining nodes are leaf IPs or already visited
   - further depth adds noise without improving the architecture doc

### 3b. Detect interfaces and control landmarks

At each visited node, search for clocks, resets, handshake signals, and
control landmarks:

```python
node_re = "^" + escape_regex(node) + r"\..*"
rtl_trace_serve_query(session_id, "find --query '{node_re}(clk|clock)' --regex --limit 20 --format json")
rtl_trace_serve_query(session_id, "find --query '{node_re}(rst|reset)' --regex --limit 20 --format json")
rtl_trace_serve_query(session_id, "find --query '{node_re}valid' --regex --limit 30 --format json")
rtl_trace_serve_query(session_id, "find --query '{node_re}ready' --regex --limit 30 --format json")
rtl_trace_serve_query(session_id, "find --query '{node_re}(fsm|state|mode|sel)' --regex --limit 10 --format json")
rtl_trace_serve_query(session_id, "find --query '{node_re}(enable|en|grant|arb)' --regex --limit 10 --format json")
```

Bus protocol fingerprints:
- `aw/ar/w/r/b` with valid/ready -> AXI
- `haddr`, `htrans`, `hready` -> AHB
- `psel`, `penable`, `paddr` -> APB
- `tvalid`, `tready`, `tdata` -> AXI-Stream

**Struct member signals:** DBs compiled with struct-member decomposition
may expose `req.aw.valid` instead of `req_aw_valid`. Treat these as the
same logical interface and group them by the parent struct to avoid
double-counting.

### 3c. Trace key connections

For each detected interface, trace one representative handshake signal.
Resolve the exact signal path first with `find`, then trace that exact
signal:

```python
rtl_trace_serve_query(
    session_id,
    "find --query '^{esc_node}\\..*aw.*valid' --regex --limit 5 --format json",
)
rtl_trace_serve_query(
    session_id,
    "trace --mode drivers --signal {resolved_signal} --depth 2 --format json",
)
rtl_trace_serve_query(
    session_id,
    "trace --mode loads --signal {resolved_signal} --depth 2 --format json",
)
```

Record the resolved signal as that interface's `probe_signal`. You will
reuse it in Phase 4 when cross-subsystem interface matching is
ambiguous.

Be selective:
- trace one `valid` or `ready` per bus, not every data bit
- trace clocks and resets to identify domains
- trace `enable`, `sel`, `mux`, `arb`, or similar control points when present
- use `--depth 2` for normal traces

When tracing around one subsystem, prevent unbounded fanout into other
subsystems by building a `--stop-at` regex from escaped sibling
subsystem instance paths.

### 3d. Late wrapper correction

Discovery should catch most wrappers, but if a subsystem assigned for
deep crawl later proves to be a wrapper:

1. Rewrite that manifest entry from `subsystem` or `instance_array` to
   `wrapper`.
2. Insert its child sub-manifest entries under that wrapper.
3. Write the wrapper doc using the wrapper template.
4. Continue the sequential crawl on the newly inserted child
   subsystems.

Do not silently keep a wrapper as if it were one monolithic subsystem.

### 3e. Write the doc immediately

After finishing a manifest entry, write its doc immediately:
- subsystem docs use the subsystem template
- wrapper docs use the wrapper template

Read `references/templates.md` and follow it literally for filenames,
overview fields, hierarchy tree style, wrapper layout, and link format.

---

## Phase 4 — Cross-subsystem interface mapping

After all subsystem docs are written, build the system connectivity map.

Match external interfaces from the collected per-doc summaries. When the
peer is ambiguous or the connection crosses wrappers or leaf IP, trace
explicitly from the recorded `probe_signal`:

```python
rtl_trace_serve_query(session_id,
    "trace --mode loads --signal {probe_signal} --depth 4 --format json")
```

Build the final connectivity table from those results.

---

## Phase 5 — Write the index document

Write `design_index.md` in `output_dir`. It must include:
- design metadata
- top-level hierarchy table
- wrapper-aware condensed hierarchy tree
- system interconnect table
- clock domains summary
- reset tree summary

Use the shared template in `references/templates.md`.

Then stop the serve session:

```python
rtl_trace_serve_stop(session_id)
```

---

## Hard leaf patterns

Use this narrow hard-leaf list. These are implementation blocks, not
doc-worthy subsystems:

```text
(sram|ram|rom|rf_|regfile)
(fifo|async_fifo|sync_fifo)
(phy|pll|dll|serdes|io_pad)
(stub|blackbox)
(std_cell|lib_)
(clock_gate|icg|reset_sync|cdc_sync)
```

Do **not** hard-stop broader subsystem keywords such as `cpu`, `pcie`,
`ddr`, `dma`, `uart`, or `gpio`. Those are usually still subsystems.

## Doc-worthy subsystem IP hints

If a block matches one of these hints and is not a confirmed wrapper or
hard leaf, prefer `subsystem`:

```text
(cpu|core|processor|riscv|arm)
(mem_ctrl|ddr|hbm|lpddr)
(uart|spi|i2c|gpio|timer|wdt)
(axi_dma|dma_engine)
(pcie|usb|ethernet|mac|gmii)
```

The user can extend either list.

---

## Error handling

- If compile fails: report the error and stop.
- If discovery exceeds 4 wrapper levels: treat the remaining node as a subsystem.
- If the serve session dies: restart it and continue; already written docs are safe.
- If a block's purpose remains unclear: document that uncertainty explicitly.
- If cross-subsystem matching stays ambiguous after `probe_signal` tracing: record the best bounded hypothesis and say the connection needs further structural or RTL review.

---

## Output templates

Read `references/templates.md` for:
- `design_index.md`
- subsystem architecture docs
- wrapper docs

Follow those templates exactly.
