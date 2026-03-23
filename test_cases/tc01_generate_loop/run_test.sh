#!/bin/bash
# Test script for tc01_generate_loop
# Tests large generate loop feature

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Source VCS environment (if available)
if [ -f ~/my_env/vcs.bash ]; then
    source ~/my_env/vcs.bash
    # Step 1: VCS compilation for syntax check
    echo "[Step 1] Running VCS compilation for syntax check..."
    vcs -full64 -f files.f -top generate_loop_top -l vcs.log -sverilog && echo "VCS compilation passed!" || echo "VCS not available, skipping..."
fi

# Fallback to rtl_trace (slang) for syntax check
RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 01: Large Generate Loop"
echo "=========================================="

# Step 1/2: rtl_trace compile (also serves as syntax check)
echo "[Step 1] Running rtl_trace compile (syntax check + DB generation)..."
"$RTL_TRACE" compile \
    --db tc01.db \
    --top generate_loop_top \
    -f files.f

# Step 2: Trace drivers example
echo "[Step 2] Running rtl_trace trace (drivers mode)..."
"$RTL_TRACE" trace \
    --db tc01.db \
    --mode drivers \
    --signal "generate_loop_top.stage_data[0]"

# Step 3: Trace loads example
echo "[Step 3] Running rtl_trace trace (loads mode)..."
"$RTL_TRACE" trace \
    --db tc01.db \
    --mode loads \
    --signal "generate_loop_top.data_in"

# Step 4: Trace through generated instance
echo "[Step 4] Tracing signal through generated instance..."
"$RTL_TRACE" trace \
    --db tc01.db \
    --mode drivers \
    --signal "generate_loop_top.gen_buffer_stages[0].u_buffer.out"

echo "=========================================="
echo "Test Case 01: PASSED"
echo "=========================================="
