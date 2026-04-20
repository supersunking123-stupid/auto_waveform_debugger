---
name: rtl-crawler-multi-agent
description: "Multi-agent version of RTL Crawler. Use only when the user explicitly asks for subagents, delegation, or parallel agent work while crawling/mapping an RTL design, or otherwise clearly authorizes multi-agent execution. If the user wants RTL crawling but has not authorized delegation, use the sibling `rtl-crawler` skill instead. This skill explores the structural hierarchy with rtl_trace and generates per-subsystem markdown architecture docs plus a design index."
---

# RTL Crawler — Orchestrator

You are the orchestrator of a multi-agent RTL design exploration flow.
You handle the big picture — compiling the DB, discovering the full
subsystem tree (including nested wrappers), spawning worker agents, and
assembling the final index document.

Worker agents handle the detail work — deep-crawling one subsystem each
and writing its architecture doc.

This skill is **multi-agent only**. If the user has not explicitly
authorized subagents / delegation / parallel agent work, stop and use
the sibling `rtl-crawler` skill instead.

---

## Before you start

Gather these from the user (ask if not provided):

| Parameter | Example | Notes |
|-----------|---------|-------|
| `db_path` | `rtl_trace.db` | Path to compiled structural DB |
| `top_module` | `top` | Top-level instance name |
| `max_depth` | `4` | How deep workers crawl (default 4) |
| `output_dir` | `./rtl_docs` | Where to write markdown files |
| `filelist` | `files.f` | Only needed if DB must be compiled |
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

This is the most important phase.  Your goal is to find all the **real
functional subsystems** in the design, regardless of how many wrapper
levels sit between them and the top.

### 2a. Start the serve session

```python
rtl_trace_serve_start(serve_args=["--db", db_path])
# Save session_id — pass it to every worker.
```

### 2b. Recursive discovery loop

Start with a discovery queue containing just the top module.  Then
repeat:

1. Pop the next node from the queue.
2. Get its children:
   ```python
   rtl_trace_serve_query(session_id, "hier --root {node} --depth 1 --format json --show-source")
   ```
3. For each child, identify it:
   ```python
   rtl_trace_serve_query(session_id, "whereis-instance --instance {child} --format json")
   ```
4. Classify each child (see §Classification rules below).
5. If a child is classified as a **wrapper**, add it to the discovery
   queue instead of the manifest.  The loop will process it next.
6. Everything else goes into the subsystem manifest.

The loop ends when the discovery queue is empty.

### Classification rules

Apply these in order for each child instance.  Keep a visited set keyed
by full instance path so the same node is never inserted twice.

**1. Hard leaf IP** — module type matches a **hard leaf** pattern (see
§Hard leaf patterns), or `hier --depth 1` shows that it has no
children.  Record it in the manifest as `leaf_ip`.  Do NOT explore
further.

Hard-leaf patterns are intentionally narrow: memories, FIFOs, standard
cells, stubs, PHYs, and similar implementation blocks.  Do **not**
classify a block as `leaf_ip` merely because its name contains a
subsystem keyword such as `pcie`, `ddr`, `cpu`, or `uart`.

**2. Instance-array candidate** — siblings share indexed naming and the
same module type.  Collapse them to a single `instance_array`
representative **only if** they also look structurally equivalent:

- same module type
- same source file (from `whereis-instance` / `hier --show-source`)
- same immediate-child module fingerprint from `hier --depth 1`

If any of those checks differ, keep them as separate manifest entries.

**3. Wrapper** — the instance looks like an intermediate hierarchical
container rather than a real functional block.  **Add it back to the
discovery queue** instead of assigning a worker.

A wrapper typically has these characteristics:
- It contains multiple children that are themselves substantial blocks
  (each having their own children).
- It has very few direct signals of its own beyond clock, reset, and
  passthrough/control plumbing.
- Its child blocks look functionally independent from one another.

To confirm a suspected wrapper, use regex-scoped signal searches.
`rtl_trace find` treats `--query` as a literal substring unless
`--regex` is passed, so always pass `--regex` when using wildcards or
anchors.  Also escape the literal instance path before embedding it in a
regex.

Because modern DBs may expose direct **struct-member signals** such as
`child.req.aw.valid`, probe both flat direct signals and direct
struct-member fields. First collect the immediate child instance names
from the `hier --depth 1` result. When reviewing the `find` matches
below, ignore any result whose first token after `child.` is one of
those child instance names; that match belongs to a descendant child
block, not to the current instance's own local logic.

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

If the instance has multiple substantial children but few or no **direct
signals** matching those probes, it is likely a wrapper.

When you identify a wrapper:
- Add it to the discovery queue so its children get classified.
- Record it in the manifest as `wrapper` — it will get a full
  architecture-style wrapper doc, with the same core sections expected
  for subsystem docs (clock domains, resets, external interfaces,
  internal hierarchy, and debugging notes), and workers' docs will be
  organized under it.

If you are unsure whether a block is a wrapper or a subsystem, choose
`subsystem`.  False wrapper classifications are more damaging than
slightly over-documenting one block.

**4. Subsystem** — everything else.  This includes doc-worthy IP blocks
such as PCIe, DDR/HBM controllers, CPUs/clusters, DMA engines, NoCs,
peripheral fabrics, and similar major functional units.  Add it to the
manifest as `subsystem`.

### Example: multi-die SoC

Given this structure:
```
chip_top
├── compute_die[0..3]     (each is a substantial block with internal hierarchy)
├── io_die                (a wrapper — just instantiates sub-blocks)
│   ├── d2d_subsys[0..3]  (real subsystem — D2D controller + SerDes + PMA)
│   ├── pcie_subsys       (real subsystem — PCIe controller complex)
│   └── mem_if_subsys     (real subsystem — DDR/HBM interface)
└── global_clk_rst        (leaf IP — clock/reset generation)
```

The discovery loop would proceed:

**Iteration 1:** Process `chip_top`
- `compute_die[0..3]` → instance array, crawl one representative → manifest
- `io_die` → has 3 substantial children, no local FSMs → **wrapper** → back to queue
- `global_clk_rst` → matches a hard leaf pattern → `leaf_ip` → manifest

**Iteration 2:** Process `io_die`
- `d2d_subsys[0..3]` → instance array → manifest (representative)
- `pcie_subsys` → subsystem → manifest
- `mem_if_subsys` → subsystem → manifest

Final manifest:
```
instance_array: chip_top.compute_die[0..3]         module: compute_die    count: 4   parent: chip_top
wrapper:        chip_top.io_die                     module: io_die_top                parent: chip_top
instance_array: chip_top.io_die.d2d_subsys[0..3]   module: d2d_subsystem  count: 4   parent: chip_top.io_die
subsystem:      chip_top.io_die.pcie_subsys         module: pcie_complex              parent: chip_top.io_die
subsystem:      chip_top.io_die.mem_if_subsys       module: mem_interface             parent: chip_top.io_die
leaf_ip:        chip_top.global_clk_rst             module: clk_rst_gen               parent: chip_top
```

Note the `parent` field — it records the hierarchy path so you can
reconstruct the tree in the index doc.

### Discovery depth limit

To prevent the discovery loop from running away on pathological designs,
limit it to 4 levels of wrapper nesting.  If you hit this limit, treat
the remaining node as a subsystem and let its worker deal with it.

### 2c. Build the coverage dedup map

After the manifest is complete, scan it for blocks that appear in more
than one subsystem's subtree.  Common examples:

- `axi_protocol_bridge` used inside both `noc_top` and `pcie_complex`
- `async_fifo` used everywhere
- `clock_gate_cell` in every subsystem

Build a map keyed by a **coverage fingerprint**, not by module type
alone:

`fingerprint = module_type + source_file + sorted(immediate_child_module_types)`

Pass this to every worker as `{covered_blocks}` so they know which
internal blocks are already being documented by another worker.  A
worker may skip a deep crawl only when the module type **and** the quick
fingerprint match.  If the source file or child fingerprint differs,
document it separately even if the module type string is the same.

To build the map efficiently, you do NOT need to pre-crawl every
subsystem.  Instead, use the instance-array and leaf-IP information you
already have from the manifest, plus a quick `hier --depth 2` on each
subsystem root during Phase 2.  Record any repeated fingerprints that
appear under more than one subsystem.  Assign each shared fingerprint to
the first subsystem (alphabetically or by manifest order) that contains
it.

The map is best-effort — workers may still discover shared modules
deeper than depth 2 that the map missed.  That is fine; some
redundant exploration across workers is acceptable.  The map
eliminates the most common duplicates (bus bridges, FIFOs, standard
utility blocks) without requiring an exhaustive pre-scan.

Example `{covered_blocks}` value passed to a worker:

```
axi_protocol_bridge|rtl/common/axi_protocol_bridge.sv|axi_slice,rr_arb
  → covered by chip_top_u_noc__noc_top_architecture.md
round_robin_arb|rtl/common/round_robin_arb.sv|grant_mask,priority_enc
  → covered by chip_top_u_noc__noc_top_architecture.md
async_fifo|rtl/common/async_fifo.sv|ram_wrapper,sync_bits
  → covered by chip_top_io_die_d2d_subsys_0__d2d_subsystem_architecture.md
```

---

## Phase 3 — Spawn workers

For each `subsystem` and `instance_array` representative in the manifest,
spawn a worker subagent.  Wrappers and leaf IPs do NOT get workers.

### Building the worker prompt

Read `references/worker_prompt.md` in this skill directory.  Fill in the
placeholders for each worker:

| Placeholder | Source |
|-------------|--------|
| `{session_id}` | From Phase 2a |
| `{subsystem_instance}` | Instance path |
| `{module_type}` | Module type |
| `{source_file}` | Source location |
| `{max_depth}` | From user config |
| `{output_dir}` | From user config |
| `{output_filename}` | `{instance_slug}__{module_type_slug}_architecture.md` |
| `{parent_wrapper}` | Parent wrapper path, or `none` if directly under top |
| `{sibling_subsystems}` | ALL other subsystem instance paths (across all wrappers) |
| `{hard_leaf_patterns}` | The hard leaf patterns list |
| `{subsystem_ip_hints}` | Optional name hints for doc-worthy subsystem IP |
| `{covered_blocks}` | Coverage dedup map (see §2c) — structurally equivalent blocks already covered elsewhere |
| `{is_array_representative}` | `true` or `false` |
| `{array_count}` | Count if array, else `N/A` |

### Spawning strategy

**Default: sequential.** One worker at a time.  Safest with a shared
serve session.

**Optional: parallel.** If the user asks and the serve backend handles
concurrent queries, spawn all workers at once.

### Handling worker escalations

A worker may return an `ESCALATION` **instead of** a normal summary.
This happens when the worker discovers an unexpected layer of
sub-subsystems that the orchestrator's recursive discovery missed.

When you receive an escalation:

1. Rewrite the original manifest entry from `subsystem` /
   `instance_array` to `wrapper`, preserving its parent pointer.
2. Attach the returned wrapper doc filename to that manifest entry.
3. Preserve the wrapper summary fields from the escalation block on that
   manifest entry:
   - `clock_domains`
   - `resets`
   - `external_interfaces`
   - `top_level_children`
   - `debugging_notes`
4. Read the sub-manifest from the escalation block and insert those
   child entries under the wrapper, keyed by full instance path so you
   cannot create duplicates.
5. Treat those preserved wrapper fields exactly like a worker summary in
   later phases: include wrapper-owned `external_interfaces` in Phase 4
   connectivity mapping, and include wrapper `clock_domains` / `resets`
   in the final index summaries.
6. Recompute `sibling_subsystems` and the coverage dedup map for any
   newly spawned workers that were not already completed.
7. Spawn a new worker for each sub-subsystem listed.
8. Build the final index from the **updated manifest**, not from the
   pre-escalation plan.

This is a safety net — the recursive discovery in Phase 2 should catch
most wrappers.  But designs can surprise you, and it is better to
escalate than to silently produce a shallow doc.

---

## Worker summary format

Each worker returns one of these at the end of its output.

### Normal completion

```
=== WORKER SUMMARY ===
subsystem_instance: top.io_die.pcie_subsys
module_type: pcie_complex
source_file: rtl/pcie/pcie_complex.sv:1
output_file: top_io_die_pcie_subsys__pcie_complex_architecture.md
parent_wrapper: top.io_die

clock_domains:
  - pcie_clk_250
  - pcie_clk_500

resets:
  - pcie_rst_n (active low, async)

external_interfaces:
  - direction: master
    protocol: AXI
    peer: top.io_die.d2d_subsys[0]
    key_signals: ar/aw/w/r/b
    probe_signal: top.io_die.pcie_subsys.req.aw.valid
  - direction: slave
    protocol: APB
    peer: top.compute_die[0].u_cfg
    key_signals: psel/penable/paddr
    probe_signal: top.io_die.pcie_subsys.cfg.penable

top_level_children:
  - u_pcie_ctrl (pcie_controller) — main PCIe controller
  - u_tlp_engine (tlp_processor) — TLP pack/unpack
  - u_dma[0..3] (pcie_dma_ch) — DMA channels ×4 [leaf IP]

debugging_notes:
  - TLP engine FSM is the critical path for debug
  - DMA channels share one arbiter — check u_dma_arb for stalls
=== END SUMMARY ===
```

### Escalation (unexpected sub-subsystems found)

```
=== WORKER ESCALATION ===
subsystem_instance: top.io_die
module_type: io_die_top
reason: "Assigned block is a wrapper containing 3 major sub-subsystems."
wrapper_doc: top_io_die__io_die_top_architecture.md

clock_domains:
  - io_clk

resets:
  - io_rst_n (active low, async)

external_interfaces:
  - direction: slave
    protocol: AXI
    peer: top.noc
    key_signals: aw/ar/w/r/b
    probe_signal: top.io_die.s_axi.aw.valid

top_level_children:
  - d2d_subsys[0..3] (d2d_subsystem) — die-to-die links ×4
  - pcie_subsys (pcie_complex) — PCIe controller complex
  - mem_if_subsys (mem_interface) — memory interface complex

debugging_notes:
  - Wrapper-level debug starts at the shared ingress ports and child-boundary handshakes.
  - Check wrapper-owned reset, clock, and arbitration logic before diving into child subsystems.

sub_manifest:
  - instance_array: top.io_die.d2d_subsys[0]  module: d2d_subsystem  count: 4
  - subsystem: top.io_die.pcie_subsys        module: pcie_complex
  - subsystem: top.io_die.mem_if_subsys      module: mem_interface
=== END ESCALATION ===
```

---

## Phase 4 — Cross-subsystem interface mapping

After all workers (including any spawned from escalations) have returned,
build the connectivity map.

Match up `external_interfaces` from worker summaries **and from wrapper
metadata recorded on manifest entries** (including metadata preserved
from escalation blocks).  For ambiguous or leaf-IP connections, trace
explicitly using the worker-provided `probe_signal` field, which is the
exact resolved signal path the worker already used to identify that
interface:

```python
rtl_trace_serve_query(session_id,
    "trace --mode loads --signal {probe_signal} --depth 4 --format json")
```

Build the connectivity table.

---

## Phase 5 — Write the index document

Write `design_index.md` in `{output_dir}`.  Contents:

- Design metadata (top module, DB path, crawl date, depth)
- Top-level hierarchy table with links to subsystem and wrapper docs
- **Top-level hierarchy tree** — condensed, reflecting the wrapper
  nesting.  Stop at subsystem boundaries; each subsystem line links to
  its doc.  Wrapper levels appear as tree nodes but are NOT expanded
  into internal detail.  Example:

  ```
  chip_top
  ├── compute_die[0..3]  (compute_die ×4)  [doc](./chip_top_compute_die_0__compute_die_architecture.md)
  ├── io_die             (io_die_top)       — wrapper  [doc](./chip_top_io_die__io_die_top_architecture.md)
  │   ├── d2d_subsys[0..3] (d2d_subsystem ×4) [doc](./chip_top_io_die_d2d_subsys_0__d2d_subsystem_architecture.md)
  │   ├── pcie_subsys    (pcie_complex)     [doc](./chip_top_io_die_pcie_subsys__pcie_complex_architecture.md)
  │   └── mem_if_subsys  (mem_interface)    [doc](./chip_top_io_die_mem_if_subsys__mem_interface_architecture.md)
  └── global_clk_rst     (clk_rst_gen)      — leaf IP
  ```

- System interconnect (connectivity table from Phase 4)
- Clock domains summary (merged from all worker summaries and wrapper
  metadata recorded on manifest entries)
- Reset tree summary (merged from all worker summaries and wrapper
  metadata recorded on manifest entries)

Read `references/templates.md` for the exact template format.  Follow
that file literally; it includes the wrapper-aware hierarchy tree.

Then stop the serve session:

```python
rtl_trace_serve_stop(session_id)
```

---

## Wrapper docs

For each `wrapper` in the manifest, ensure there is a **full
architecture-style wrapper doc**, not a brief hierarchy note.

- If the wrapper came from a worker escalation, **reuse** the returned
  wrapper doc and preserved wrapper metadata.  Do not overwrite it with
  a shorter format.
- If the wrapper was discovered directly by the orchestrator, write a
  wrapper doc using the same core sections required in
  `references/worker_prompt.md` Step 5b:
  - `## Overview`
  - `## Role in the system`
  - `## Contents`
  - `## External interfaces`
  - `## Internal hierarchy`
  - `## Key internal blocks`
  - `## Notes for debugging agents`
- When the orchestrator writes a wrapper doc directly, record the same
  metadata fields on the manifest entry that an escalation would carry:
  `clock_domains`, `resets`, `external_interfaces`,
  `top_level_children`, and `debugging_notes`.
- Wrapper docs focus on wrapper-owned glue logic and summarize child
  subsystems.  They should not deep-crawl the children themselves.

---

## Hard leaf patterns (shared with workers)

Case-insensitive regex.  Pass to every worker and use these for **hard**
leaf-IP classification:

```
(sram|ram|rom|rf_|regfile)
(fifo|async_fifo|sync_fifo)
(phy|pll|dll|serdes|io_pad)
(stub|blackbox)
(std_cell|lib_)
(clock_gate|icg|reset_sync|cdc_sync)
```

These are intentionally narrow.  A block that matches broad subsystem
keywords such as `cpu`, `pcie`, `ddr`, `dma`, `uart`, or `gpio` is
still usually a `subsystem`, not a `leaf_ip`.

## Doc-worthy subsystem IP hints

Case-insensitive regex.  These are name hints for blocks that often
deserve their own architecture docs.  They are **never** hard-stop
patterns.

```
(cpu|core|processor|riscv|arm)
(mem_ctrl|ddr|hbm|lpddr)
(uart|spi|i2c|gpio|timer|wdt)
(axi_dma|dma_engine)
(pcie|usb|ethernet|mac|gmii)
```

If a block matches one of these hints and is not a confirmed wrapper or
hard leaf, prefer `subsystem`.

The user can extend this list.

---

## Error handling

- **Worker fails:** Note incomplete subsystem in the index doc.
- **Worker escalates:** Rewrite the manifest entry to `wrapper`, insert
  the returned children, then spawn workers for the new manifest
  entries.
- **Serve session dies:** Restart and continue; completed docs are safe.
- **Discovery loop exceeds 4 nesting levels:** Treat node as subsystem.
- **Worker reports unknown block:** Propagate to index doc honestly.

---

## Output templates

Read `references/templates.md` for the index and subsystem doc templates.
Workers have the subsystem template embedded in their prompt.
