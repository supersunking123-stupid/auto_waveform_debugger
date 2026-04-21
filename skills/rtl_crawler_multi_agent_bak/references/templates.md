# RTL Crawler — Output Templates

Use these templates exactly when generating the crawl output documents.
Fill in the `{placeholders}` with actual data from your crawl. Remove
any sections that are not applicable (e.g. if you found no instance
arrays, drop that column from the table).

---

## Index document: `design_index.md`

```markdown
# {Design Name} — Architecture Overview

## Design metadata

- **Top module:** `{top_module}`
- **DB path:** `{db_path}`
- **Crawl date:** {YYYY-MM-DD}
- **Crawl depth limit:** {max_depth}

## Top-level hierarchy

| Instance | Module type | Category | Doc |
|----------|-------------|----------|-----|
| `{instance_path}` | `{module_type}` | Subsystem | [{instance_slug}__{module_type_slug}_architecture.md](./{instance_slug}__{module_type_slug}_architecture.md) |
| `{instance_path}` | `{module_type}` | Wrapper | [{instance_slug}__{module_type_slug}_architecture.md](./{instance_slug}__{module_type_slug}_architecture.md) |
| `{instance_path}` | `{module_type}` | Leaf IP ({brief description}) | — |
| `{instance_path}[0..{N-1}]` | `{module_type}` | Instance array (×{N}) | [{instance_slug}__{module_type_slug}_architecture.md](./{instance_slug}__{module_type_slug}_architecture.md) |

### Hierarchy tree

Draw a condensed indented tree that reflects wrapper nesting and **stops
at subsystem boundaries**.  Do NOT expand subsystem internals here.
Each subsystem or wrapper line should end with its doc link.

    chip_top
    ├── compute_die[0..3]      (compute_die ×4)      → [doc](./chip_top_compute_die_0__compute_die_architecture.md)
    ├── io_die                 (io_die_top)          — wrapper → [doc](./chip_top_io_die__io_die_top_architecture.md)
    │   ├── d2d_subsys[0..3]   (d2d_subsystem ×4)    → [doc](./chip_top_io_die_d2d_subsys_0__d2d_subsystem_architecture.md)
    │   ├── pcie_subsys        (pcie_complex)        → [doc](./chip_top_io_die_pcie_subsys__pcie_complex_architecture.md)
    │   └── mem_if_subsys      (mem_interface)       → [doc](./chip_top_io_die_mem_if_subsys__mem_interface_architecture.md)
    └── global_clk_rst         (clk_rst_gen)         — leaf IP

## System interconnect

| Source | Destination | Protocol | Purpose |
|--------|-------------|----------|---------|
| `{src_instance}` | `{dst_instance}` | {AXI/AHB/APB/custom} | {brief purpose} |

### Block diagram (text)

Draw a simple ASCII block diagram showing the major blocks and their
connections.  Keep it to the top-level subsystems only.  Example:

    ┌──────────┐     AXI      ┌──────────┐     AXI      ┌──────────┐
    │   CPU    │─────────────▶│   NOC    │─────────────▶│   DDR    │
    │ subsystem│              │ crossbar │              │  ctrl    │
    └──────────┘              └──────────┘              └──────────┘
                                   │
                                   │ APB
                                   ▼
                              ┌──────────┐
                              │  Periph  │
                              │   bus    │
                              └──────────┘

## Clock domains

| Domain | Source | Frequency (if known) | Consumers |
|--------|--------|----------------------|-----------|
| `{clk_name}` | {source, e.g. PLL instance} | {freq or "unknown"} | {list of subsystems} |

## Reset tree

| Reset | Type | Scope |
|-------|------|-------|
| `{rst_name}` | {active high/low, sync/async} | {which subsystems it covers} |

## Notes

{Any global observations: design style conventions, naming patterns,
known issues with the DB, blocks that need RTL source review, etc.}
```

---

## Subsystem document: `{instance_slug}__{module_type_slug}_architecture.md`

```markdown
# {Subsystem Name} — Architecture

## Overview

- **Instance:** `{instance_path}`
- **Module:** `{module_type}`
- **Source:** `{source_file}:{line}`
- **Clock domain(s):** {comma-separated list}
- **Reset(s):** {comma-separated list with polarity}
- **Parent:** `{parent_wrapper}` (or top-level)
- **Instance coverage:** {If this is an instance array representative, list
  all instances that share this architecture, e.g. "This document covers
  `chip_top.compute_die[0]` through `chip_top.compute_die[3]` (×4, identical
  module type `compute_die`, same source, same immediate-child fingerprint).
  Only `compute_die[0]` was explored."
  If this is a unique instance, omit this field entirely.}

## Role in the system

{1-3 sentences describing what this subsystem does.  Base this on the
module name, its interfaces, and its internal structure.  If you are
unsure, say "inferred from structure" or "purpose unclear — requires
spec review."}

## External interfaces

| Direction | Protocol | Connected to | Key signals |
|-----------|----------|--------------|-------------|
| {Master/Slave/Bidirectional} | {AXI/AHB/APB/custom} | `{peer_instance}` | {channel summary, e.g. "aw/ar/w/r/b"} |

## Internal hierarchy

Use an indented tree.  For each node, show: instance name, module type
in parentheses, and a short annotation.

    {subsystem_root}
    ├── u_ctrl          (control_fsm)           — Main control state machine
    ├── u_datapath      (dp_pipeline)           — 4-stage data pipeline
    │   ├── u_stage[0..3] (pipe_stage)          — Identical pipeline stages ×4
    │   └── u_bypass      (bypass_mux)          — Bypass/forwarding logic
    ├── u_regfile       (register_file_2r1w)    — 32-entry register file [leaf IP]
    └── u_clk_gate      (clock_gate_cell)       — ICG for power management [leaf IP]

Mark leaf IPs with `[leaf IP]` so readers know you did not crawl further.
Mark instance arrays with `×{count}` and note that only one was explored.

## Key internal blocks

For each non-leaf child that you explored, write a subsection:

### {instance_name} ({module_type})

- **Source:** `{source_file}:{line}`
- **Purpose:** {what this block does, 1-2 sentences}
- **Clock:** `{clk_name}` | **Reset:** `{rst_name}`
- **Key signals:** `{signal_1}`, `{signal_2}`, `{signal_3}`
- **Interfaces:** {bus connections within the subsystem}

{Repeat for each notable internal block.  Skip trivial glue logic,
clock gates, and standard cells — they are already visible in the
hierarchy tree above.}

## Notes for debugging agents

{Practical tips for anyone debugging this subsystem.  Examples:}

- {Where to start: "The main FSM in u_ctrl is the best entry point
  for understanding transaction flow."}
- {Gotchas: "Clock gating via u_clk_gate means datapath signals may
  freeze when idle — check clk_en first."}
- {Known tricky areas: "The bypass mux u_bypass is a common source of
  data corruption — inspect bypass_sel."}
- {Cross-references: "This subsystem shares the AXI bus with u_gpu —
  see gpu_architecture.md for the other side."}
```

---

## Naming conventions

- Index file: `design_index.md`
- Subsystem/wrapper files: `{instance_slug}__{module_type_slug}_architecture.md`
  - `instance_slug` is the full instance path converted to lowercase and
    normalized to underscores.
  - Keep enough hierarchy in the slug to avoid collisions between
    different instances of the same module type.
  - For instance arrays, use the explored representative in the slug.
  - Examples:
    - `chip_top_u_noc__noc_top_architecture.md`
    - `chip_top_io_die__io_die_top_architecture.md`
    - `chip_top_compute_die_0__compute_die_architecture.md`

If two instances share a module type but differ in source file or
immediate-child fingerprint, they must get separate docs.

---

## Wrapper document: `{instance_slug}__{module_type_slug}_architecture.md`

```markdown
# {Wrapper Name} — Hierarchy

## Overview

- **Instance:** `{instance_path}`
- **Module:** `{module_type}`
- **Source:** `{source_file}:{line}`
- **Parent:** `{parent_wrapper}` (or top-level)
- **Role:** Hierarchical container for the following subsystems.

## Contents

| Instance | Module | Doc |
|----------|--------|-----|
| `{child_1}` | `{module_1}` | [{child_1_slug}__{module_1_slug}_architecture.md](./{child_1_slug}__{module_1_slug}_architecture.md) |
| `{child_2}` | `{module_2}` | [{child_2_slug}__{module_2_slug}_architecture.md](./{child_2_slug}__{module_2_slug}_architecture.md) |

## Shared infrastructure

{List any clock/reset generation, shared buses, or glue logic that
lives at the wrapper level rather than inside a child subsystem.  If
none, write "None — pure hierarchical wrapper."}
```

---

## What NOT to put in the docs

- Full signal lists — these bloat the file and are easily queried from
  the DB.  Document only key/interface signals.
- RTL source code — the docs point to source files, they don't reproduce
  them.
- Waveform data — the crawler is purely structural.  Waveform-based
  analysis is a separate concern.
- Speculation about behavior — stick to what the structure tells you.
  If you need to infer behavior, flag it clearly as inference.
