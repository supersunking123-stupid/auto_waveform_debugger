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
| `{instance_path}` | `{module_type}` | Subsystem | [{subsystem}_architecture.md](./{subsystem}_architecture.md) |
| `{instance_path}` | `{module_type}` | Leaf IP ({brief description}) | — |
| `{instance_path}[0..{N-1}]` | `{module_type}` | Instance array (×{N}) | [{subsystem}_architecture.md](./{subsystem}_architecture.md) |

### Hierarchy tree

Draw a condensed indented tree that **stops at subsystem boundaries**.
Do NOT expand subsystem internals here — those belong in the per-subsystem
docs. Each subsystem line should end with a link to its doc.

    top (top)
    ├── u_noc              (noc_top)          — NOC crossbar           → [doc](./noc_top_architecture.md)
    ├── u_cpu              (riscv_core_wrap)  — CPU subsystem          → [doc](./riscv_core_wrap_architecture.md)
    ├── u_gpu              (gpu_wrapper)      — GPU subsystem          → [doc](./gpu_wrapper_architecture.md)
    ├── u_ddr_ctrl         (ddr4_mc)          — DDR controller         [leaf IP]
    ├── u_core[0..3]       (riscv_core)       — CPU cores ×4           [leaf IP]
    └── u_periph           (periph_bus)       — Peripheral bus         [leaf]

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

## Subsystem document: `{subsystem}_architecture.md`

```markdown
# {Subsystem Name} — Architecture

## Overview

- **Instance:** `{instance_path}`
- **Module:** `{module_type}`
- **Source:** `{source_file}:{line}`
- **Clock domain(s):** {comma-separated list}
- **Reset(s):** {comma-separated list with polarity}

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
- Subsystem files: `{subsystem_name}_architecture.md`
  - Use the module type name (not the instance name) as the subsystem name.
  - Convert to lowercase and replace spaces/special chars with underscores.
  - Examples: `noc_top_architecture.md`, `gpu_wrapper_architecture.md`

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
