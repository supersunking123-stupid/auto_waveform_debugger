# RTL Crawler — Output Templates

Use these templates exactly when generating the crawl output documents.
Fill in the `{placeholders}` with actual data from your crawl. Remove
any sections that are not applicable.

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

Draw a condensed indented tree that reflects wrapper nesting and stops
at subsystem boundaries. Do not expand subsystem internals here. Each
subsystem or wrapper line should end with its doc link.

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
connections. Keep it to the top-level subsystems only.

## Clock domains

| Domain | Source | Frequency (if known) | Consumers |
|--------|--------|----------------------|-----------|
| `{clk_name}` | {source} | {freq or "unknown"} | {list of subsystems} |

## Reset tree

| Reset | Type | Scope |
|-------|------|-------|
| `{rst_name}` | {active high/low, sync/async} | {which subsystems it covers} |

## Notes

{Any global observations: naming patterns, repeated infrastructure,
wrapper-heavy organization, blocks that need RTL review, or DB limits.}
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
- **Instance coverage:** {If this is an instance-array representative,
  list all instances it covers and note that only the representative was
  explored. Omit this field for unique instances.}

## Role in the system

{1-3 sentences describing what this subsystem does. Base this on the
module name, interfaces, and internal structure. If unsure, say the
role is inferred from structure or remains unclear.}

## External interfaces

| Direction | Protocol | Connected to | Key signals |
|-----------|----------|--------------|-------------|
| {Master/Slave/Bidirectional} | {AXI/AHB/APB/custom} | `{peer_instance}` | {summary} |

## Internal hierarchy

Use an indented tree. For each node, show instance name, module type,
and a short annotation.

    {subsystem_root}
    ├── u_ctrl          (control_fsm)           — Main control state machine
    ├── u_datapath      (dp_pipeline)           — 4-stage data pipeline
    │   ├── u_stage[0..3] (pipe_stage)          — Identical pipeline stages ×4
    │   └── u_bypass      (bypass_mux)          — Bypass and forwarding logic
    ├── u_regfile       (register_file_2r1w)    — Register file [leaf IP]
    └── u_clk_gate      (clock_gate_cell)       — ICG [leaf IP]

Mark leaf IPs with `[leaf IP]`. Mark instance arrays with `×{count}`.

## Key internal blocks

For each notable non-leaf child that you actually explored, write a
subsection:

### {instance_name} ({module_type})

- **Source:** `{source_file}:{line}`
- **Purpose:** {1-2 sentences}
- **Clock:** `{clk_name}` | **Reset:** `{rst_name}`
- **Key signals:** `{signal_1}`, `{signal_2}`, `{signal_3}`
- **Interfaces:** {bus or control connections within the subsystem}

If a repeated internal block was skipped because it matched a previously
documented quick fingerprint, write a cross-reference instead of a full
subsection.

## Notes for debugging agents

- {Best place to start for understanding transaction flow}
- {Gotchas such as clock gating, arbitration, CDC, or muxing hazards}
- {Cross-references to related subsystem docs}
```

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

{List any wrapper-owned clock/reset generation, shared buses, address
decoders, arbiters, retiming clusters, interrupt muxes, or simple glue.
If none, write "None — pure hierarchical wrapper."}
```

---

## Naming conventions

- Index file: `design_index.md`
- Subsystem and wrapper files: `{instance_slug}__{module_type_slug}_architecture.md`
  - `instance_slug` is the full instance path normalized to lowercase
    with separators converted to underscores.
  - Keep enough hierarchy in the slug to avoid collisions.
  - For instance arrays, use the explored representative in the slug.

Examples:
- `chip_top_u_noc__noc_top_architecture.md`
- `chip_top_io_die__io_die_top_architecture.md`
- `chip_top_compute_die_0__compute_die_architecture.md`

If two instances share a module type but differ in source file or
immediate-child fingerprint, they must get separate docs.

---

## What NOT to put in the docs

- Full signal lists
- RTL source code
- Waveform data
- Unmarked speculation
