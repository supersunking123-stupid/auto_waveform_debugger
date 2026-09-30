#!/bin/bash
# Test script for tc05_multi_dim_arrays
# Tests packed/unpacked arrays, bit-select, part-select

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
echo "Test Case 05: Multi-Dim Arrays"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc05.db \
    --top multi_dim_arrays_top \
    -f files.f
# [CHECK] DB file exists and is non-empty
"$PYTHON" -c "
import os
db='tc05.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

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
    --signal "multi_dim_arrays_top.bit_vector_out" \
    --format json > tc05_trace_drivers.json 2>/dev/null
"$PYTHON" -c "
import json, sys
data = json.load(open('tc05_trace_drivers.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Trace: individual bit
echo "[Step 5] Tracing individual bit..."
"$RTL_TRACE" trace \
    --db tc05.db \
    --mode drivers \
    --signal "multi_dim_arrays_top.bit_0_0_0"

echo "=========================================="
echo "Test Case 05: PASSED"
echo "=========================================="
