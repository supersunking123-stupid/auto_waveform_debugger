#!/bin/bash
# Test script for tc05_multi_dim_arrays
# Tests packed/unpacked arrays, bit-select, part-select

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 05: Multi-Dim Arrays"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc05.db \
    --top multi_dim_arrays_top \
    -f files.f

# List signals to find proper paths
echo "[Step 2] Finding signals with 'storage'..."
"$RTL_TRACE" find --db tc05.db --query "storage" --limit 10

# Trace: data_in (input array)
echo "[Step 3] Tracing data_in loads..."
"$RTL_TRACE" trace \
    --db tc05.db \
    --mode loads \
    --signal "multi_dim_arrays_top.data_in"

# Trace: bit_vector_out
echo "[Step 4] Tracing bit_vector_out drivers..."
"$RTL_TRACE" trace \
    --db tc05.db \
    --mode drivers \
    --signal "multi_dim_arrays_top.bit_vector_out"

# Trace: individual bit
echo "[Step 5] Tracing individual bit..."
"$RTL_TRACE" trace \
    --db tc05.db \
    --mode drivers \
    --signal "multi_dim_arrays_top.bit_0_0_0"

echo "=========================================="
echo "Test Case 05: PASSED"
echo "=========================================="
