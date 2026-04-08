# RTL Crawler Worker Prompt Template

The orchestrator fills in `{placeholders}` below and passes the result
as the subagent's task prompt.  The worker receives a self-contained
prompt — it does not need to read any other skill files.

---

BEGIN TEMPLATE (everything below this line is the worker prompt):

---

# Task: Deep-crawl subsystem `{subsystem_instance}`

You are a worker agent in the RTL Crawler flow.  Your job is to explore
one subsystem of an RTL design and write a markdown architecture doc.

## Your assignment

| Field | Value |
|-------|-------|
| Subsystem instance | `{subsystem_instance}` |
| Module type | `{module_type}` |
| Source file | `{source_file}` |
| Max crawl depth | `{max_depth}` |
| Output file | `{output_dir}/{output_filename}` |
| Serve session ID | `{session_id}` |
| Parent wrapper | `{parent_wrapper}` |
| Instance array? | `{is_array_representative}` (count: `{array_count}`) |

## Sibling subsystems (do not crawl into these)

These are the other subsystems in the design.  Use them in `--stop-at`
regex when tracing signals, so your traces do not fan out into other
subsystems.  Escape each literal instance path before building the regex:

```
{sibling_subsystems}
```

## Coverage fingerprints already covered by other workers

The orchestrator has identified structurally equivalent internal blocks
that appear in multiple subsystems.  Another worker is already
documenting each one.  When you encounter one of these blocks inside
your subtree, note the instance in your hierarchy tree but **skip the
deep crawl only if the quick fingerprint really matches**:

- same module type
- same source file
- same immediate-child module fingerprint

If only the module type matches, that is **not enough** to skip the
block.  Parameterization and surrounding structure can change the
architecture materially.

In the
"Key internal blocks" section, write a cross-reference instead:

> **u_axi_bridge** (`axi_protocol_bridge`): Same module type documented
> in [chip_top_u_noc__noc_top_architecture.md](./chip_top_u_noc__noc_top_architecture.md).
> This instance has the same quick fingerprint, so the deep crawl was
> omitted here.

If you discover that your instance is configured differently from the
one the other worker documented (different child count, different
signal names suggesting different parameterization), go ahead and
document it anyway and note the difference.

```
{covered_blocks}
```

## Tools available

You have access to `rtl_trace_serve_query`.  A serve session is already
running — use the session ID above for all queries.  Do NOT start or
stop the serve session; the orchestrator manages its lifecycle.

Key commands:

```python
# Expand hierarchy
rtl_trace_serve_query("{session_id}", "hier --root {path} --depth 2 --format json --show-source")

# Identify a specific instance
rtl_trace_serve_query("{session_id}", "whereis-instance --instance {path} --format json")

# Search for signals
# IMPORTANT: `find --query` is literal substring match unless `--regex`
# is passed.  When you use wildcards or anchors, always pass `--regex`,
# and escape the literal instance path before embedding it in the regex.
rtl_trace_serve_query(
    "{session_id}",
    "find --query '^<escaped_path>\\.[^.]*pattern' --regex --limit N --format json",
)

# Trace connectivity
rtl_trace_serve_query("{session_id}", "trace --mode drivers --signal {sig} --depth 2 --format json")
rtl_trace_serve_query("{session_id}", "trace --mode loads --signal {sig} --depth 2 --format json")
```

---

## Step 0 — Wrapper check (do this FIRST)

Before deep-crawling, check whether your assigned block is actually a
wrapper — a hierarchical container that holds multiple major sub-
subsystems rather than being a single functional unit.

```python
rtl_trace_serve_query("{session_id}",
    "hier --root {subsystem_instance} --depth 1 --format json --show-source")
```

Look at the children.  Your block is likely a **wrapper** if ALL of
these are true:

1. It has 2 or more children that are themselves substantial blocks
   (each having their own internal hierarchy — check with another
   `hier --depth 1` on each child if needed).
2. The children look like independent subsystems with distinct
   functions (e.g. a D2D controller, a PCIe complex, and a memory
   interface — not pipeline stages or datapath components of the
   same function).
3. The block itself has very few **direct** local signals beyond clock,
   reset, and passthrough wires.  Search only signals directly under the
   instance, not deep descendants:
   ```python
   local_prefix = "^" + escape_regex("{subsystem_instance}") + r"\.[^.]*"
   rtl_trace_serve_query("{session_id}",
       "find --query '{local_prefix}(fsm|state|mode|sel)' --regex --limit 5 --format json")
   rtl_trace_serve_query("{session_id}",
       "find --query '{local_prefix}(valid|ready|req|gnt)' --regex --limit 5 --format json")
   ```
   Few or no results = likely a wrapper.

**If it IS a wrapper → ESCALATE.**  Do not try to deep-crawl all
sub-subsystems yourself.  Instead:

1. Write a brief wrapper doc (see wrapper doc template below).
2. Return an ESCALATION response (see Step 5b) with the list of
   sub-subsystems.  The orchestrator will spawn dedicated workers
   for each one.

**If it is NOT a wrapper** (it's a real functional unit), proceed to
Step 1.

If the evidence is mixed, err toward **not** escalating.  A false
wrapper classification is worse than documenting one slightly broader
subsystem.

How to distinguish wrappers from real subsystems — examples:

| Block | Children | Local logic | Verdict |
|-------|----------|-------------|---------|
| `io_die` | d2d_subsys, pcie_subsys, mem_if | None | **Wrapper** — escalate |
| `noc_top` | xbar, decoder, arb, addr_map | FSMs, muxes | **Subsystem** — crawl |
| `cpu_cluster` | core[0..3], l2_cache, snoop_ctrl | Snoop FSM | **Subsystem** — crawl |
| `compute_tile` | cpu_cluster, local_mem, dma | None | **Wrapper** — escalate |

The key distinction: a wrapper exists only to group things together; a
subsystem has its own functional logic that coordinates its children.

---

## Step 1 — Expand the hierarchy

Starting from `{subsystem_instance}`, expand the hierarchy level by
level:

```python
rtl_trace_serve_query("{session_id}",
    "hier --root {subsystem_instance} --depth 2 --format json --show-source")
```

Use `--depth 2` to batch two levels per call.

For each child instance, classify it:

1. **Leaf IP** — module name matches a hard leaf pattern (see below),
   or `hier --depth 1` shows zero children.
   Record it, do NOT expand further.
2. **Instance array** — multiple siblings share indexed naming, the
   same module type, the same source file, and the same immediate-child
   module fingerprint.  Explore ONE representative, note the count.
3. **Interior node** — queue it for the next expansion.

If the siblings share a module type but their source files or immediate
child-module fingerprints differ, treat them as separate nodes.

Repeat level by level until:
- Current depth from `{subsystem_instance}` >= `{max_depth}`
- All remaining children are leaf IPs or already visited
- Your judgment says to stop (see §Stopping heuristics)

## Step 2 — Detect interfaces at each visited node

Search for bus handshake and clock/reset signals:

```python
node_prefix = "^" + escape_regex("{node}") + r"\.[^.]*"
rtl_trace_serve_query("{session_id}", "find --query '{node_prefix}(clk|clock)' --regex --limit 20 --format json")
rtl_trace_serve_query("{session_id}", "find --query '{node_prefix}(rst|reset)' --regex --limit 20 --format json")
rtl_trace_serve_query("{session_id}", "find --query '{node_prefix}.*valid' --regex --limit 30 --format json")
rtl_trace_serve_query("{session_id}", "find --query '{node_prefix}.*ready' --regex --limit 30 --format json")
```

Bus protocol fingerprinting:
- `aw/ar/w/r/b` channels with valid/ready → AXI
- `haddr`, `htrans`, `hready` → AHB
- `psel`, `penable`, `paddr` → APB
- `tvalid`, `tready`, `tdata` → AXI-Stream

Also probe for control signals:

```python
rtl_trace_serve_query("{session_id}", "find --query '{node_prefix}(fsm|state|mode|sel)' --regex --limit 10 --format json")
rtl_trace_serve_query("{session_id}", "find --query '{node_prefix}(enable|en|grant|arb)' --regex --limit 10 --format json")
```

## Step 3 — Trace key connections

For each detected bus interface, trace ONE handshake signal:

```python
rtl_trace_serve_query("{session_id}",
    "trace --mode drivers --signal {node}.awvalid --depth 2 --format json")
rtl_trace_serve_query("{session_id}",
    "trace --mode loads --signal {node}.awvalid --depth 2 --format json")
```

Be selective:
- Trace `*valid` or `*ready` per bus, not every data bit.
- Trace clock and reset to identify domains.
- Use `--depth 2` for one hop beyond the immediate connection.
- Use `--stop-at` to avoid tracing into sibling subsystems.  Build the
  regex from escaped instance paths:

```python
rtl_trace_serve_query("{session_id}",
    "trace --mode drivers --signal {sig} --depth 3 --stop-at '{sibling_regex}' --format json")
```

Where `{sibling_regex}` is built from the sibling subsystems list.

## Step 4 — Write the subsystem doc

Write the markdown file to `{output_dir}/{output_filename}`.

Use this exact template:

```markdown
# {Subsystem Name} — Architecture

## Overview

- **Instance:** `{subsystem_instance}`
- **Module:** `{module_type}`
- **Source:** `{source_file}`
- **Clock domain(s):** {comma-separated list}
- **Reset(s):** {comma-separated list with polarity}
- **Parent:** `{parent_wrapper}` (or top-level)
- **Instance coverage:** {If you are an array representative, list all
  instances sharing this architecture.  E.g. "Covers compute_die[0..3]
  (×4).  Only [0] was explored."  Omit if unique instance.}

## Role in the system

{1-3 sentences describing what this subsystem does.  Base this on the
module name, its interfaces, and its internal structure.  If unsure,
say "inferred from structure" or "purpose unclear — requires spec review."}

## External interfaces

| Direction | Protocol | Connected to | Key signals |
|-----------|----------|--------------|-------------|
| {Master/Slave} | {AXI/AHB/APB/custom} | `{peer_instance}` | {summary} |

## Internal hierarchy

    {subsystem_root}
    ├── u_ctrl          (control_fsm)           — Main control FSM
    ├── u_datapath      (dp_pipeline)           — 4-stage pipeline
    │   ├── u_stage[0..3] (pipe_stage)          — Pipeline stages ×4
    │   └── u_bypass      (bypass_mux)          — Forwarding logic
    ├── u_regfile       (register_file_2r1w)    — Register file [leaf IP]
    └── u_clk_gate      (clock_gate_cell)       — ICG [leaf IP]

Mark leaf IPs with `[leaf IP]`.  Mark arrays with `×{count}`.

## Key internal blocks

### {instance_name} ({module_type})

- **Source:** `{source_file}:{line}`
- **Purpose:** {1-2 sentences}
- **Clock:** `{clk}` | **Reset:** `{rst}`
- **Key signals:** `{sig1}`, `{sig2}`, `{sig3}`
- **Interfaces:** {bus connections within the subsystem}

{Repeat for each notable internal block.  Skip trivial glue, clock
gates, and standard cells.}

## Notes for debugging agents

- {Where to start for understanding transaction flow}
- {Gotchas: clock gating, async crossings, muxing hazards}
- {Cross-references to related subsystem docs}
```

## Step 5 — Return your result

At the very end of your response, output exactly ONE of the following
blocks.  The orchestrator parses this, so follow the format precisely.

### Step 5a — Normal summary (you completed the deep crawl)

```
=== WORKER SUMMARY ===
subsystem_instance: {subsystem_instance}
module_type: {module_type}
source_file: {source_file}
output_file: {output_filename}
parent_wrapper: {parent_wrapper}

clock_domains:
  - {clk_1}
  - {clk_2}

resets:
  - {rst_1} ({polarity}, {sync/async})

external_interfaces:
  - direction: {master/slave}
    protocol: {AXI/AHB/APB/custom}
    peer: {peer_instance_path}
    key_signals: {summary}

top_level_children:
  - {inst} ({module}) — {short description}
  - {inst}[0..N] ({module}) — {description} ×{count}
  - {inst} ({module}) — {description} [leaf IP]

debugging_notes:
  - {note 1}
  - {note 2}
=== END SUMMARY ===
```

### Step 5b — Escalation (you found the block is a wrapper)

Use this ONLY if Step 0 determined the block is a wrapper.

Write a brief wrapper doc first:

```markdown
# {Wrapper Name} — Hierarchy

## Overview

- **Instance:** `{subsystem_instance}`
- **Module:** `{module_type}`
- **Source:** `{source_file}`
- **Role:** Hierarchical container for the following subsystems.

## Contents

| Instance | Module | Doc |
|----------|--------|-----|
| `{child_1}` | `{module_1}` | [{child_1_slug}__{module_1_slug}_architecture.md](./{child_1_slug}__{module_1_slug}_architecture.md) |
| `{child_2}` | `{module_2}` | [{child_2_slug}__{module_2_slug}_architecture.md](./{child_2_slug}__{module_2_slug}_architecture.md) |

## Shared infrastructure

{List any clock/reset generation, shared buses, or glue logic that
lives at the wrapper level.  If none: "None — pure hierarchical wrapper."}
```

Then output the escalation block:

```
=== WORKER ESCALATION ===
subsystem_instance: {subsystem_instance}
module_type: {module_type}
reason: "{brief explanation of why this is a wrapper}"
wrapper_doc: {output_filename}

sub_manifest:
  - subsystem: {child_1_path}  module: {child_1_module}
  - subsystem: {child_2_path}  module: {child_2_module}
  - instance_array: {child_3_path}[0..N]  module: {child_3_module}  count: {N+1}
=== END ESCALATION ===
```

---

## Hard leaf patterns

These are **hard leaf** module-name patterns (case-insensitive regex).
Do not crawl deeper when a module type matches any of these:

```
{hard_leaf_patterns}
```

These are **subsystem IP hints**.  They often deserve their own docs.
They are never hard-stop patterns:

```
{subsystem_ip_hints}
```

## Stopping heuristics

### Hard stops

| Condition | Detection |
|-----------|-----------|
| Depth limit reached | current_depth >= {max_depth} |
| Module matches hard leaf pattern | Regex match on module type |
| Leaf node (no children) | hier --depth 1 returns zero children |
| Equivalent block already explored | Same quick fingerprint fully crawled elsewhere — cross-reference |

### Soft stops (use judgment)

- **Vendor/private IP names** — module name suggests well-known internal IP and the block is already clearly below the subsystem level → record and stop.
- **Repetitive structure** — all children identical → document one, note count.
- **Vendor source** — show-source path is in `/ip_lib/`, `/vendor/`, etc.
- **Diminishing returns** — you know enough to describe the block.

## Practical tips

- Budget tool calls: use `--depth 2` on hier, `--limit` on find, `--stop-at` on trace.
- Handle unknowns honestly — "purpose unclear" beats a wrong guess.
- Track visited instance paths and quick fingerprints — skip only true duplicates and cross-reference.
