# Waveform Explorer (AI-Centric Debugger)

`wave_agent_cli` is a high-performance, C++ based waveform analysis engine designed specifically for AI Agents. It acts as a **Temporal Database Query Engine**, providing structured, high-signal-to-noise ratio data from VCD files, avoiding the token overhead of raw waveform logs.

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
./wave_agent_cli <vcd_file> '<json_query>'
```

### Supported Commands

#### 1. `list_signals`
List all hierarchical signal paths found in the VCD.
- **Query:** `{"cmd": "list_signals"}`
- **Response:** `{"status": "success", "data": ["TOP.clk", "TOP.dut.rst_n", ...]}`

#### 2. `get_snapshot`
Get the values of multiple signals at a specific timestamp.
- **Query:** `{"cmd": "get_snapshot", "args": {"signals": ["TOP.clk", "TOP.count"], "time": 5000}}`
- **Response:** `{"status": "success", "data": {"TOP.clk": "1", "TOP.count": "b00001010"}}`

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
