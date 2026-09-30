#!/bin/bash
# Test script for tc08_priority_encoder
# Tests priority if-else and case/casex

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
echo "Test Case 08: Priority Encoder"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc08.db \
    --top priority_encoder_top \
    -f files.f
# [CHECK] DB file exists and is non-empty
"$PYTHON" -c "
import os
db='tc08.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Trace: drivers to grant_idx (should show priority logic)
echo "[Step 2] Tracing drivers to grant_idx..."
"$RTL_TRACE" trace \
    --db tc08.db \
    --mode drivers \
    --signal "priority_encoder_top.grant_idx" \
    --format json > tc08_trace_drivers.json 2>/dev/null
"$PYTHON" -c "
import json, sys
data = json.load(open('tc08_trace_drivers.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Trace: loads from req input
echo "[Step 3] Tracing loads from req..."
"$RTL_TRACE" trace \
    --db tc08.db \
    --mode loads \
    --signal "priority_encoder_top.req[5]" \
    --format json > tc08_trace_loads.json 2>/dev/null
"$PYTHON" -c "
import json, sys
data = json.load(open('tc08_trace_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Trace: drivers to onehot output
echo "[Step 4] Tracing drivers to grant_onehot..."
"$RTL_TRACE" trace \
    --db tc08.db \
    --mode drivers \
    --signal "priority_encoder_top.grant_onehot"

echo "=========================================="
echo "Test Case 08: PASSED"
echo "=========================================="
