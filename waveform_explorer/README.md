# Waveform Explorer (AI-Centric Debugger)

`wave_agent_cli` is a high-performance, C++ based waveform analysis engine designed specifically for AI Agents. It acts as a **Temporal Database Query Engine**, providing structured, high-signal-to-noise ratio data from VCD/FST/FSDB files, avoiding the token overhead of raw waveform logs.

## Core Philosophies
- **AI-Native:** Returns structured JSON, not visual waves.
- **On-Demand:** Pull only the specific time snapshots or signal transitions needed.
- **Pattern Awareness:** Built-in heuristics to identify clocks, static signals, and glitches.

## Installation & Build

### Prerequisites
- CMake 3.14+
- C++17 Compiler (GCC 8+ or Clang)

### Build Steps
```bash
cd waveform_explorer
mkdir build && cd build
cmake ..
make -j$(nproc)
```
The executable `wave_agent_cli` will be generated in the `build` directory.

## Usage Manual

### Command Format
```bash
./wave_agent_cli <waveform_file> '<json_query>'
```

Supported file types are `.vcd`, `.fst`, and `.fsdb`.
Hierarchical paths are normalized to remove a leading `TOP.` for consistency.
Queries accept both styles (`tb_top.sig` and `TOP.tb_top.sig`).

For FSDB, direct queries can also use a bare hierarchical base name for a packed vector signal.
If the dump contains a unique packed path such as `top.u_cq.cq_rd_count9[8:0]`, then querying
`top.u_cq.cq_rd_count9` will resolve to that packed signal automatically.
This fallback is only used for bracket-free queries and only when the packed match is unique.

### FSDB Build Requirement
FSDB support requires Synopsys Verdi FsdbReader SDK (`ffrAPI.h`, `libnffr.so`, `libnsys.so`).
By default, `CMakeLists.txt` looks under:
`/home/qsun/synopsys_apps/verdi/V-2023.12-SP2`.

You can override this path:
```bash
cmake -B build -DVERDI_HOME=/path/to/verdi -DENABLE_FSDB=ON .
```

### FSDB Runtime Output
Some Verdi FSDB builds print informational banners (for example `FSDB Reader...` or `logDir = ...`).
`wave_agent_cli` filters those lines so stdout remains JSON-only for MCP/client parsing.

### FSDB Packed-Signal Resolution
Some FSDB dumps store internal vectors only as packed signal names, for example:
- `top.nvdla_top...u_cq.cq_rd_count9[8:0]`
- `top.nvdla_top...u_cq.cq_rd_take_thread_id[3:0]`

To keep structural and waveform queries aligned, `wave_agent_cli` now falls back from a bare path
to the uniquely matching packed-vector signal when possible.

### Supported Commands

#### 1. `list_signals`
List waveform signal paths with optional hierarchy and type filtering.
- **Query:** `{"cmd": "list_signals"}`
- **Default behavior:** returns only signals declared directly in the top module.
- **Full namespace:** pass `{"cmd": "list_signals", "args": {"pattern": "*"}}`
- **Hierarchy wildcard:** pass `{"cmd": "list_signals", "args": {"pattern": "top.nvdla_top.nvdla_core2cvsram_ar_*"}}`
- **Type filter:** pass `{"cmd": "list_signals", "args": {"pattern": "*", "types": ["input", "output", "net"]}}`
- **Response:** includes `data`, `pattern`, `types`, and `top_module_only`.
- `types` may include any combination of `input`, `output`, `inout`, `net`, and `register`.
- For advanced matching, `pattern` also accepts `regex:<expr>`.

#### 1b. `list_signals_page` (FSDB only)
List hierarchical signal paths in a paged, prefix-filtered way for large FSDB waveforms.
- **Query:** `{"cmd": "list_signals_page", "args": {"prefix": "top.mem0_", "cursor": "", "limit": 200}}`
- **Response:** includes `data`, `has_more`, and `next_cursor`.
- `list_signals` now defaults to top-module-only output.
- `list_signals_page` is intended for large FSDB designs to avoid massive one-shot JSON payloads.

#### 2. `get_snapshot`
Get the values of multiple signals at a specific timestamp.
- **Query:** `{"cmd": "get_snapshot", "args": {"signals": ["TOP.clk", "TOP.count"], "time": 5000, "radix": "hex"}}`
- **Response:** `{"status": "success", "data": {"TOP.clk": "rising", "TOP.count": "h0a"}}`
- Scalar responses are simplified to `0`, `1`, `x`, `z`, `rising`, or `falling`.
- Stable multi-bit responses default to hex radix, for example `h0a`.
- Bus responses return `changing` if the bus transitions exactly at that timestamp.
- `radix` is optional and accepts `hex`, `bin`, or `dec`.

#### 2b. `get_value_at_time`
Get one signal value at a specific timestamp.
- **Query:** `{"cmd": "get_value_at_time", "args": {"path": "TOP.count", "time": 5000, "radix": "hex"}}`
- **Response:** `{"status": "success", "data": "h0a"}`
- The returned value uses the same simplified formatting as `get_snapshot`.

#### 3. `get_transitions`
Get a compressed history of signal flips within a time window.
- **Query:** `{"cmd": "get_transitions", "args": {"path": "TOP.clk", "start_time": 0, "end_time": 20000, "max_limit": 10}}`
- **Response:** Includes timestamps and values, with `glitch: true` if multiple changes occur at the same timestamp.

#### 3a. `count_transitions`
Count edges or toggles within a time window.
- **Query:** `{"cmd": "count_transitions", "args": {"path": "TOP.clk", "start_time": 0, "end_time": 20000, "edge_type": "posedge"}}`
- **Scalar behavior:** `posedge`, `negedge`, and `anyedge` are supported.
- **Multi-bit behavior:** counts toggles regardless of the requested edge filter.

#### 3c. `dump_waveform_data`
Write large waveform windows directly to a local JSONL file.
- **Query:** `{"cmd": "dump_waveform_data", "args": {"signals": ["TOP.clk", "TOP.count"], "start_time": 0, "end_time": 20000, "output_path": "/tmp/wave.jsonl", "mode": "transitions"}}`
- **Modes:** `transitions` or `samples`
- **Sample mode:** requires `sample_period`
- **Safety:** existing files are preserved unless `overwrite` is set to `true`

#### 3b. `get_signal_overview`
Get a resolution-aware overview of one signal over a time window.
- **Query:** `{"cmd": "get_signal_overview", "args": {"path": "TOP.bus[7:0]", "start_time": 0, "end_time": 20000, "resolution": "auto", "radix": "hex"}}`
- **Response:** returns `resolution`, `timescale`, `signal`, `width`, and ordered `segments`.
- Single-bit stable segments use direct logic states such as `0`, `1`, `x`, `z`.
- Single-bit dense activity uses `{"state": "flipping", "transitions": N}`.
- Multi-bit stable segments use `{"state": "stable", "value": "h3f"}`.
- Multi-bit dense activity uses `{"state": "flipping", "unique_values": N, "transitions": N}`.
- `resolution` may be an integer or `"auto"`. Auto chooses a coarse enough resolution to keep the overview navigable.

#### 4. `find_edge`
Search for the next/previous transition edge.
- **Query:** `{"cmd": "find_edge", "args": {"path": "TOP.clk", "edge_type": "posedge", "start_time": 1000}}`
- **Types:** `posedge`, `negedge`, `anyedge`.

#### 5. `find_value_intervals`
Find all matching value intervals for one signal within a bounded time range.
- **Query:** `{"cmd": "find_value_intervals", "args": {"path": "TOP.rid", "value": "d8", "start_time": 307050000, "end_time": 327970000, "radix": "dec"}}`
- **Response:** `{"status": "success", "data": [{"start": 307050000, "end": 307059999}, ...]}`
- `value` may be prefixed, for example `d8`, `h08`, or `b00001000`.
- If `value` has no prefix, `radix` controls how it is interpreted.

#### 6. `analyze_pattern`
AI-powered heuristic analysis of a signal's behavior.
- **Query:** `{"cmd": "analyze_pattern", "args": {"path": "TOP.clk", "start_time": 0, "end_time": 100000}}`
- **Output:** Categorizes signals as "Clock-like", "Static", or "Dynamic" with summarized stats.

#### 7. `find_condition`
Find the first timestamp where a logical condition is met.
- **Query:** `{"cmd": "find_condition", "args": {"expression": "TOP.count == b00000001", "start_time": 0}}`

## Developer Context
- **Binary Search:** Signal state retrieval uses `std::upper_bound` for $O(\log N)$ performance on large datasets.
- **Memory Efficient:** VCD is parsed into optimized transition arrays per signal.
- **Third-party:** Uses `nlohmann/json` for robust JSON handling.

## MCP Service
The tool can be run as an MCP (Model Context Protocol) service, allowing AI Agents to interact with it directly via tools.

### Setup MCP
Ensure the virtual environment is set up and `fastmcp` is installed:
```bash
cd /home/qsun/AI_PROJ/auto_waveform_debugger
python3 -m venv .venv
.venv/bin/python3 -m pip install --upgrade pip
.venv/bin/python3 -m pip install -r requirements.txt
```
For this repo, use the shared root virtualenv. `requirements.txt` includes
`fastmcp` and the other Python packages used by the MCP wrappers and tests.
### Run MCP Server
```bash
.venv/bin/python3 waveform_mcp.py
```

## Integration with AI Agents (MCP)

To use this tool with MCP-compatible clients like `gemini-cli` or `Claude Desktop`, add the following configuration to your client's settings file.

### 1. Configuration for `gemini-cli`
Add this to your `mcpServers` section in `~/.gemini/config.json` (or your specific config path):

```json
{
  "mcpServers": {
    "waveform-explorer": {
      "command": "/home/qsun/AI_PROJ/auto_waveform_debugger/.venv/bin/python",
      "args": [
        "/home/qsun/AI_PROJ/auto_waveform_debugger/waveform_explorer/waveform_mcp.py"
      ]
    }
  }
}
```

### 2. Configuration for Claude Desktop
Edit your `claude_desktop_config.json` to include:

```json
{
  "mcpServers": {
    "waveform_explorer": {
      "command": "/home/qsun/AI_PROJ/auto_waveform_debugger/.venv/bin/python",
      "args": [
        "/home/qsun/AI_PROJ/auto_waveform_debugger/waveform_explorer/waveform_mcp.py"
      ]
    }
  }
}
```

### Verification
Once configured, restart your agent and try asking:
> "List all signals in /home/qsun/AI_PROJ/auto_waveform_debugger/waveform_explorer/timer_tb.vcd"

Regression-style examples for previously failing FSDB queries are recorded in
`/home/qsun/AI_PROJ/auto_waveform_debugger/failure_history.md`.

### Exposed Tools
- `list_signals(vcd_path, pattern="", types=None)`
...

- `get_signal_info(vcd_path, path)`
- `get_snapshot(vcd_path, signals, time)`
- `get_signal_overview(vcd_path, path, start_time, end_time, resolution, radix="hex")`
- `find_edge(vcd_path, path, edge_type, start_time, direction)`
- `find_value_intervals(vcd_path, path, value, start_time, end_time, radix="hex")`
- `find_condition(vcd_path, expression, start_time, direction)`
- `get_transitions(vcd_path, path, start_time, end_time, max_limit)`
- `analyze_pattern(vcd_path, path, start_time, end_time)`
