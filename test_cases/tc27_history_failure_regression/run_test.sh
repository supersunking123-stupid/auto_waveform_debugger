#!/bin/bash
# Test script for tc27_history_failure_regression
# Regression tests for previously failing waveform queries

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Go to test_cases directory where assets are located
TEST_CASES_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$TEST_CASES_DIR/.." && pwd)"

# Check required assets
WAVE_FSDB="${TEST_CASES_DIR}/wave.fsdb"
WAVE_CLI_BIN="${ROOT_DIR}/waveform_explorer/build/wave_agent_cli"

[ -f "$WAVE_FSDB" ] || { echo "ERROR: wave.fsdb not found at $WAVE_FSDB"; exit 1; }
[ -f "$WAVE_CLI_BIN" ] || { echo "ERROR: wave_agent_cli binary not found at $WAVE_CLI_BIN"; exit 1; }

echo "=========================================="
echo "Test Case 27: History Failure Regression"
echo "=========================================="
echo ""
echo "Running regression tests for previously failing queries..."
python3 test_tc27_history_failure.py

echo ""
echo "=========================================="
echo "Test Case 27: PASSED"
echo "=========================================="
