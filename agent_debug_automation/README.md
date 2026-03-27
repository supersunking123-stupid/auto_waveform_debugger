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
- `get_snapshot(vcd_path, signals, time, radix="hex")`
- `get_value_at_time(vcd_path, path, time, radix="hex")`
- `find_edge(vcd_path, path, edge_type, start_time, direction="forward")`
- `find_value_intervals(vcd_path, path, value, start_time, end_time, radix="hex")`
- `find_condition(vcd_path, expression, start_time, direction="forward")`
- `get_transitions(vcd_path, path, start_time, end_time, max_limit=50)`
- `analyze_pattern(vcd_path, path, start_time, end_time)`

Waveform semantics stay aligned with `wave_agent_cli`. In particular, backward edge search now resolves the last matching edge at or before `T`, including an edge exactly at `T`.
For multi-bit value queries, `radix` may be `hex`, `bin`, or `dec`; the default is `hex`.

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
