#!/bin/bash
# Test script for tc10_clock_gating
# Tests clock gating cells and high-fanout nets

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 10: Clock Gating"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc10.db \
    --top clock_gating_top \
    -f files.f

# Trace: drivers to gated clock
echo "[Step 2] Tracing drivers to gated clock..."
"$RTL_TRACE" trace \
    --db tc10.db \
    --mode drivers \
    --signal "clock_gating_top.gated_clk[0]"

# Trace: loads from global enable
echo "[Step 3] Tracing loads from global_en..."
"$RTL_TRACE" trace \
    --db tc10.db \
    --mode loads \
    --signal "clock_gating_top.global_en"

# Trace: clock network fanout
echo "[Step 4] Tracing clock network..."
"$RTL_TRACE" trace \
    --db tc10.db \
    --mode loads \
    --signal "clock_gating_top.clk" \
    --depth 5

echo "=========================================="
echo "Test Case 10: PASSED"
echo "=========================================="
