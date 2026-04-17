# auto_waveform_debugger — Project Capabilities

This document describes what auto_waveform_debugger can do. It is intended for an AI chatbot to understand the project's functionality. No implementation details are included.

## What This Project Does

auto_waveform_debugger helps AI agents debug digital hardware (RTL) failures by combining two sources of evidence:

- **Structural evidence** — how signals are connected in the design (who drives what, what loads what)
- **Temporal evidence** — what signal values are over time (waveform data)

Together, these allow an agent to trace a failing signal back through logic cones, observe values at each stage, and identify root causes.

## Components

### 1. RTL Structural Tracer (`rtl_trace`)

Parses Verilog/SystemVerilog RTL source and builds a connectivity database.

| Command | What It Does |
|---|---|
| `compile` | Builds a binary connectivity database from RTL source files |
| `trace` | Traces structural drivers or loads for any signal, with optional bit-range filtering, depth limits, and include/exclude patterns |
| `find` | Searches signal names with plain text or regex |
| `hier` | Browses the instance hierarchy tree with depth control |
| `whereis-instance` | Looks up an instance's module type, source location, and elaborated parameters |
| `serve` | Interactive mode for repeated queries without reloading the database |

### 2. Waveform Explorer

Queries VCD/FST/FSDB waveform files for temporal signal data.

| Function | What It Does |
|---|---|
| `list_signals` | Enumerates signals in the waveform, filterable by hierarchy, pattern, or type |
| `get_signal_info` | Returns metadata (width, type) for one signal |
| `get_snapshot` | Gets values of multiple signals at one timestamp |
| `get_value_at_time` | Gets one signal value at one timestamp |
| `get_transitions` | Returns compressed transition history in a time window |
| `count_transitions` | Counts edges (posedge/negedge) or value toggles in a time window |
| `get_signal_overview` | Summarizes signal behavior over a time range (clock-like, static, switching segments) |
| `find_edge` | Finds the next or previous signal transition from a reference time |
| `find_value_intervals` | Finds all time ranges where a signal holds a specific value |
| `find_condition` | Finds the first timestamp where a logical expression becomes true |
| `analyze_pattern` | Classifies signal behavior (clock, static, dynamic) with statistics |
| `dump_waveform_data` | Exports large waveform datasets to a local file for offline processing |

### 3. Cross-Link Tools (Structural + Temporal Combined)

These tools merge structural tracing with waveform observation to explain signal behavior.

| Function | What It Does |
|---|---|
| `trace_with_snapshot` | Traces structural drivers/loads of a signal and returns waveform values for all cone endpoints at a given time |
| `explain_signal_at_time` | Traces the structural cone, samples endpoint values, and ranks candidate logic paths by how well they explain the signal state |
| `rank_cone_by_time` | Scores all signals in a structural cone by their transition activity near a timestamp, highlighting stuck or active candidates |
| `explain_edge_cause` | Finds a signal transition edge and traces the upstream cause chain with waveform evidence at each stage |

### 4. Session and Virtual Signal Management

The waveform tools operate through sessions that maintain state across queries.

| Capability | What It Does |
|---|---|
| **Sessions** | Saved waveform views tied to a specific waveform file |
| **Cursor** | A current focus time, usable as a time reference in other queries |
| **Bookmarks** | Named saved timestamps, referenceable by name |
| **Signal Groups** | Named saved signal lists for batch queries |
| **Bus concat** | Concatenates multiple signals into one virtual bus (Verilog `{a,b}` style) |
| **Bus slice** | Extracts a bit range from a bus |
| **Bus split** | Splits a bus into equal-width sub-buses |
| **Bus reverse** | Reverses the bit order of a bus |
| **Signal expression** | Creates a virtual signal from a Verilog-like expression (supports bitwise, logical, arithmetic, shift, and ternary operators) |

## Supported File Formats

- **RTL source**: Verilog, SystemVerilog (parsed via slang)
- **Waveforms**: VCD, FST, FSDB

## Output Formats

All query tools support text and JSON output. The JSON format is designed for machine consumption by AI agents.
