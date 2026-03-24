#!/bin/bash
# Test script for cross-link test case

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Go to test_cases directory where assets are located
TEST_CASES_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$TEST_CASES_DIR/.." && pwd)"

# Check required assets
RTL_TRACE_DB="${TEST_CASES_DIR}/rtl_trace.db"
WAVE_FSDB="${TEST_CASES_DIR}/wave.fsdb"
RTL_TRACE_BIN="${ROOT_DIR}/standalone_trace/build/rtl_trace"
WAVE_CLI_BIN="${ROOT_DIR}/waveform_explorer/build/wave_agent_cli"

[ -f "$RTL_TRACE_DB" ] || { echo "ERROR: rtl_trace.db not found"; exit 1; }
[ -f "$WAVE_FSDB" ] || { echo "ERROR: wave.fsdb not found"; exit 1; }
[ -f "$RTL_TRACE_BIN" ] || { echo "ERROR: rtl_trace binary not found"; exit 1; }
[ -f "$WAVE_CLI_BIN" ] || { echo "ERROR: wave_agent_cli binary not found"; exit 1; }

echo "Running tests..."
python3 test_*.py

echo "Test case completed."
