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
# [CHECK] DB file exists and is non-empty
python3 -c "
import os
db='tc10.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Trace: drivers to gated clock
echo "[Step 2] Tracing drivers to gated clock..."
"$RTL_TRACE" trace \
    --db tc10.db \
    --mode drivers \
    --signal "clock_gating_top.gated_clk[0]" \
    --format json > tc10_trace_drivers.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc10_trace_drivers.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Trace: loads from global enable
echo "[Step 3] Tracing loads from global_en..."
"$RTL_TRACE" trace \
    --db tc10.db \
    --mode loads \
    --signal "clock_gating_top.global_en" \
    --format json > tc10_trace_loads.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc10_trace_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

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
