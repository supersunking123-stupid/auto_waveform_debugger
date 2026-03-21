# standalone_trace_graph

Small standalone RTL hierarchy elaborator built on top of `third_party/slang`.

## What It Does

`rtl_elab` parses and elaborates Verilog / SystemVerilog, starts from a user-specified top module, and emits a hierarchy tree that contains only:

- parent / child hierarchy
- preserved scope nodes for generate blocks and instance arrays
- original module name
- uniquified module name when effective parameters differ
- effective parameter values for each instance

Supported output formats:

- `json`
- `text`

## Build

```bash
cmake -B build -GNinja .
cmake --build build -j$(nproc)
```

## Usage

```bash
./build/rtl_elab -top <module> [--format json|text] [--relax-defparam] [slang source args...]
```

Notes:

- `-top` is required.
- `--relax-defparam` ignores defparam-related elaboration errors, matching the behavior needed by some existing flows.
- `--single-unit` can be passed through to slang when a testbench relies on cross-file macro visibility.

Example:

```bash
./build/rtl_elab --format json -top top tests/data/smoke.sv
```

## Implementation Summary

Completed so far:

- Added standalone CMake project in [CMakeLists.txt](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace_graph/CMakeLists.txt)
- Implemented `rtl_elab` in [main.cc](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace_graph/main.cc)
- Used `slang::driver::Driver` and full elaboration via `createCompilation()`
- Implemented effective parameter capture for value and type parameters
- Implemented deterministic uniquified module names using a stable hash of effective parameter values
- Preserved generate scopes and instance array scopes in the emitted tree
- Added a smoke regression in [tests/smoke_test.py](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace_graph/tests/smoke_test.py)
- Reworked JSON emission to stream directly from the elaborated `slang` tree instead of building a full in-memory hierarchy IR first

## NVDLA Run

Validated on NVDLA using `~/DVT/nvdla/hw/verif/sim/Makefile.vcs` as the source of compile inputs.

Successful invocation characteristics:

- top: `top`
- required options:
  - `--relax-defparam`
  - `--single-unit`
- output JSON size: about `70 MB`

Generated artifacts currently in this repo:

- [nvdla_top_hier.json](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace_graph/nvdla_top_hier.json)
- [nvdla_top_hier_memmon.json](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace_graph/nvdla_top_hier_memmon.json)
- [nvdla_top_hier_stream_memmon.json](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace_graph/nvdla_top_hier_stream_memmon.json)

Observed top-level summary for NVDLA:

- root: `top`
- root children: `16`
- `nvdla_top` elaborates as `NV_nvdla`
- `NV_nvdla` child partitions:
  - `u_partition_o`
  - `u_partition_c`
  - `u_partition_ma`
  - `u_partition_mb`
  - `u_partition_a`
  - `u_partition_p`

## Memory Findings

Memory was measured on NVDLA with 1-second sampling from `/proc/<pid>/status`.

Before streamed JSON rewrite:

- peak RSS: `2,267,124 KB` (`2.16 GiB`)
- peak VmHWM: `2,281,588 KB` (`2.18 GiB`)

After streamed JSON rewrite:

- peak RSS: `2,233,684 KB` (`2.13 GiB`)
- peak VmHWM: `2,281,812 KB` (`2.18 GiB`)

Conclusion:

- Streaming JSON reduced peak RSS only slightly, about `1.5%`.
- The dominant memory cost is still `slang` parse + elaboration, not the old in-memory hierarchy tree.
- The current NVDLA flow likely needs `--single-unit`, which also pushes frontend memory up.

Memory logs:

- [nvdla_top_hier_memmon.mem.tsv](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace_graph/nvdla_top_hier_memmon.mem.tsv)
- [nvdla_top_hier_stream_memmon.mem.tsv](/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace_graph/nvdla_top_hier_stream_memmon.mem.tsv)

## Current Status

The tool is functional and validated on both:

- the local smoke test
- the NVDLA hierarchy use case

Known limitation:

- Peak memory is still mainly bounded by `slang` elaboration on large designs.
