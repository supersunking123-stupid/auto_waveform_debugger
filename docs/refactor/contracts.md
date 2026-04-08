# Frozen External Contracts

**Date:** 2026-03-31
**Source:** docs/MCP_SIGNATURES.md, standalone_trace/README.md, waveform_explorer/README.md

These contracts are IMMUTABLE during Phases 1–3 of the refactor. Any change requires Main Agent approval.

---

## 1. rtl_trace CLI Contract

### Subcommands

| Subcommand | Required Flags | Optional Flags |
|---|---|---|
| `compile` | `--db`, `--top` | `--incremental`, `--relax-defparam`, `--mfcu`, `--low-mem`, `--partition-budget N`, `--compile-log <file>`, `--timescale`, plus slang passthrough (`-f`, `-y`, `+incdir+...`, source paths) |
| `trace` | `--db`, `--mode`, `--signal` | `--cone-level N`, `--prefer-port-hop`, `--depth N`, `--max-nodes N`, `--include RE`, `--exclude RE`, `--stop-at RE`, `--format text\|json` |
| `find` | `--db`, `--query` | `--regex`, `--limit N`, `--format text\|json` |
| `hier` | `--db` | `--root <path>`, `--depth N`, `--max-nodes N`, `--format text\|json`, `--show-source` |
| `whereis-instance` | `--db`, `--instance` | `--format text\|json`, `--show-params` |
| `serve` | `--db` | (interactive mode) |

### trace --mode values
- `drivers`
- `loads`

### trace --signal forms
- Full hierarchical: `top.u0.sig`
- Bit select: `top.u0.sig[3]`
- Part select: `top.u0.sig[7:4]`

### serve mode protocol
- Commands entered one line at a time
- Each response ends with `<<END>>`
- Supports: `find`, `trace`, `hier`, `whereis-instance`

### GraphDb binary format
- Current version: **v4**
- Do NOT bump unless necessary

### JSON output schema (trace --format json)
```json
{
  "target": "...",
  "mode": "drivers|loads",
  "summary": { "count": N, "visited": N },
  "endpoints": [ ... ],
  "stops": [ ... ]
}
```

---

## 2. wave_agent_cli JSON Protocol

### Invocation
```bash
./wave_agent_cli <waveform_file> '<json_query>'
```

Supported file extensions: `.vcd`, `.fst`, `.fsdb`

### Commands

| Command | Required Args | Optional Args |
|---|---|---|
| `list_signals` | — | `pattern`, `types` |
| `list_signals_page` | `prefix` | `cursor`, `limit` |
| `get_snapshot` | `signals`, `time` | `radix` |
| `get_value_at_time` | `path`, `time` | `radix` |
| `get_transitions` | `path` | `start_time`, `end_time`, `max_limit` |
| `get_signal_overview` | `path` | `start_time`, `end_time`, `resolution`, `radix` |
| `find_edge` | `path`, `edge_type`, `start_time` | — |
| `find_value_intervals` | `path`, `value` | `start_time`, `end_time`, `radix` |
| `analyze_pattern` | `path` | `start_time`, `end_time` |
| `find_condition` | `expression` | `start_time` |

### Value representation
- 1-bit stable: `0`, `1`, `x`, `z`
- 1-bit edge: `rising`, `falling`
- Multi-bit stable: hex-prefixed (e.g., `h0a`), or binary/decimal based on radix
- Multi-bit at transition: `changing`

### Edge types
- `posedge`, `negedge`, `anyedge`

### Path normalization
- Leading `TOP.` is stripped for consistency
- FSDB bare names resolve to unique packed-vector signals

### FSDB-specific behavior
- `list_signals_page` is FSDB-only
- FSDB packed-signal resolution: bare name → unique packed match
- Verdi banner lines filtered from stdout

---

## 3. MCP Tool Contract (Main Agent MCP)

Source: `docs/MCP_SIGNATURES.md` — this is the frozen golden reference.

### RTL trace tools
```
rtl_trace(args, rtl_trace_bin=None, timeout_sec=300)
rtl_trace_serve_start(serve_args=None, rtl_trace_bin=None)
rtl_trace_serve_query(session_id, command_line)
rtl_trace_serve_stop(session_id)
```

### Waveform passthrough tools
```
wave_agent_query(vcd_path, cmd, args=None, wave_cli_bin=None)
list_signals(vcd_path=None, pattern="", types=None, session_name=None)
get_signal_info(vcd_path=None, path="", session_name=None)
get_snapshot(vcd_path=None, signals=None, time=0, radix="hex", signals_are_groups=False, session_name=None)
get_value_at_time(vcd_path=None, path="", time=0, radix="hex", session_name=None)
find_edge(vcd_path=None, path="", edge_type="anyedge", start_time=0, direction="forward", session_name=None)
find_value_intervals(vcd_path=None, path="", value="", start_time=0, end_time=0, radix="hex", session_name=None)
find_condition(vcd_path=None, expression="", start_time=0, direction="forward", session_name=None)
get_transitions(vcd_path=None, path="", start_time=0, end_time=0, max_limit=50, session_name=None)
get_signal_overview(vcd_path=None, path="", start_time=0, end_time=0, resolution="auto", radix="hex", session_name=None)
analyze_pattern(vcd_path=None, path="", start_time=0, end_time=0, session_name=None)
```

### Session tools
```
create_session(waveform_path, session_name, description="")
list_sessions(waveform_path=None)
get_session(session_name=None, waveform_path=None)
switch_session(session_name, waveform_path=None)
delete_session(session_name, waveform_path=None)
set_cursor(time, waveform_path=None, session_name=None)
move_cursor(delta, waveform_path=None, session_name=None)
get_cursor(waveform_path=None, session_name=None)
create_bookmark(bookmark_name, time, description="", waveform_path=None, session_name=None)
delete_bookmark(bookmark_name, waveform_path=None, session_name=None)
list_bookmarks(waveform_path=None, session_name=None)
create_signal_group(group_name, signals, description="", waveform_path=None, session_name=None)
update_signal_group(group_name, signals=None, description=None, waveform_path=None, session_name=None)
delete_signal_group(group_name, waveform_path=None, session_name=None)
list_signal_groups(waveform_path=None, session_name=None)
```

### Cross-link tools
```
trace_with_snapshot(db_path, waveform_path=None, signal="", time=0, mode="drivers", trace_options=None, sample_offsets=None, clock_path=None, cycle_offsets=None, rank_window_before=None, rank_window_after=None, rtl_trace_bin=None, wave_cli_bin=None, session_name=None)
explain_signal_at_time(db_path, waveform_path=None, signal="", time=0, mode="drivers", trace_options=None, rank_window_before=None, rank_window_after=None, rtl_trace_bin=None, wave_cli_bin=None, session_name=None)
rank_cone_by_time(db_path, waveform_path=None, signal="", time=0, mode="drivers", trace_options=None, window_start=None, window_end=None, rtl_trace_bin=None, wave_cli_bin=None, session_name=None)
explain_edge_cause(db_path, waveform_path=None, signal="", time=0, edge_type="anyedge", direction="backward", mode="drivers", trace_options=None, rank_window_before=None, rank_window_after=None, rtl_trace_bin=None, wave_cli_bin=None, session_name=None)
```

### Type aliases
- `TimeReference`: integer | `"Cursor"` | `"BM_<bookmark_name>"`
- `ResolutionReference`: integer | `"auto"`
- `EdgeType`: `posedge` | `negedge` | `anyedge`
- `EdgeTypeAlias`: `rise` | `rising` | `risingedge` | `fall` | `falling` | `fallingedge` | `edge` | `any`
- `Direction`: `forward` | `backward`
- `TraceMode`: `drivers` | `loads`

---

## 4. Standalone Waveform MCP Contract

Source: `waveform_explorer/waveform_mcp.py`

```
list_signals(vcd_path, pattern="", types=None)
get_signal_info(vcd_path, path)
get_snapshot(vcd_path, signals, time, radix="hex")
get_value_at_time(vcd_path, path, time, radix="hex")
find_edge(vcd_path, path, edge_type, start_time, direction="forward")
find_value_intervals(vcd_path, path, value, start_time, end_time, radix="hex")
find_condition(vcd_path, expression, start_time, direction="forward")
get_transitions(vcd_path, path, start_time, end_time, max_limit=50)
get_signal_overview(vcd_path, path, start_time, end_time, resolution, radix="hex")
analyze_pattern(vcd_path, path, start_time, end_time)
```

Note: Standalone MCP uses `vcd_path` (required) instead of session-aware `vcd_path` (optional).
