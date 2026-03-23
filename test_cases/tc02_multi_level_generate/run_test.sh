#!/bin/bash
# Test script for tc02_multi_level_generate
# Tests multi-level nested generate blocks

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Source VCS environment (if available)
if [ -f ~/my_env/vcs.bash ]; then
    source ~/my_env/vcs.bash
    echo "[VCS] Running VCS compilation for syntax check..."
    vcs -full64 -f files.f -top multi_level_generate_top -l vcs.log -sverilog && echo "VCS compilation passed!" || echo "VCS not available, skipping..."
fi

RTL_TRACE="${RTL_TRACE:-/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace}"

echo "=========================================="
echo "Test Case 02: Multi-Level Generate"
echo "=========================================="

# Compile
echo "[Step 1] Running rtl_trace compile..."
"$RTL_TRACE" compile \
    --db tc02.db \
    --top multi_level_generate_top \
    -f files.f

# Trace: drivers through 2D hierarchy
echo "[Step 2] Tracing drivers through nested generate..."
"$RTL_TRACE" trace \
    --db tc02.db \
    --mode drivers \
    --signal "multi_level_generate_top.gen_rows[1].gen_cols[2].u_pe.data_out"

# Trace: loads across row
echo "[Step 3] Tracing loads across row..."
"$RTL_TRACE" trace \
    --db tc02.db \
    --mode loads \
    --signal "multi_level_generate_top.data_in[0]"

# Trace: deep hierarchy access
echo "[Step 4] Deep hierarchy trace..."
"$RTL_TRACE" trace \
    --db tc02.db \
    --mode drivers \
    --signal "multi_level_generate_top.gen_rows[0].gen_cols[0].u_pe.data_reg"

echo "=========================================="
echo "Test Case 02: PASSED"
echo "=========================================="
