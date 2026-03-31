#!/bin/bash
# Test script for tc07_mux_tree
# Tests large combinational mux tree

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 07: Mux Tree"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc07.db \
    --top mux_tree_top \
    -f files.f
# [CHECK] DB file exists and is non-empty
python3 -c "
import os
db='tc07.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Trace: drivers to output (should trace back through mux tree)
echo "[Step 2] Tracing drivers to output..."
"$RTL_TRACE" trace \
    --db tc07.db \
    --mode drivers \
    --signal "mux_tree_top.data_out" \
    --depth 10 \
    --format json > tc07_trace_drivers.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc07_trace_drivers.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Trace: loads from a specific input
echo "[Step 3] Tracing loads from input[5]..."
"$RTL_TRACE" trace \
    --db tc07.db \
    --mode loads \
    --signal "mux_tree_top.data_in[5]" \
    --format json > tc07_trace_loads.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc07_trace_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Trace: select signal loads
echo "[Step 4] Tracing loads from select..."
"$RTL_TRACE" trace \
    --db tc07.db \
    --mode loads \
    --signal "mux_tree_top.sel[0]"

echo "=========================================="
echo "Test Case 07: PASSED"
echo "=========================================="
