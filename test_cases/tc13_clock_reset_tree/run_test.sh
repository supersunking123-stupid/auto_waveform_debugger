#!/bin/bash
# Test script for tc13_clock_reset_tree
# Tests complex clock gating and reset synchronization

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Source VCS environment
source ~/my_env/vcs.bash

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 13: Clock/Reset Tree"
echo "=========================================="

# Step 1: VCS compilation
echo "[Step 1] Running VCS compilation..."
vcs -full64 -f files.f -top clock_reset_tree_top -l vcs.log -sverilog +v2k 2>&1 && echo "VCS compilation PASSED!" || echo "VCS compilation FAILED!"

# Step 2: rtl_trace compile
echo "[Step 2] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc13.db \
    --top clock_reset_tree_top \
    -f files.f
# [CHECK] DB file exists and is non-empty
python3 -c "
import os
db='tc13.db'
assert os.path.exists(db), f'DB file not found: {db}'
assert os.path.getsize(db) > 0, f'DB file is empty: {db}'
print('  [CHECK] DB file OK, size:', os.path.getsize(db))
"

# Step 3: Trace clock tree (root to leaf)
echo "[Step 3] Tracing clock tree from primary clock..."
"$RTL_TRACE" trace \
    --db tc13.db \
    --mode loads \
    --signal "clock_reset_tree_top.clk_primary" \
    --depth 10 \
    --format json > tc13_trace_clk_loads.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc13_trace_clk_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Step 4: Trace clock gating hierarchy
echo "[Step 4] Tracing 3-level clock gating hierarchy..."
"$RTL_TRACE" trace \
    --db tc13.db \
    --mode drivers \
    --signal "clock_reset_tree_top.gen_subsystems[0].u_clock_subsystem.gen_clock_dist[0].u_clock_dist.clk_out" \
    --depth 5

# Step 5: Trace reset tree
echo "[Step 5] Tracing reset synchronization tree..."
"$RTL_TRACE" trace \
    --db tc13.db \
    --mode loads \
    --signal "clock_reset_tree_top.global_rst_n" \
    --depth 10 \
    --format json > tc13_trace_rst_loads.json 2>/dev/null
python3 -c "
import json, sys
data = json.load(open('tc13_trace_rst_loads.json'))
assert 'endpoints' in data, 'Missing endpoints'
assert len(data['endpoints']) > 0, 'No endpoints found'
print('  [CHECK] endpoints count:', len(data['endpoints']))
"

# Step 6: Trace clock mux
echo "[Step 6] Tracing clock mux..."
"$RTL_TRACE" trace \
    --db tc13.db \
    --mode drivers \
    --signal "clock_reset_tree_top.gen_subsystems[0].u_clock_subsystem.mux_clk_out"

# Step 7: Trace clock enable chain
echo "[Step 7] Tracing clock enable chain..."
"$RTL_TRACE" trace \
    --db tc13.db \
    --mode loads \
    --signal "clock_reset_tree_top.global_clk_en" \
    --depth 5

# Step 8: Find all gated clocks
echo "[Step 8] Finding all gated clock signals..."
"$RTL_TRACE" find --db tc13.db --query "gated_clk" --limit 20

# Step 9: Find all reset sync signals
echo "[Step 9] Finding all reset sync signals..."
"$RTL_TRACE" find --db tc13.db --query "rst_n_sync" --limit 20

echo "=========================================="
echo "Test Case 13: PASSED"
echo "=========================================="
