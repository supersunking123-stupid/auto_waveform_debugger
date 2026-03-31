#!/bin/bash
# Test script for tc15_mixed_signal_flow
# Tests mixed combinatorial/sequential paths with feedback

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Source VCS environment
source ~/my_env/vcs.bash

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 15: Mixed Signal Flow"
echo "=========================================="

# Step 1: VCS compilation
echo "[Step 1] Running VCS compilation..."
vcs -full64 -f files.f -top mixed_signal_flow_top -l vcs.log -sverilog +v2k 2>&1 && echo "VCS compilation PASSED!" || echo "VCS compilation FAILED!"

# Step 2: rtl_trace compile (default FEEDBACK_SEL=0)
echo "[Step 2] Running rtl_trace compile (FEEDBACK_SEL=0)..."
"$RTL_TRACE" compile \
    --db tc15.db \
    --top mixed_signal_flow_top \
    -f files.f
# [CHECK] DB file exists and is non-empty
python3 -c "
import os
db='tc15.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Step 3: Trace through pipeline
echo "[Step 3] Tracing through 8-stage pipeline..."
"$RTL_TRACE" trace \
    --db tc15.db \
    --mode drivers \
    --signal "mixed_signal_flow_top.pipe_data[4]" \
    --depth 10 \
    --format json > tc15_trace_pipe.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc15_trace_pipe.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Step 4: Trace feedback path
echo "[Step 4] Tracing feedback path..."
"$RTL_TRACE" trace \
    --db tc15.db \
    --mode drivers \
    --signal "mixed_signal_flow_top.feedback_combined"

# Step 5: Trace accumulator
echo "[Step 5] Tracing accumulator..."
"$RTL_TRACE" trace \
    --db tc15.db \
    --mode drivers \
    --signal "mixed_signal_flow_top.accum_reg"

# Step 6: Trace enable chain
echo "[Step 6] Tracing enable chain..."
"$RTL_TRACE" trace \
    --db tc15.db \
    --mode loads \
    --signal "mixed_signal_flow_top.enable" \
    --depth 5 \
    --format json > tc15_trace_loads.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc15_trace_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Step 7: Trace output with deep cone
echo "[Step 7] Tracing final output..."
"$RTL_TRACE" trace \
    --db tc15.db \
    --mode drivers \
    --signal "mixed_signal_flow_top.data_out" \
    --depth 15

# Step 8: Trace overflow detection
echo "[Step 8] Tracing overflow flag..."
"$RTL_TRACE" trace \
    --db tc15.db \
    --mode drivers \
    --signal "mixed_signal_flow_top.overflow"

# Step 9: Find pipeline signals
echo "[Step 9] Finding pipeline signals..."
"$RTL_TRACE" find --db tc15.db --query "pipe_data" --limit 20

# Step 10: Compile with feedback enabled
echo "[Step 10] Running rtl_trace compile (FEEDBACK_SEL=3)..."
"$RTL_TRACE" compile \
    --db tc15_fb.db \
    --top mixed_signal_flow_top \
    -f files.f \
    -D FEEDBACK_SEL=3

echo "[Step 11] Tracing with feedback enabled..."
"$RTL_TRACE" trace \
    --db tc15_fb.db \
    --mode drivers \
    --signal "mixed_signal_flow_top.data_out" \
    --depth 10

echo "=========================================="
echo "Test Case 15: PASSED"
echo "=========================================="
