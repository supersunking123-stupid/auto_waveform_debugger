# agent_debug_automation

Merged MCP service that exposes:
- `standalone_trace` as `rtl_trace`
- `waveform_explorer` as `wave_agent_cli`
- cross-link tools that combine structural trace data with waveform evidence

The low-level command surfaces stay intact. The added value is the orchestration layer on top.

## Build prerequisites

Build both native binaries first:

```bash
# 1) standalone_trace
cd /home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace
cmake -B build -GNinja .
ninja -C build

# 2) waveform_explorer
cd /home/qsun/AI_PROJ/auto_waveform_debugger/waveform_explorer
cmake -B build .
cmake --build build -j"$(nproc)"
```

Install the Python MCP runtime:

```bash
uv pip install fastmcp
```

## Run MCP service

```bash
cd /home/qsun/AI_PROJ/auto_waveform_debugger/agent_debug_automation
python agent_debug_automation_mcp.py
```

## MCP client config

```json
{
  "mcpServers": {
    "agent_debug_automation": {
      "command": "/home/qsun/AI_PROJ/auto_waveform_debugger/.venv/bin/python",
      "args": [
        "/home/qsun/AI_PROJ/auto_waveform_debugger/agent_debug_automation/agent_debug_automation_mcp.py"
      ]
    }
  }
}
```

If you are not using the repo virtualenv, replace `command` with your system `python3`.

## Waveform View Model

The session-aware tools expose a simple waveform-view model:

- `Session`
  - A saved waveform view bound to one waveform file.
  - A Session stores the current `Cursor`, named `Bookmarks`, and named `Signal Groups`.
  - Each waveform auto-creates a `Default_Session` on first session-aware use.
- `Cursor`
  - The current focus time in a Session.
  - Query tools may use `time="Cursor"` instead of an integer timestamp.
- `Bookmark`
  - A named saved time in a Session.
  - Query tools may use `time="BM_<bookmark_name>"`.
- `Signal Group`
  - A named saved signal list in a Session.
  - `get_snapshot(...)` expands group names when `signals_are_groups=True`.

Default resolution rules:

- If `vcd_path` is omitted on a session-aware waveform tool, the active Session selects the waveform file.
- The active Session is global, but each saved Session is bound to a single waveform path.
- Session state persists on disk under `agent_debug_automation/.session_store`.

## Exposed tools

### `standalone_trace` passthrough

- `rtl_trace(args, rtl_trace_bin=None, timeout_sec=300)`
  - Pass the original `rtl_trace` subcommand and options as a token list.
  - Examples:
    - `["compile","--db","rtl_trace.db","--top","top","-f","files.f"]`
    - `["trace","--db","rtl_trace.db","--mode","drivers","--signal","top.u0.sig"]`
    - `["find","--db","rtl_trace.db","--query","foo.bar"]`
    - `["hier","--db","rtl_trace.db","--root","top.u0"]`
- `rtl_trace_serve_start(serve_args=None, rtl_trace_bin=None)`
- `rtl_trace_serve_query(session_id, command_line)`
- `rtl_trace_serve_stop(session_id)`

These keep the original `rtl_trace` command model unchanged.

### `waveform_explorer` passthrough

- `wave_agent_query(vcd_path, cmd, args=None, wave_cli_bin=None)`
- `list_signals(vcd_path)`
- `get_signal_info(vcd_path, path)`
- `get_snapshot(vcd_path=None, signals, time, radix="hex", signals_are_groups=False, session_name=None)`
- `get_value_at_time(vcd_path=None, path, time, radix="hex", session_name=None)`
- `find_edge(vcd_path=None, path, edge_type, start_time, direction="forward", session_name=None)`
- `find_value_intervals(vcd_path=None, path, value, start_time, end_time, radix="hex", session_name=None)`
- `find_condition(vcd_path=None, expression, start_time, direction="forward", session_name=None)`
- `get_transitions(vcd_path=None, path, start_time, end_time, max_limit=50, session_name=None)`
- `get_signal_overview(vcd_path=None, path, start_time, end_time, resolution="auto", radix="hex", session_name=None)`
- `analyze_pattern(vcd_path=None, path, start_time, end_time, session_name=None)`

Waveform semantics stay aligned with `wave_agent_cli`. In particular, backward edge search now resolves the last matching edge at or before `T`, including an edge exactly at `T`.
For multi-bit value queries, `radix` may be `hex`, `bin`, or `dec`; the default is `hex`.
`get_signal_overview` provides a zoomed-out, resolution-aware summary of one signal and also accepts `resolution="auto"` for an overview capped to a manageable number of segments.

### Session tools

- `create_session(waveform_path, session_name, description="")`
- `list_sessions(waveform_path=None)`
- `get_session(session_name=None, waveform_path=None)`
- `switch_session(session_name, waveform_path=None)`
- `delete_session(session_name, waveform_path=None)`
- `set_cursor(time, waveform_path=None, session_name=None)`
- `move_cursor(delta, waveform_path=None, session_name=None)`
- `get_cursor(waveform_path=None, session_name=None)`
- `create_bookmark(bookmark_name, time, description="", waveform_path=None, session_name=None)`
- `delete_bookmark(bookmark_name, waveform_path=None, session_name=None)`
- `list_bookmarks(waveform_path=None, session_name=None)`
- `create_signal_group(group_name, signals, description="", waveform_path=None, session_name=None)`
- `update_signal_group(group_name, signals=None, description=None, waveform_path=None, session_name=None)`
- `delete_signal_group(group_name, waveform_path=None, session_name=None)`
- `list_signal_groups(waveform_path=None, session_name=None)`

Session state is stored on disk under `agent_debug_automation/.session_store`.
Each waveform gets a default `Default_Session` on first session-aware use.
Time-taking merged tools accept either an integer time, `"Cursor"`, or `"BM_<bookmark_name>"`.
`get_snapshot` can expand saved Signal Groups when `signals_are_groups=true`.
The active Session is global, but every saved Session is bound to a single waveform path.

Common session-aware workflow:

```python
create_session("/path/to/wave.fsdb", "debug_view", "AXI read debug")
switch_session("debug_view", waveform_path="/path/to/wave.fsdb")
set_cursor(307050000, waveform_path="/path/to/wave.fsdb")
create_bookmark("rlast_edge", "Cursor", waveform_path="/path/to/wave.fsdb")
create_signal_group(
  "axi_read_rsp",
  ["top...rlast", "top...rready", "top...rid[7:0]"],
  waveform_path="/path/to/wave.fsdb",
)
get_snapshot(
  vcd_path="/path/to/wave.fsdb",
  signals=["axi_read_rsp"],
  time="BM_rlast_edge",
  signals_are_groups=True,
)
get_signal_overview(
  vcd_path="/path/to/wave.fsdb",
  path="top...rid[7:0]",
  start_time="Cursor",
  end_time="BM_rlast_edge",
  resolution="auto",
)
```

### Cross-link tools

- `trace_with_snapshot(db_path, waveform_path, signal, time, ...)`
  - Runs structural trace.
  - Collects cone endpoint signals from the trace JSON.
  - Returns sampled waveform values for the cone at `time`.
  - Supports additional absolute snapshots with `sample_offsets`.
  - Supports cycle-relative snapshots with `clock_path` and `cycle_offsets`.

- `explain_signal_at_time(db_path, waveform_path, signal, time, ...)`
  - Traces the structural cone first.
  - Samples the endpoint and RHS terms at `time`.
  - Returns ranked candidate logic paths with both structural and waveform evidence.

- `rank_cone_by_time(db_path, waveform_path, signal, time, ...)`
  - Builds the structural cone.
  - Scores cone signals by local transition timing and stuck behavior around `time`.
  - Returns full ranking plus focused views for active and stuck candidates.

- `explain_edge_cause(db_path, waveform_path, signal, time, edge_type="anyedge", ...)`
  - Finds the relevant edge near `time`.
  - Re-runs the signal-at-time explanation at the resolved edge.
  - Returns edge context plus ranked upstream candidates.

Cross-link tools also accept session time aliases, so `time` may be an integer, `"Cursor"`, or `"BM_<bookmark_name>"`.

## Cross-link behavior

### Path mapping

- Structural-to-waveform path normalization only auto-handles a leading `TOP.`.
- For FSDB, the mapper first tries exact `get_signal_info` lookups.
- If exact lookup fails, FSDB falls back to prefix-scoped `list_signals_page` queries instead of enumerating the full waveform namespace.
- VCD and FST keep the original `list_signals` behavior.

### Time windows

- `trace_with_snapshot` and `explain_signal_at_time` default to `[T-1000, T]`.
- `rank_cone_by_time` defaults to `[T-1000, T+1000]`.
- Cycle-relative sampling uses `posedge` on the provided `clock_path`.

### Ranking

The ranking is heuristic and waveform-driven. It does not symbolically solve RTL expressions.

Current priorities:
- `closeness_score` is the strongest term
  - `drivers` prefers transitions at or just before `T`
  - `loads` prefers transitions at or just after `T`
- `most_active_near_time` is ordered after closest relevant flip activity
- stuck signals are separated into:
  - `stuck_to_1`
  - `stuck_to_0`
  - `stuck_other`
- `stuck_to_1` is scored higher than `stuck_to_0`

Returned per-signal summaries include:
- `closest_transition_time`
- `closest_transition_distance`
- `closest_transition_direction`
- `preferred_transition_side`
- `closeness_score`
- `recent_toggle_count`
- `activity_score`
- `stuck_class`
- `stuck_score`
- `total_score`

## FSDB notes

- FSDB is handled differently from VCD and FST.
- On load, the FSDB backend builds metadata and keeps the reader open.
- Per-signal transitions are fetched lazily when queries need them.
- `list_signals_page` exists specifically for large FSDB designs.

This is what makes the cross-link tools practical on large ASIC waveforms such as NVDLA.

## Tests

Portable regression:

```bash
python3 -m unittest waveform_explorer.tests.test_signal_overview
python3 -m unittest agent_debug_automation.tests.test_cross_linking
```

The test module includes:
- unit-style regression on the bundled `timer_tb.vcd` fixture
- a real-environment NVDLA integration regression, auto-enabled only when these paths exist:
  - `/home/qsun/DVT/nvdla/hw/verif/sim_vip/cc_alexnet_conv5_relu5_int16_dtest_cvsram/wave.fsdb`
  - `/home/qsun/DVT/nvdla/hw/verif/sim_vip/rtl_trace.db`

Run only the real-environment regression:

```bash
python3 -m unittest agent_debug_automation.tests.test_cross_linking.NvdlaSessionIntegrationTests
```

Because session state is persisted under `.session_store`, these tests should be run serially rather than in parallel.
