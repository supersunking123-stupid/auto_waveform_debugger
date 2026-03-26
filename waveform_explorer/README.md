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
List all hierarchical signal paths found in the waveform file.
- **Query:** `{"cmd": "list_signals"}`
- **Response:** `{"status": "success", "data": ["TOP.clk", "TOP.dut.rst_n", ...]}`

#### 1b. `list_signals_page` (FSDB only)
List hierarchical signal paths in a paged, prefix-filtered way for large FSDB waveforms.
- **Query:** `{"cmd": "list_signals_page", "args": {"prefix": "top.mem0_", "cursor": "", "limit": 200}}`
- **Response:** includes `data`, `has_more`, and `next_cursor`.
- `list_signals` remains unchanged for all formats.
- `list_signals_page` is intended for large FSDB designs to avoid massive one-shot JSON payloads.

#### 2. `get_snapshot`
Get the values of multiple signals at a specific timestamp.
- **Query:** `{"cmd": "get_snapshot", "args": {"signals": ["TOP.clk", "TOP.count"], "time": 5000}}`
- **Response:** `{"status": "success", "data": {"TOP.clk": "rising", "TOP.count": "b00001010"}}`
- Scalar responses are simplified to `0`, `1`, `x`, `z`, `rising`, or `falling`.
- Bus responses return the current bus value, or `changing` if the bus transitions exactly at that timestamp.

#### 2b. `get_value_at_time`
Get one signal value at a specific timestamp.
- **Query:** `{"cmd": "get_value_at_time", "args": {"path": "TOP.count", "time": 5000}}`
- **Response:** `{"status": "success", "data": "b00001010"}`
- The returned value uses the same simplified formatting as `get_snapshot`.

#### 3. `get_transitions`
Get a compressed history of signal flips within a time window.
- **Query:** `{"cmd": "get_transitions", "args": {"path": "TOP.clk", "start_time": 0, "end_time": 20000, "max_limit": 10}}`
- **Response:** Includes timestamps and values, with `glitch: true` if multiple changes occur at the same timestamp.

#### 4. `find_edge`
Search for the next/previous transition edge.
- **Query:** `{"cmd": "find_edge", "args": {"path": "TOP.clk", "edge_type": "posedge", "start_time": 1000}}`
- **Types:** `posedge`, `negedge`, `anyedge`.

#### 5. `analyze_pattern`
AI-powered heuristic analysis of a signal's behavior.
- **Query:** `{"cmd": "analyze_pattern", "args": {"path": "TOP.clk", "start_time": 0, "end_time": 100000}}`
- **Output:** Categorizes signals as "Clock-like", "Static", or "Dynamic" with summarized stats.

#### 6. `find_condition`
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
uv pip install fastmcp
```
### Run MCP Server
```bash
python waveform_mcp.py
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
- `list_signals(vcd_path)`
...

- `get_signal_info(vcd_path, path)`
- `get_snapshot(vcd_path, signals, time)`
- `find_edge(vcd_path, path, edge_type, start_time, direction)`
- `find_condition(vcd_path, expression, start_time, direction)`
- `get_transitions(vcd_path, path, start_time, end_time, max_limit)`
- `analyze_pattern(vcd_path, path, start_time, end_time)`
