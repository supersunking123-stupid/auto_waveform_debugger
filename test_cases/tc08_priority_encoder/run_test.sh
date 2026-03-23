#!/bin/bash
# Test script for tc08_priority_encoder
# Tests priority if-else and case/casex

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 08: Priority Encoder"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc08.db \
    --top priority_encoder_top \
    -f files.f

# Trace: drivers to grant_idx (should show priority logic)
echo "[Step 2] Tracing drivers to grant_idx..."
"$RTL_TRACE" trace \
    --db tc08.db \
    --mode drivers \
    --signal "priority_encoder_top.grant_idx"

# Trace: loads from req input
echo "[Step 3] Tracing loads from req..."
"$RTL_TRACE" trace \
    --db tc08.db \
    --mode loads \
    --signal "priority_encoder_top.req[5]"

# Trace: drivers to onehot output
echo "[Step 4] Tracing drivers to grant_onehot..."
"$RTL_TRACE" trace \
    --db tc08.db \
    --mode drivers \
    --signal "priority_encoder_top.grant_onehot"

echo "=========================================="
echo "Test Case 08: PASSED"
echo "=========================================="
