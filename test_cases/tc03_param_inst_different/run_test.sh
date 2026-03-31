#!/bin/bash
# Test script for tc03_param_inst_different
# Tests same module instantiated with different parameters

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 03: Param Inst Different"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc03.db \
    --top param_inst_different_top \
    -f files.f
# [CHECK] DB file exists and is non-empty
python3 -c "
import os
db='tc03.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Trace: drivers to FIFO with specific params
echo "[Step 2] Tracing drivers to 8-bit FIFO..."
"$RTL_TRACE" trace \
    --db tc03.db \
    --mode drivers \
    --signal "param_inst_different_top.u_fifo0.mem[0]" \
    --format json > tc03_trace_drivers.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc03_trace_drivers.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Trace: drivers to wider FIFO
echo "[Step 3] Tracing drivers to 16-bit FIFO..."
"$RTL_TRACE" trace \
    --db tc03.db \
    --mode drivers \
    --signal "param_inst_different_top.u_fifo1.mem[0]"

# Trace: loads from different param instances
echo "[Step 4] Tracing loads from deeper FIFO..."
"$RTL_TRACE" trace \
    --db tc03.db \
    --mode loads \
    --signal "param_inst_different_top.u_fifo2.wr_data" \
    --format json > tc03_trace_loads.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc03_trace_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

echo "=========================================="
echo "Test Case 03: PASSED"
echo "=========================================="
