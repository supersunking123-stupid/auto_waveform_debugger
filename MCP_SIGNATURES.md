# MCP Signatures

This file lists the MCP tool signatures exposed by the local servers in this repo as of March 28, 2026.

## Main Agent MCP

Source: [`agent_debug_automation/agent_debug_automation_mcp.py`](/home/qsun/AI_PROJ/auto_waveform_debugger/agent_debug_automation/agent_debug_automation_mcp.py)

### Waveform view model

The main agent MCP exposes a session-aware waveform view model:

- `Session`
  - A saved waveform view bound to one waveform file.
  - Stores the current `Cursor`, named `Bookmarks`, and named `Signal Groups`.
- `Cursor`
  - The current focus time in a Session.
  - May be referenced as `time="Cursor"`.
- `Bookmark`
  - A named saved time in a Session.
  - May be referenced as `time="BM_<bookmark_name>"`.
- `Signal Group`
  - A named saved signal list in a Session.
  - May be expanded by `get_snapshot(..., signals_are_groups=True)`.

Default behavior:
- Each waveform gets a `Default_Session` on first session-aware use.
- If a session-aware waveform tool omits `vcd_path`, the active Session selects the waveform file.
- The active Session is global, but each saved Session is waveform-bound.

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
list_signals(vcd_path: Optional[str] = None, session_name: Optional[str] = None)
get_signal_info(vcd_path: Optional[str] = None, path: str = "", session_name: Optional[str] = None)
get_snapshot(vcd_path: Optional[str] = None, signals: Optional[List[str]] = None, time: TimeReference = 0, radix: str = "hex", signals_are_groups: bool = False, session_name: Optional[str] = None)
get_value_at_time(vcd_path: Optional[str] = None, path: str = "", time: TimeReference = 0, radix: str = "hex", session_name: Optional[str] = None)
find_edge(vcd_path: Optional[str] = None, path: str = "", edge_type: EdgeType | EdgeTypeAlias = "anyedge", start_time: TimeReference = 0, direction: Direction = "forward", session_name: Optional[str] = None)
find_value_intervals(vcd_path: Optional[str] = None, path: str = "", value: str = "", start_time: TimeReference = 0, end_time: TimeReference = 0, radix: str = "hex", session_name: Optional[str] = None)
find_condition(vcd_path: Optional[str] = None, expression: str = "", start_time: TimeReference = 0, direction: Direction = "forward", session_name: Optional[str] = None)
get_transitions(vcd_path: Optional[str] = None, path: str = "", start_time: TimeReference = 0, end_time: TimeReference = 0, max_limit: int = 50, session_name: Optional[str] = None)
get_signal_overview(vcd_path: Optional[str] = None, path: str = "", start_time: TimeReference = 0, end_time: TimeReference = 0, resolution: ResolutionReference = "auto", radix: str = "hex", session_name: Optional[str] = None)
analyze_pattern(vcd_path: Optional[str] = None, path: str = "", start_time: TimeReference = 0, end_time: TimeReference = 0, session_name: Optional[str] = None)
```

### Session tools

```python
create_session(waveform_path: str, session_name: str, description: str = "")
list_sessions(waveform_path: Optional[str] = None)
get_session(session_name: Optional[str] = None, waveform_path: Optional[str] = None)
switch_session(session_name: str, waveform_path: Optional[str] = None)
delete_session(session_name: str, waveform_path: Optional[str] = None)
set_cursor(time: TimeReference, waveform_path: Optional[str] = None, session_name: Optional[str] = None)
move_cursor(delta: int, waveform_path: Optional[str] = None, session_name: Optional[str] = None)
get_cursor(waveform_path: Optional[str] = None, session_name: Optional[str] = None)
create_bookmark(bookmark_name: str, time: TimeReference, description: str = "", waveform_path: Optional[str] = None, session_name: Optional[str] = None)
delete_bookmark(bookmark_name: str, waveform_path: Optional[str] = None, session_name: Optional[str] = None)
list_bookmarks(waveform_path: Optional[str] = None, session_name: Optional[str] = None)
create_signal_group(group_name: str, signals: List[str], description: str = "", waveform_path: Optional[str] = None, session_name: Optional[str] = None)
update_signal_group(group_name: str, signals: Optional[List[str]] = None, description: Optional[str] = None, waveform_path: Optional[str] = None, session_name: Optional[str] = None)
delete_signal_group(group_name: str, waveform_path: Optional[str] = None, session_name: Optional[str] = None)
list_signal_groups(waveform_path: Optional[str] = None, session_name: Optional[str] = None)
```

### Cross-linked structural and waveform analysis tools

```python
trace_with_snapshot(
    db_path: str,
    waveform_path: Optional[str] = None,
    signal: str = "",
    time: TimeReference = 0,
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    sample_offsets: Optional[List[int]] = None,
    clock_path: Optional[str] = None,
    cycle_offsets: Optional[List[int]] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
    session_name: Optional[str] = None,
)

explain_signal_at_time(
    db_path: str,
    waveform_path: Optional[str] = None,
    signal: str = "",
    time: TimeReference = 0,
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
    session_name: Optional[str] = None,
)

rank_cone_by_time(
    db_path: str,
    waveform_path: Optional[str] = None,
    signal: str = "",
    time: TimeReference = 0,
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    window_start: Optional[int] = None,
    window_end: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
    session_name: Optional[str] = None,
)

explain_edge_cause(
    db_path: str,
    waveform_path: Optional[str] = None,
    signal: str = "",
    time: TimeReference = 0,
    edge_type: EdgeType | EdgeTypeAlias = "anyedge",
    direction: Direction = "backward",
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
    session_name: Optional[str] = None,
)
```

### Notes

- `radix` for waveform value queries supports `hex`, `bin`, and `dec`.
- `TimeReference` accepts an integer time, `"Cursor"`, or `"BM_<bookmark_name>"`.
- `ResolutionReference` accepts an integer or `"auto"`.
- `EdgeTypeAlias` accepts forms such as `rise`, `rising`, `fall`, `falling`, `edge`, and `any`.
- `Direction` is `forward` or `backward`.
- `TraceMode` is `drivers` or `loads`.
- `get_snapshot(...)` can expand saved signal groups when `signals_are_groups=True`.

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
get_signal_overview(vcd_path: str, path: str, start_time: int, end_time: int, resolution: Union[int, str], radix: str = "hex")
analyze_pattern(vcd_path: str, path: str, start_time: int, end_time: int)
```

## Quick Examples

```python
get_snapshot(vcd_path="wave.fsdb", signals=["top.a", "top.b"], time=100, radix="hex")
get_value_at_time(vcd_path="wave.fsdb", path="top.addr", time=100, radix="dec")
find_value_intervals(vcd_path="wave.fsdb", path="top.rid", value="d8", start_time=307050000, end_time=327970000, radix="dec")
find_edge(vcd_path="wave.fsdb", path="top.clk", edge_type="rising", start_time=100, direction="backward")
get_signal_overview(vcd_path="wave.fsdb", path="top.bus[7:0]", start_time=0, end_time=100000, resolution="auto", radix="hex")
trace_with_snapshot(db_path="rtl_trace.db", waveform_path="wave.fsdb", signal="top.foo", time="Cursor")
```
