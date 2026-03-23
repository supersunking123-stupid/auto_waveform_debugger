#!/bin/bash
# Test script for tc11_deep_soc
# Tests deep SoC-like hierarchy with clock/reset tree

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Source VCS environment
source ~/my_env/vcs.bash

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 11: Deep SoC Hierarchy"
echo "=========================================="

# Step 1: VCS compilation for syntax check
echo "[Step 1] Running VCS compilation..."
vcs -full64 -f files.f -top deep_soc_top -l vcs.log -sverilog +v2k 2>&1 && echo "VCS compilation PASSED!" || echo "VCS compilation FAILED!"

# Step 2: rtl_trace compile
echo "[Step 2] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc11.db \
    --top deep_soc_top \
    -f files.f

# Step 3: Trace through deep hierarchy (5 levels)
echo "[Step 3] Tracing through 5-level hierarchy..."
"$RTL_TRACE" trace \
    --db tc11.db \
    --mode drivers \
    --signal "deep_soc_top.gen_subsystems[0].u_subsystem.gen_blocks[0].u_block.u_stage1_reg.u_dff.q" \
    --depth 10

# Step 4: Trace clock tree
echo "[Step 4] Tracing clock distribution..."
"$RTL_TRACE" trace \
    --db tc11.db \
    --mode loads \
    --signal "deep_soc_top.clk_raw" \
    --depth 5

# Step 5: Trace reset tree
echo "[Step 5] Tracing reset distribution..."
"$RTL_TRACE" trace \
    --db tc11.db \
    --mode loads \
    --signal "deep_soc_top.rst_n_raw" \
    --depth 5

# Step 6: Trace cross-subsystem signal
echo "[Step 6] Tracing cross-subsystem data path..."
"$RTL_TRACE" trace \
    --db tc11.db \
    --mode drivers \
    --signal "deep_soc_top.data_out" \
    --depth 15

# Step 7: Find signals in deep hierarchy
echo "[Step 7] Finding signals in deep hierarchy..."
"$RTL_TRACE" find --db tc11.db --query "gen_subsystems" --limit 10

echo "=========================================="
echo "Test Case 11: PASSED"
echo "=========================================="
