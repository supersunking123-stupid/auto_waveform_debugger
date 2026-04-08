---
name: rtl-crawler
description: "Systematically explore an RTL design's structural hierarchy using rtl_trace tools, then generate per-subsystem markdown documentation describing the architecture. Use this skill whenever the user asks to 'crawl', 'explore', 'document', or 'map' an RTL design, or when they mention 'RTL Crawler'. Also trigger when the user wants to generate architecture docs from a compiled rtl_trace database, or asks you to understand the structure of a chip/SoC/ASIC/FPGA design before debugging. This skill produces markdown files that serve as a shared knowledge base so that other agents (or humans) can quickly orient themselves in the design without repeating the exploration."
---

# RTL Crawler

You are about to systematically explore an RTL design the way a human
engineer would in Verdi: start at the top, drill into interesting blocks,
note what each block does, sketch the interfaces, and stop when you hit
well-known IP or leaf cells.

Your output is a set of markdown files — one per major subsystem plus an
index — that will be consumed by other agents or engineers. Keep them
concise and factual. When you are unsure about a block's purpose, say so
rather than guessing.

---

## Before you start

Gather these from the user (ask if not provided):

| Parameter | Example | Notes |
|-----------|---------|-------|
| `db_path` | `rtl_trace.db` | Path to compiled structural DB |
| `top_module` | `top` | Top-level instance name |
| `max_depth` | `4` | How deep to crawl (default 4) |
| `output_dir` | `./rtl_docs` | Where to write markdown files |
| `filelist` | `files.f` | Only needed if DB must be compiled |
| `compile_args` | `--mfcu` | Extra compile flags (optional) |

If the user gives you a compiled DB, skip Phase 1.  If they say "just
explore as deep as you think is needed", use depth 4 and apply the
stopping heuristics below.

---

## Phase 1 — Compile (skip if DB exists)

```python
rtl_trace(args=["compile", "--db", db_path, "--top", top_module, "-f", filelist, ...compile_args])
```

If this fails, report the error and stop. You cannot crawl without a DB.

---

## Phase 2 — Top-level discovery

### 2a. Start a serve session

Use serve mode for the entire crawl. It avoids reloading the DB on every
call and is significantly faster on large designs.

```python
rtl_trace_serve_start(serve_args=["--db", db_path])
# Save the returned session_id — you will use it for every subsequent query.
```

### 2b. List top-level children

```python
rtl_trace_serve_query(session_id, "hier --root {top_module} --depth 1 --format json --show-source")
```

### 2c. Identify each child

For each child instance:

```python
rtl_trace_serve_query(session_id, "whereis-instance --instance {child_path} --format json")
```

Record the instance path, module type, and source file location.

### 2d. Classify each child

Apply these rules in order:

1. **Leaf IP** — module name matches any stop pattern (see §Stopping
   heuristics).  Record it but do NOT crawl deeper.
2. **Instance array** — multiple siblings share the same module type
   (e.g. `u_core[0]` through `u_core[3]`).  Crawl ONE representative,
   note the array count.
3. **Subsystem** — everything else.  Gets its own markdown file and will
   be crawled in Phase 3.

Write down a subsystem manifest before proceeding. Example:

```
subsystem:      top.u_noc          module: noc_top         source: rtl/noc/noc_top.sv:1
subsystem:      top.u_gpu          module: gpu_wrapper     source: rtl/gpu/gpu_wrapper.sv:5
leaf_ip:        top.u_ddr_ctrl     module: ddr4_mc         source: rtl/mem/ddr4_mc.sv:1
instance_array: top.u_core[0..3]  module: riscv_core_wrap source: rtl/cpu/riscv_core_wrap.sv:1  count: 4
```

---

## Phase 3 — Subsystem deep crawl

Run this phase once per subsystem, sequentially.  **Write each subsystem's
doc immediately after finishing it** — do not wait until the end.

### 3a. Expand hierarchy level by level

Starting from the subsystem root:

```python
rtl_trace_serve_query(session_id, "hier --root {node_path} --depth 2 --format json --show-source")
```

Use `--depth 2` to batch two levels per call (saves round trips).
For each child returned, classify it the same way as Phase 2d.
Queue unvisited, non-leaf children for the next expansion.

Repeat until one of these conditions is met:
- Current depth from subsystem root >= `max_depth`
- All children are leaf IPs or already visited
- Agent judgment says to stop (see §Stopping heuristics)

### 3b. Detect interfaces at each node

Search for bus handshake and clock/reset signals:

```python
rtl_trace_serve_query(session_id, "find --query {node_path}.*clk --limit 20 --format json")
rtl_trace_serve_query(session_id, "find --query {node_path}.*rst --limit 20 --format json")
rtl_trace_serve_query(session_id, "find --query {node_path}.*valid --limit 30 --format json")
rtl_trace_serve_query(session_id, "find --query {node_path}.*ready --limit 30 --format json")
```

Use the signal names to fingerprint the bus protocol:
- `aw/ar/w/r/b` channels with `valid`/`ready` → AXI
- `haddr`, `htrans`, `hready` → AHB
- `psel`, `penable`, `paddr` → APB
- `tvalid`, `tready`, `tdata` → AXI-Stream

Also search for control signals that reveal the block's function:

```python
rtl_trace_serve_query(session_id, "find --query {node_path}.*fsm* --limit 10 --format json")
rtl_trace_serve_query(session_id, "find --query {node_path}.*state* --limit 10 --format json")
```

### 3c. Trace key connections

For each detected interface, trace ONE handshake signal to find the peer:

```python
rtl_trace_serve_query(session_id,
    "trace --mode drivers --signal {node}.awvalid --depth 2 --format json")
rtl_trace_serve_query(session_id,
    "trace --mode loads --signal {node}.awvalid --depth 2 --format json")
```

Tracing strategy — be selective:
- Trace `*valid` or `*ready` for each bus, not every data bit.
- Trace clock and reset to identify domains.
- Trace `*enable*`, `*sel*`, `*mux*` if present.
- Use `--depth 2` to look one hop beyond the immediate connection.
- Use `--stop-at` to prevent the trace from running into other subsystems:

```python
rtl_trace_serve_query(session_id,
    "trace --mode drivers --signal {sig} --depth 3 --stop-at {other_subsys_regex} --format json")
```

### 3d. Record your findings

For each node you visit, keep a structured note:

```
Node: top.u_noc.u_xbar
  Module type:   crossbar_4x4
  Source:        rtl/noc/crossbar_4x4.sv:15
  Clock domain:  noc_clk
  Reset:         noc_rst_n (active low)
  Interfaces:
    - AXI slave port 0 ← top.u_noc.u_decode
    - AXI master port 0 → top.u_noc.u_sram_if
    - AXI master port 1 → top.u_noc.u_periph_if
  Children:
    - u_arb[0..3]: round_robin_arb (instance array, count=4)
    - u_route_lut: route_lookup (leaf)
```

### 3e. Write the subsystem doc

After finishing a subsystem, write its markdown doc immediately.
See `references/templates.md` for the exact template.

---

## Phase 4 — Interface mapping

After all subsystems are crawled, build the cross-subsystem connectivity.
For each subsystem-level bus interface discovered in Phase 3, trace across
boundaries:

```python
rtl_trace_serve_query(session_id,
    "trace --mode loads --signal {subsystem}.{port_signal} --depth 4 --format json")
```

Build a connectivity list:

```
u_cpu  → u_noc   via AXI master (instruction fetch, data access)
u_noc  → u_sram  via AXI slave  (SRAM port)
u_noc  → u_ddr   via AXI slave  (DDR controller)
u_gpu  → u_noc   via AXI master (texture fetch)
```

---

## Phase 5 — Write the index document

Write `design_index.md` in the output directory.  It contains:
- Design metadata (top module, DB path, crawl date, depth)
- Top-level hierarchy table with links to subsystem docs
- **Top-level hierarchy tree** — a condensed indented tree that stops at
  subsystem boundaries. Do NOT expand subsystem internals here; each
  subsystem line should end with a `[doc](./..._architecture.md)` link.
  The detailed internal trees belong in the per-subsystem docs only.
- System interconnect (the connectivity list from Phase 4)
- Clock domains summary
- Reset tree summary

See `references/templates.md` for the exact template.

Then stop the serve session:

```python
rtl_trace_serve_stop(session_id)
```

---

## Stopping heuristics

Check these at every hierarchy node before expanding further.

### Hard stops (always stop here)

| Condition | How to detect |
|-----------|---------------|
| Depth limit reached | `current_depth >= max_depth` |
| Module name matches stop pattern | Regex on module type (see below) |
| Leaf node (no children) | `hier --depth 1` returns zero children |
| Module type already explored | You already fully crawled this module type in another instance — just cross-reference |

### Stop patterns (regex, case-insensitive)

These module names indicate known IP or leaf cells not worth crawling:

```
(sram|ram|rom|rf_|regfile)
(fifo|async_fifo|sync_fifo)
(cpu|core|processor|riscv|arm)
(phy|pll|dll|serdes|io_pad)
(mem_ctrl|ddr|hbm|lpddr)
(wrapper|stub|blackbox)
(std_cell|lib_)
(uart|spi|i2c|gpio|timer|wdt)
(axi_dma|dma_engine)
(pcie|usb|ethernet|mac|gmii)
```

The user can extend this list. When in doubt, ask.

### Soft stops (use your judgment)

Stop and summarize (don't crawl deeper) when you see:
- **Standard IP names** — module name strongly suggests a well-known IP.
  Record type and move on.
- **Repetitive structure** — all children are identical instances.
  Document one, note the count.
- **Vendor/external source** — `--show-source` returns a path outside the
  project tree (e.g. `/ip_lib/`, `/vendor/`, `/synopsys/`).
- **Diminishing returns** — you already know enough to describe the block's
  role and interfaces.  Going deeper adds noise.

---

## Practical tips

**Budget your tool calls.** A large SoC can have hundreds of instances.

- Use `--depth 2` or `--depth 3` on `hier` to batch multiple levels.
- Use `--limit` on `find` to cap output.
- Use `--stop-at` on `trace` to prevent unbounded fanout.
- Trace one representative signal per bus, not every bit.

**Handle unknowns honestly.** If you cannot determine a block's purpose:

> **u_mystery_block** (`weird_module_v3`): Purpose unclear from structural
> analysis alone. Contains a 4-deep pipeline and connects to the NOC via
> a custom interface. Requires RTL source review or spec consultation.

This is far more useful than a guess.

**Maintain a visited set.** Keep track of which module types you have
already fully explored. When you encounter the same module type again in
a different instance, skip the deep crawl and write "Same architecture
as {other_instance}, see [{other_doc}]."

**Write docs incrementally.** After each subsystem crawl, write that
subsystem's markdown file immediately. If the crawl is interrupted
partway through, the completed subsystem docs are still usable.

---

## Output templates

Read `references/templates.md` (in this skill directory) for the exact
markdown templates for both the index document and per-subsystem documents.
Read it before you start writing any docs.
