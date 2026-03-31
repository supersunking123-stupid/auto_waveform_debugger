#!/bin/bash
# Test script for tc04_conditional_generate
# Tests if/else generate and case generate

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 04: Conditional Generate"
echo "=========================================="

# Compile with pipeline enabled
echo "[Step 1] Running rtl_trace compile (USE_PIPELINE=1)..."
"$RTL_TRACE" compile \
    --db tc04_pipe.db \
    --top conditional_generate_top \
    -f files.f \
    -D USE_PIPELINE=1
# [CHECK] DB file exists and is non-empty
python3 -c "
import os
db='tc04_pipe.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Trace: drivers through pipeline
echo "[Step 2] Tracing drivers through pipeline..."
"$RTL_TRACE" trace \
    --db tc04_pipe.db \
    --mode drivers \
    --signal "conditional_generate_top.stage_data[2]" \
    --format json > tc04_trace_drivers.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc04_trace_drivers.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Compile with combinational path
echo "[Step 3] Running rtl_trace compile (USE_PIPELINE=0)..."
"$RTL_TRACE" compile \
    --db tc04_comb.db \
    --top conditional_generate_top \
    -f files.f \
    -D USE_PIPELINE=0

# Trace: combinational path
echo "[Step 4] Tracing combinational path..."
"$RTL_TRACE" trace \
    --db tc04_comb.db \
    --mode drivers \
    --signal "conditional_generate_top.result" \
    --format json > tc04_trace_comb.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc04_trace_comb.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Test case generate with different OP_SELECT
echo "[Step 5] Running rtl_trace compile (OP_SELECT=2 for AND)..."
"$RTL_TRACE" compile \
    --db tc04_and.db \
    --top conditional_generate_top \
    -f files.f \
    -D USE_PIPELINE=1 \
    -D OP_SELECT=2

echo "[Step 6] Tracing AND operation..."
"$RTL_TRACE" trace \
    --db tc04_and.db \
    --mode drivers \
    --signal "conditional_generate_top.op_result"

echo "=========================================="
echo "Test Case 04: PASSED"
echo "=========================================="
