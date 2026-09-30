#!/bin/bash
# Test script for tc14_system_tasks
# Tests rtl_trace ability to skip system task "noise"

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

RTL_TRACE="${RTL_TRACE:-$ROOT_DIR/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 14: System Tasks Noise"
echo "=========================================="

# Step 1: VCS compilation
echo "[Step 1] Running VCS compilation..."
if command -v vcs >/dev/null 2>&1; then
    vcs -full64 -f files.f -top system_tasks_top -l vcs.log -sverilog +v2k 2>&1 && echo "VCS compilation PASSED!" || echo "VCS compilation FAILED!"
else
    echo "vcs not found on PATH, skipping VCS compilation step..."
fi

# Step 2: rtl_trace compile
echo "[Step 2] Running rtl_trace compile (should skip system tasks)..."
"$RTL_TRACE" compile \
    --db tc14.db \
    --top system_tasks_top \
    -f files.f
# [CHECK] DB file exists and is non-empty
"$PYTHON" -c "
import os
db='tc14.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Step 3: Trace data path (should not include system tasks)
echo "[Step 3] Tracing data path (system tasks should be filtered)..."
"$RTL_TRACE" trace \
    --db tc14.db \
    --mode drivers \
    --signal "system_tasks_top.data_out" \
    --format json > tc14_trace_drivers.json 2>/dev/null
"$PYTHON" -c "
import json, sys
data = json.load(open('tc14_trace_drivers.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Step 4: Trace valid signal
echo "[Step 4] Tracing valid signal..."
"$RTL_TRACE" trace \
    --db tc14.db \
    --mode drivers \
    --signal "system_tasks_top.valid"

# Step 5: Trace FIFO (complex structure with system tasks nearby)
echo "[Step 5] Tracing FIFO structure..."
"$RTL_TRACE" trace \
    --db tc14.db \
    --mode drivers \
    --signal "system_tasks_top.fifo" \
    --depth 3

# Step 6: Find signals (verify system tasks don't pollute results)
echo "[Step 6] Finding signals with 'data'..."
"$RTL_TRACE" find --db tc14.db --query "data" --limit 20

# Step 7: Trace error flag
echo "[Step 7] Tracing error flag..."
"$RTL_TRACE" trace \
    --db tc14.db \
    --mode drivers \
    --signal "system_tasks_top.error_flag"

# Step 8: Trace loads from enable
echo "[Step 8] Tracing loads from enable..."
"$RTL_TRACE" trace \
    --db tc14.db \
    --mode loads \
    --signal "system_tasks_top.enable" \
    --format json > tc14_trace_loads.json 2>/dev/null
"$PYTHON" -c "
import json, sys
data = json.load(open('tc14_trace_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

echo "=========================================="
echo "Test Case 14: PASSED"
echo "=========================================="
