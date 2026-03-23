---
name: standalone-trace
description: Use for RTL signal tracing with rtl_trace. Trigger when debugging synthesizable Verilog/SystemVerilog connectivity, including driver/load origin, cross-module port propagation, bit-slice impact, hierarchical signal lookup, and traversal stop diagnostics.
---

# Standalone Trace

Use this skill to run `rtl_trace` for common RTL debug workflows.
Assume `rtl_trace` is already available in `$PATH`.

## Workflow

1. Compile or refresh trace DB from RTL sources:
```bash
rtl_trace compile --db <db_path> --top <top_module> \
  [--incremental] [--relax-defparam] [--mfcu] \
  [--partition-budget N] [--compile-log <file>] [slang args...]
```

2. Search signal names when exact hierarchy is unknown:
```bash
rtl_trace find --db <db_path> --query <text> [--regex] [--limit N] [--format text|json]
```

3. Trace drivers or loads:
```bash
rtl_trace trace --db <db_path> --mode <drivers|loads> --signal <hier.path> \
  [--cone-level N] [--prefer-port-hop] [--depth N] [--max-nodes N] \
  [--include RE] [--exclude RE] [--stop-at RE] [--format text|json]
```

4. Query hierarchy tree:
```bash
rtl_trace hier --db <db_path> [--root <hier.path>] [--depth N] [--max-nodes N] [--format text|json]
```

## Query Patterns

- Single signal:
```bash
rtl_trace trace --db <db_path> --mode drivers --signal top.u0.sig
```

- Compile with timing log:
```bash
rtl_trace compile --db rtl_trace.db --top top --compile-log compile.log -f rtl.f
```

- Bit or slice query:
```bash
rtl_trace trace --db <db_path> --mode loads --signal top.u0.bus[3]
rtl_trace trace --db <db_path> --mode loads --signal top.u0.bus[31:16]
```

- Multi-level logic cone (single command):
```bash
rtl_trace trace --db <db_path> --mode drivers --signal top.u0.sig --cone-level 2
rtl_trace trace --db <db_path> --mode loads --signal top.u0.sig --cone-level 3
```

- Prefer port bridging when expression context has no useful RHS/LHS:
```bash
rtl_trace trace --db <db_path> --mode drivers --signal top.u0.sig --prefer-port-hop
```

- Machine-readable output:
```bash
rtl_trace trace --db <db_path> --mode loads --signal top.u0.sig --format json
rtl_trace hier --db <db_path> --root top.u0 --format json
```

- Bounded traversal:
```bash
rtl_trace trace --db <db_path> --mode drivers --signal top.u0.sig \
  --depth 8 --max-nodes 5000 --include "top\\." --exclude "debug|scan"
```

## Output Requirements

- Report exact commands used.
- Report `target`, `mode`, `count`, key endpoints, and stops.
- For `drivers`, include `rhs` paths when present.
- For `loads`, include `lhs` paths when present.
- For bit-slice results, include `bit_map` and `bit_map_approximate`.
- For hierarchy results, include `root`, `node_count`, `truncated`, and tree summary.
- If signal is not found, run `find` and report top suggestions.

## Practical Rules

- `compile` requires `--top`; if it is missing, fix the command before retrying.
- Prefer `--incremental` for repeated DB compile on unchanged inputs.
- Use `--relax-defparam` when the design has unresolved cross-hier `defparam` issues but you still need a usable DB.
- Use `--mfcu` when the source flow expects grouped compilation units from filelists.
- Use `--compile-log <file>` when profiling compile time; it records major compile steps plus `save_db_streaming` substeps such as `collect_strings`, `write_header_tables`, `emit_signals`, `write_hierarchy`, and `write_global_nets`.
- Use `--partition-budget N` only when you explicitly want partitioned DB emission.
- Recompile DB after RTL/filelist/define/include changes.
- If output includes `depth_limit` / `node_limit`, rerun with larger limits.
- If hierarchy output is too large, rerun with `--root` and smaller `--depth`.
- Use `--cone-level N` to expand logic cone automatically:
  - `drivers`: follow assignment RHS signals backward.
  - `loads`: follow assignment LHS signals forward.
- Use `--prefer-port-hop` when the first hit is a port-connection expression and you want traversal to continue across the instance boundary.
- Re-run `compile` after trace-engine changes if you need updated cross-port recursion or `loads`-side `lhs` information in the DB.
- Treat `bit_map_approximate=true` as unresolved static range and report it explicitly.
