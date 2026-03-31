#!/bin/bash
# Test script for tc12_multi_cone
# Tests multiple driving/loading cones with reconvergent fanout

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Source VCS environment
source ~/my_env/vcs.bash

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 12: Multi-Cone Reconvergent"
echo "=========================================="

# Step 1: VCS compilation
echo "[Step 1] Running VCS compilation..."
vcs -full64 -f files.f -top multi_cone_top -l vcs.log -sverilog +v2k 2>&1 && echo "VCS compilation PASSED!" || echo "VCS compilation FAILED!"

# Step 2: rtl_trace compile
echo "[Step 2] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc12.db \
    --top multi_cone_top \
    -f files.f
# [CHECK] DB file exists and is non-empty
python3 -c "
import os
db='tc12.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Step 3: Trace reconvergent point
echo "[Step 3] Tracing first reconvergence point..."
"$RTL_TRACE" trace \
    --db tc12.db \
    --mode drivers \
    --signal "multi_cone_top.reconverge_0" \
    --depth 5 \
    --format json > tc12_trace_reconverge.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc12_trace_reconverge.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Step 4: Trace deep cone
echo "[Step 4] Tracing deep logic cone..."
"$RTL_TRACE" trace \
    --db tc12.db \
    --mode drivers \
    --signal "multi_cone_top.deep_cone[3]" \
    --depth 10

# Step 5: Trace final merge (multiple paths)
echo "[Step 5] Tracing final merge point..."
"$RTL_TRACE" trace \
    --db tc12.db \
    --mode drivers \
    --signal "multi_cone_top.final_merge" \
    --depth 15

# Step 6: Trace loads from input (fanout analysis)
echo "[Step 6] Tracing loads from input (high fanout)..."
"$RTL_TRACE" trace \
    --db tc12.db \
    --mode loads \
    --signal "multi_cone_top.in_a" \
    --depth 5 \
    --format json > tc12_trace_loads.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc12_trace_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Step 7: Trace convergent flag
echo "[Step 7] Tracing convergent flag logic..."
"$RTL_TRACE" trace \
    --db tc12.db \
    --mode drivers \
    --signal "multi_cone_top.flag_converge" \
    --depth 10

# Step 8: Trace feedback path
echo "[Step 8] Tracing feedback path..."
"$RTL_TRACE" trace \
    --db tc12.db \
    --mode drivers \
    --signal "multi_cone_top.out_primary" \
    --depth 5

echo "=========================================="
echo "Test Case 12: PASSED"
echo "=========================================="
