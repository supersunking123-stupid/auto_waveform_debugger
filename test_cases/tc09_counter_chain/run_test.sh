#!/bin/bash
# Test script for tc09_counter_chain
# Tests counter chain with carry propagation

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Project virtualenv Python (override with PYTHON=/path/to/interpreter).
PYTHON="${PYTHON:-$ROOT_DIR/.venv/bin/python3}"
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: Python interpreter not executable: $PYTHON" >&2
    echo "Create the repo .venv or set PYTHON to an executable interpreter path." >&2
    exit 1
fi

RTL_TRACE="${RTL_TRACE:-$ROOT_DIR/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 09: Counter Chain"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc09.db \
    --top counter_chain_top \
    -f files.f
# [CHECK] DB file exists and is non-empty
"$PYTHON" -c "
import os
db='tc09.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Trace: drivers to counter output (sequential)
echo "[Step 2] Tracing drivers to counter output..."
"$RTL_TRACE" trace \
    --db tc09.db \
    --mode drivers \
    --signal "counter_chain_top.counter_out[0]" \
    --depth 5 \
    --format json > tc09_trace_drivers.json 2>/dev/null
"$PYTHON" -c "
import json, sys
data = json.load(open('tc09_trace_drivers.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

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
    --signal "counter_chain_top.enable" \
    --format json > tc09_trace_loads.json 2>/dev/null
"$PYTHON" -c "
import json, sys
data = json.load(open('tc09_trace_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

echo "=========================================="
echo "Test Case 09: PASSED"
echo "=========================================="
