#!/bin/bash
# Test script for tc06_structural_hierarchy
# Tests deep module instantiation chain

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
echo "Test Case 06: Structural Hierarchy"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc06.db \
    --top structural_hierarchy_top \
    -f files.f
# [CHECK] DB file exists and is non-empty
"$PYTHON" -c "
import os
db='tc06.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Trace: drivers across multiple hierarchy levels
echo "[Step 2] Tracing drivers across hierarchy (deep traversal)..."
"$RTL_TRACE" trace \
    --db tc06.db \
    --mode drivers \
    --signal "structural_hierarchy_top.chain_2" \
    --depth 10 \
    --format json > tc06_trace_drivers.json 2>/dev/null
"$PYTHON" -c "
import json, sys
data = json.load(open('tc06_trace_drivers.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Trace: loads from input through hierarchy
echo "[Step 3] Tracing loads from input..."
"$RTL_TRACE" trace \
    --db tc06.db \
    --mode loads \
    --signal "structural_hierarchy_top.data_in" \
    --format json > tc06_trace_loads.json 2>/dev/null
"$PYTHON" -c "
import json, sys
data = json.load(open('tc06_trace_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

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
