#!/bin/bash
# Test script for tc09_counter_chain
# Tests counter chain with carry propagation

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 09: Counter Chain"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc09.db \
    --top counter_chain_top \
    -f files.f

# Trace: drivers to counter output (sequential)
echo "[Step 2] Tracing drivers to counter output..."
"$RTL_TRACE" trace \
    --db tc09.db \
    --mode drivers \
    --signal "counter_chain_top.counter_out[0]" \
    --depth 5

# Trace: carry chain propagation
echo "[Step 3] Tracing carry chain..."
"$RTL_TRACE" trace \
    --db tc09.db \
    --mode drivers \
    --signal "counter_chain_top.carry_chain[2]"

# Trace: loads from enable
echo "[Step 4] Tracing loads from enable..."
"$RTL_TRACE" trace \
    --db tc09.db \
    --mode loads \
    --signal "counter_chain_top.enable"

echo "=========================================="
echo "Test Case 09: PASSED"
echo "=========================================="
