#!/bin/bash
# Test script for tc01_generate_loop
# Tests large generate loop feature

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

# Source VCS environment only if the site setup script exists; otherwise rely
# on vcs already being on PATH (VCS steps are skipped when it is not).
if [ -f ~/my_env/vcs.bash ]; then
    source ~/my_env/vcs.bash
fi

# VCS compilation for syntax check
echo "[VCS] Running VCS compilation for syntax check..."
if command -v vcs >/dev/null 2>&1; then
    vcs -full64 -f files.f -top generate_loop_top -l vcs.log -sverilog && echo "VCS compilation passed!" || echo "VCS compilation FAILED!"
else
    echo "vcs not found on PATH, skipping VCS compilation step..."
fi

RTL_TRACE="${RTL_TRACE:-$ROOT_DIR/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 01: Large Generate Loop"
echo "=========================================="

# Step 1/2: rtl_trace compile (also serves as syntax check)
echo "[Step 1] Running rtl_trace compile (syntax check + DB generation)..."
"$RTL_TRACE" compile \
    --db tc01.db \
    --top generate_loop_top \
    -f files.f
# [CHECK] DB file exists and is non-empty
"$PYTHON" -c "
import os
db='tc01.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Step 2: Trace drivers example
echo "[Step 2] Running rtl_trace trace (drivers mode)..."
"$RTL_TRACE" trace \
    --db tc01.db \
    --mode drivers \
    --signal "generate_loop_top.stage_data[0]" \
    --format json > tc01_trace_drivers.json 2>/dev/null
"$PYTHON" -c "
import json, sys
data = json.load(open('tc01_trace_drivers.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Step 3: Trace loads example
echo "[Step 3] Running rtl_trace trace (loads mode)..."
"$RTL_TRACE" trace \
    --db tc01.db \
    --mode loads \
    --signal "generate_loop_top.data_in" \
    --format json > tc01_trace_loads.json 2>/dev/null
"$PYTHON" -c "
import json, sys
data = json.load(open('tc01_trace_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Step 4: Trace through generated instance
echo "[Step 4] Tracing signal through generated instance..."
"$RTL_TRACE" trace \
    --db tc01.db \
    --mode drivers \
    --signal "generate_loop_top.gen_buffer_stages[0].u_buffer.out"

echo "=========================================="
echo "Test Case 01: PASSED"
echo "=========================================="
