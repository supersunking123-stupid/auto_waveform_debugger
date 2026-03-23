#!/bin/bash
# Test script for tc06_structural_hierarchy
# Tests deep module instantiation chain

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 06: Structural Hierarchy"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc06.db \
    --top structural_hierarchy_top \
    -f files.f

# Trace: drivers across multiple hierarchy levels
echo "[Step 2] Tracing drivers across hierarchy (deep traversal)..."
"$RTL_TRACE" trace \
    --db tc06.db \
    --mode drivers \
    --signal "structural_hierarchy_top.chain_2" \
    --depth 10

# Trace: loads from input through hierarchy
echo "[Step 3] Tracing loads from input..."
"$RTL_TRACE" trace \
    --db tc06.db \
    --mode loads \
    --signal "structural_hierarchy_top.data_in"

# Trace: to final output
echo "[Step 4] Tracing drivers to final output..."
"$RTL_TRACE" trace \
    --db tc06.db \
    --mode drivers \
    --signal "structural_hierarchy_top.data_out" \
    --depth 10

echo "=========================================="
echo "Test Case 06: PASSED"
echo "=========================================="
