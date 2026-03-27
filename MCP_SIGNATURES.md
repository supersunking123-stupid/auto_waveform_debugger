# MCP Signatures

This file lists the MCP tool signatures exposed by the local servers in this repo as of March 27, 2026.

## Main Agent MCP

Source: [`agent_debug_automation/agent_debug_automation_mcp.py`](/home/qsun/AI_PROJ/auto_waveform_debugger/agent_debug_automation/agent_debug_automation_mcp.py)

### RTL trace tools

```python
rtl_trace(args: List[str], rtl_trace_bin: Optional[str] = None, timeout_sec: int = 300)
rtl_trace_serve_start(serve_args: Optional[List[str]] = None, rtl_trace_bin: Optional[str] = None)
rtl_trace_serve_query(session_id: str, command_line: str)
rtl_trace_serve_stop(session_id: str)
```

#### `rtl_trace(...)` supported command families

`rtl_trace(...)` is a generic wrapper around the local `standalone_trace/build/rtl_trace` CLI. The supported commands are the CLI subcommands documented in [`standalone_trace/README.md`](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/README.md).

Common patterns:

```python
rtl_trace(args=["compile", "--db", "rtl_trace.db", "--top", "top", "-f", "files.f"])
rtl_trace(args=["trace", "--db", "rtl_trace.db", "--mode", "drivers", "--signal", "top.u0.sig"])
rtl_trace(args=["trace", "--db", "rtl_trace.db", "--mode", "loads", "--signal", "top.u0.bus[7:4]", "--format", "json"])
rtl_trace(args=["find", "--db", "rtl_trace.db", "--query", "foo.bar"])
rtl_trace(args=["hier", "--db", "rtl_trace.db", "--root", "top.u0", "--depth", "2", "--format", "json"])
```

Supported subcommands:

```text
compile
trace
find
hier
```

Important note:
- Do not call `rtl_trace(args=["serve", ...])` through this wrapper. Use `rtl_trace_serve_start`, `rtl_trace_serve_query`, and `rtl_trace_serve_stop` instead.

#### `compile`

Build or rebuild the structural DB from RTL.

Typical form:

```text
rtl_trace compile --db <db_path> --top <top_module> [--incremental] [--relax-defparam] [--mfcu] [--low-mem] [--partition-budget N] [--compile-log <file>] [slang source args...]
```

Common flags:
- `--db <db_path>`
- `--top <top_module>`
- `--incremental`
- `--relax-defparam`
- `--mfcu`
- `--low-mem`
- `--partition-budget <N>`
- `--compile-log <file>`
- passthrough slang/filelist args such as `-f`, `-y`, `+incdir+...`, source file paths

#### `trace`

Trace structural drivers or loads for a signal.

Typical form:

```text
rtl_trace trace --db <db_path> --mode <drivers|loads> --signal <hier.path> [--cone-level N] [--prefer-port-hop] [--depth N] [--max-nodes N] [--include RE] [--exclude RE] [--stop-at RE] [--format text|json]
```

Common flags:
- `--db <db_path>`
- `--mode drivers|loads`
- `--signal <hier.path>`
- `--cone-level <N>`
- `--prefer-port-hop`
- `--depth <N>`
- `--max-nodes <N>`
- `--include <regex>`
- `--exclude <regex>`
- `--stop-at <regex>`
- `--format text|json`

Signal forms supported by the docs:
- full hierarchical signal
- bit select, for example `top.sig[3]`
- part select, for example `top.sig[31:16]`

#### `find`

Search signal names in the DB.

Typical form:

```text
rtl_trace find --db <db_path> --query <text> [--regex] [--limit N] [--format text|json]
```

Common flags:
- `--db <db_path>`
- `--query <text>`
- `--regex`
- `--limit <N>`
- `--format text|json`

#### `hier`

Browse the structural instance hierarchy.

Typical form:

```text
rtl_trace hier --db <db_path> [--root <hier.path>] [--depth N] [--max-nodes N] [--format text|json]
```

Common flags:
- `--db <db_path>`
- `--root <hier.path>`
- `--depth <N>`
- `--max-nodes <N>`
- `--format text|json`

#### Serve mode tools

These expose the interactive `rtl_trace serve` backend without requiring agents to manage a shell process directly.

```python
rtl_trace_serve_start(serve_args: Optional[List[str]] = None, rtl_trace_bin: Optional[str] = None)
rtl_trace_serve_query(session_id: str, command_line: str)
rtl_trace_serve_stop(session_id: str)
```

Typical flow:

```python
rtl_trace_serve_start(serve_args=["--db", "rtl_trace.db"])
rtl_trace_serve_query(session_id="<sid>", command_line="find --query timeout --limit 5")
rtl_trace_serve_query(session_id="<sid>", command_line="trace --mode drivers --signal top.u0.sig --format json")
rtl_trace_serve_query(session_id="<sid>", command_line="hier --root top.u0 --depth 2")
rtl_trace_serve_stop(session_id="<sid>")
```

### Waveform passthrough tools

```python
wave_agent_query(vcd_path: str, cmd: str, args: Optional[Dict[str, Any]] = None, wave_cli_bin: Optional[str] = None)
list_signals(vcd_path: str)
get_signal_info(vcd_path: str, path: str)
get_snapshot(vcd_path: str, signals: List[str], time: int, radix: str = "hex")
get_value_at_time(vcd_path: str, path: str, time: int, radix: str = "hex")
find_edge(vcd_path: str, path: str, edge_type: EdgeType | EdgeTypeAlias, start_time: int, direction: Direction = "forward")
find_value_intervals(vcd_path: str, path: str, value: str, start_time: int, end_time: int, radix: str = "hex")
find_condition(vcd_path: str, expression: str, start_time: int, direction: Direction = "forward")
get_transitions(vcd_path: str, path: str, start_time: int, end_time: int, max_limit: int = 50)
analyze_pattern(vcd_path: str, path: str, start_time: int, end_time: int)
```

### Cross-linked structural and waveform analysis tools

```python
trace_with_snapshot(
    db_path: str,
    waveform_path: str,
    signal: str,
    time: int,
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    sample_offsets: Optional[List[int]] = None,
    clock_path: Optional[str] = None,
    cycle_offsets: Optional[List[int]] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
)

explain_signal_at_time(
    db_path: str,
    waveform_path: str,
    signal: str,
    time: int,
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
)

rank_cone_by_time(
    db_path: str,
    waveform_path: str,
    signal: str,
    time: int,
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    window_start: Optional[int] = None,
    window_end: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
)

explain_edge_cause(
    db_path: str,
    waveform_path: str,
    signal: str,
    time: int,
    edge_type: EdgeType | EdgeTypeAlias = "anyedge",
    direction: Direction = "backward",
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
)
```

### Notes

- `radix` for waveform value queries supports `hex`, `bin`, and `dec`.
- `EdgeTypeAlias` accepts forms such as `rise`, `rising`, `fall`, `falling`, `edge`, and `any`.
- `Direction` is `forward` or `backward`.
- `TraceMode` is `drivers` or `loads`.

## Standalone Waveform MCP

Source: [`waveform_explorer/waveform_mcp.py`](/home/qsun/AI_PROJ/auto_waveform_debugger/waveform_explorer/waveform_mcp.py)

```python
list_signals(vcd_path: str)
get_signal_info(vcd_path: str, path: str)
get_snapshot(vcd_path: str, signals: List[str], time: int, radix: str = "hex")
get_value_at_time(vcd_path: str, path: str, time: int, radix: str = "hex")
find_edge(vcd_path: str, path: str, edge_type: str, start_time: int, direction: str = "forward")
find_value_intervals(vcd_path: str, path: str, value: str, start_time: int, end_time: int, radix: str = "hex")
find_condition(vcd_path: str, expression: str, start_time: int, direction: str = "forward")
get_transitions(vcd_path: str, path: str, start_time: int, end_time: int, max_limit: int = 50)
analyze_pattern(vcd_path: str, path: str, start_time: int, end_time: int)
```

## Quick Examples

```python
get_snapshot(vcd_path="wave.fsdb", signals=["top.a", "top.b"], time=100, radix="hex")
get_value_at_time(vcd_path="wave.fsdb", path="top.addr", time=100, radix="dec")
find_value_intervals(vcd_path="wave.fsdb", path="top.rid", value="d8", start_time=307050000, end_time=327970000, radix="dec")
find_edge(vcd_path="wave.fsdb", path="top.clk", edge_type="rising", start_time=100, direction="backward")
trace_with_snapshot(db_path="rtl_trace.db", waveform_path="wave.fsdb", signal="top.foo", time=100)
```
