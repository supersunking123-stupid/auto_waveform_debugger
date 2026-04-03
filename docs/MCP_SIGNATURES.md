# MCP Signatures

This file lists the MCP tool signatures exposed by the local servers in this repo as of April 3, 2026.

## Main Agent MCP

Source: [`agent_debug_automation/agent_debug_automation_mcp.py`](/home/qsun/AI_PROJ/auto_waveform_debugger/agent_debug_automation/agent_debug_automation_mcp.py)

### Waveform view model

The main agent MCP exposes a session-aware waveform view model:

- `Session`
  - A saved waveform view bound to one waveform file.
  - Stores the current `Cursor`, named `Bookmarks`, named `Signal Groups`, and named `Created Signals`.
- `Cursor`
  - The current focus time in a Session.
  - May be referenced as `time="Cursor"`.
- `Bookmark`
  - A named saved time in a Session.
  - May be referenced as `time="BM_<bookmark_name>"`.
- `Signal Group`
  - A named saved signal list in a Session.
  - May be expanded by `get_snapshot(..., signals_are_groups=True)`.
- `Created Signal`
  - A named virtual signal defined by a Verilog-like expression over real signals or other created signals.
  - Stored in the session as `created_signals[signal_name] = {expression, ast, dependencies, description}`.
  - Transparently supported by `get_transitions`, `get_value_at_time`, `find_edge`, and other session-aware waveform tools.
  - Supports chained dependencies (e.g., `v2 = v1 | c` where `v1` is also a created signal).
  - Maximum dependency depth: 16 levels. Circular dependencies are rejected at creation time.

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
rtl_trace(args=["hier", "--db", "rtl_trace.db", "--root", "top.u0", "--depth", "0", "--format", "json", "--show-source"])
rtl_trace(args=["whereis-instance", "--db", "rtl_trace.db", "--instance", "top.u0", "--format", "json"])
```

Supported subcommands:

```text
compile
trace
find
hier
whereis-instance
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
rtl_trace hier --db <db_path> [--root <hier.path>] [--depth N] [--max-nodes N] [--format text|json] [--show-source]
```

Common flags:
- `--db <db_path>`
- `--root <hier.path>`
- `--depth <N>`
- `--max-nodes <N>`
- `--format text|json`
- `--show-source`

`--show-source` is optional and off by default. When present, `hier` also emits the instantiated module definition file and line when that source metadata is available in the DB.

#### `whereis_instance` / `whereis-instance`

Quick lookup for one hierarchy node.

CLI form:

```text
rtl_trace whereis-instance --db <db_path> --instance <hier.path> [--format text|json]
```

Common flags:
- `--db <db_path>`
- `--instance <hier.path>`
- `--format text|json`

Behavior:
- returns the instance path
- returns the instantiated module type
- returns the module definition source file and line when available in the DB

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
rtl_trace_serve_query(session_id="<sid>", command_line="whereis-instance --instance top.u0 --format json")
rtl_trace_serve_stop(session_id="<sid>")
```

### Waveform passthrough tools

```python
wave_agent_query(vcd_path: str, cmd: str, args: Optional[Dict[str, Any]] = None, wave_cli_bin: Optional[str] = None)
list_signals(vcd_path: Optional[str] = None, pattern: str = "", types: Optional[List[str]] = None, session_name: Optional[str] = None)
get_signal_info(vcd_path: Optional[str] = None, path: str = "", session_name: Optional[str] = None)
get_snapshot(vcd_path: Optional[str] = None, signals: Optional[List[str]] = None, time: TimeReference = 0, radix: str = "hex", signals_are_groups: bool = False, session_name: Optional[str] = None)
get_value_at_time(vcd_path: Optional[str] = None, path: str = "", time: TimeReference = 0, radix: str = "hex", session_name: Optional[str] = None)
find_edge(vcd_path: Optional[str] = None, path: str = "", edge_type: EdgeType | EdgeTypeAlias = "anyedge", start_time: TimeReference = 0, direction: Direction = "forward", session_name: Optional[str] = None)
find_value_intervals(vcd_path: Optional[str] = None, path: str = "", value: str = "", start_time: TimeReference = 0, end_time: TimeReference = 0, radix: str = "hex", session_name: Optional[str] = None)
find_condition(vcd_path: Optional[str] = None, expression: str = "", start_time: TimeReference = 0, direction: Direction = "forward", session_name: Optional[str] = None)
get_transitions(vcd_path: Optional[str] = None, path: str = "", start_time: TimeReference = 0, end_time: TimeReference = 0, max_limit: int = 50, session_name: Optional[str] = None)
count_transitions(vcd_path: Optional[str] = None, path: str = "", start_time: TimeReference = 0, end_time: TimeReference = 0, edge_type: EdgeType | EdgeTypeAlias = "anyedge", session_name: Optional[str] = None)
  Counts edges or toggles for one signal in a time window.
  For multi-bit signals, `edge_type` must be `"anyedge"` and the tool counts value toggles.
  For single-bit signals, `edge_type` can be `"posedge"`, `"negedge"`, or `"anyedge"`.

  **Glitch handling:** When multiple transitions occur at the same timestamp (glitches), each transition is evaluated against the immediately preceding transition record in the sequence. For example, if a signal has transitions `0→1` and `1→0` both at timestamp 100, both are counted as edges. This means glitch bursts are fully enumerated—each individual transition record contributes to the count based on its actual value change.

dump_waveform_data(vcd_path: Optional[str] = None, signals: Optional[List[str]] = None, start_time: TimeReference = 0, end_time: TimeReference = 0, output_path: str = "", mode: str = "transitions", sample_period: Optional[int] = None, radix: str = "hex", overwrite: bool = False, signals_are_groups: bool = False, session_name: Optional[str] = None)
get_signal_overview(vcd_path: Optional[str] = None, path: str = "", start_time: TimeReference = 0, end_time: TimeReference = 0, resolution: ResolutionReference = "auto", radix: str = "hex", session_name: Optional[str] = None)
analyze_pattern(vcd_path: Optional[str] = None, path: str = "", start_time: TimeReference = 0, end_time: TimeReference = 0, session_name: Optional[str] = None)
```

`list_signals(...)` options:
- `pattern`
  - Default `""`: list only signals declared directly in the top module.
  - `"*"`: list the full waveform namespace.
  - Hierarchical wildcard such as `"top.nvdla_top.nvdla_core2cvsram_ar_*"`: narrow by path prefix/pattern.
  - `"regex:<expr>"`: use a raw regular expression instead of glob-style matching.
- `types`
  - Optional list of signal categories used as an OR filter.
  - Supported values: `input`, `output`, `inout`, `net`, `register`.
  - Example: `types=["input", "net"]` returns both input ports and net-style signals.

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

### Virtual signal expression tools

Create, update, delete, and list virtual signals defined by Verilog-like expressions. Virtual signals are computed on demand from real waveform signals (or other virtual signals) and behave like real signals in all session-aware waveform tools.

```python
create_signal_expression(
    signal_name: str,
    expression: str,
    description: str = "",
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
)
update_signal_expression(
    signal_name: str,
    expression: Optional[str] = None,
    description: Optional[str] = None,
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
)
delete_signal_expression(
    signal_name: str,
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
)
list_signal_expressions(
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
)
```

#### Supported operators

| Category | Operators | Notes |
|----------|-----------|-------|
| Unary bitwise | `~` `!` | `!` treats non-zero as true |
| Unary reduction | `&` `\|` `^` `~&` `~\|` `~^` `^~` | Standard Verilog reduction operators |
| Bitwise binary | `&` `\|` `^` | Standard Verilog |
| Custom binary | `~&` `~\|` `~^` | **Custom extension**: `a ~& b` = `~(a & b)` |
| Logical | `&&` `\|\|` | Vector truthiness: non-zero = true |
| Equality | `==` `!=` `===` `!==` | `===`/`!==` are 4-state identity (x === x is true) |
| Relational | `<` `>` `<=` `>=` | Returns 1-bit |
| Shift | `<<` `>>` `<<<` `>>>` | `>>>` uses signedness of left operand |
| Arithmetic | `+` `-` `*` `/` `%` `**` | Division by zero returns x |
| Ternary | `? :` | X-merge per IEEE 1364 when condition is x/z |
| Assignment in ternary | `=` | **Custom extension**: `cond ? a = b : c` treated as `cond ? b : c` |
| Literals | `0` `1` `4'b1010` `32'hFF` `8'd255` `'x` `'z` | Sized/unsized Verilog constants |

Signal paths follow standard hierarchy: `top.module.signal`. The `$` character is supported in identifiers for synthesized nets.

#### Interaction with existing tools

Virtual signals are transparently supported by these session-aware waveform tools:

- `get_transitions(path="virtual_signal_name", ...)` — returns computed transitions
- `get_value_at_time(path="virtual_signal_name", ...)` — evaluates expression at a single time point
- `find_edge(path="virtual_signal_name", ...)` — searches for edges in computed transitions
- `get_snapshot(signals=[..., "virtual_signal_name"], ...)` — mixed real/virtual snapshots
- `find_value_intervals`, `count_transitions`, `get_signal_overview`, `analyze_pattern` — all virtual-aware

Structural trace tools (`trace_with_snapshot`, `explain_signal_at_time`, `rank_cone_by_time`, `explain_edge_cause`) return an error if a virtual signal name is passed — they require a real signal path and a structural DB.

#### Performance notes

- Evaluation is **event-driven**: the virtual signal is only evaluated at times when an operand signal changes.
- Computed transitions are **cached** in memory (per session). Subsequent queries for the same virtual signal reuse the cache.
- Cache is automatically invalidated when the expression or a dependency is updated or deleted.
- For chained virtual signals (e.g., `v2 = v1 | c` where `v1` is also virtual), dependencies are resolved via topological sort.

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
list_signals(vcd_path: str, pattern: str = "", types: Optional[List[str]] = None)
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

`list_signals(...)` options are the same here:
- `pattern=""` means top-module-only output.
- `pattern="*"` means full-namespace output.
- wildcard forms such as `"top.u_axi.ar_*"` narrow by hierarchy/path.
- `pattern="regex:<expr>"` enables regex matching.
- `types` accepts any combination of `input`, `output`, `inout`, `net`, and `register`.

## Quick Examples

```python
list_signals(vcd_path="wave.fsdb")
list_signals(vcd_path="wave.fsdb", pattern="top.nvdla_top.nvdla_core2cvsram_ar_*", types=["net"])
list_signals(vcd_path="wave.fsdb", pattern="*", types=["input", "output"])
get_snapshot(vcd_path="wave.fsdb", signals=["top.a", "top.b"], time=100, radix="hex")
get_value_at_time(vcd_path="wave.fsdb", path="top.addr", time=100, radix="dec")
find_value_intervals(vcd_path="wave.fsdb", path="top.rid", value="d8", start_time=307050000, end_time=327970000, radix="dec")
find_edge(vcd_path="wave.fsdb", path="top.clk", edge_type="rising", start_time=100, direction="backward")
get_signal_overview(vcd_path="wave.fsdb", path="top.bus[7:0]", start_time=0, end_time=100000, resolution="auto", radix="hex")
trace_with_snapshot(db_path="rtl_trace.db", waveform_path="wave.fsdb", signal="top.foo", time="Cursor")

# Virtual signal expressions
create_signal_expression(signal_name="aw_sent", expression="awvalid & awready", description="AW channel handshake")
get_transitions(path="aw_sent", start_time=0, end_time=100000)
get_value_at_time(path="aw_sent", time="Cursor")
create_signal_expression(signal_name="bus_sum", expression="bus_a + bus_b")
find_edge(path="aw_sent", edge_type="posedge", start_time=0, direction="forward")
create_signal_expression(signal_name="chained", expression="aw_sent | ar_sent")  # depends on another virtual signal
list_signal_expressions()
delete_signal_expression(signal_name="bus_sum")
```
