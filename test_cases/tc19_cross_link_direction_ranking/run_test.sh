#!/bin/bash
# Test script for tc19_cross_link_direction_ranking
# Cross-link test case
#
# Runs against the small regenerable cross-link fixture (test_cases/rtl_trace.db
# + test_cases/wave.fsdb, built from test_cases/cross_link_fixture by
# test_cases/make_fixture.sh). The fixture is (re)built automatically when it
# is missing or stale.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TEST_CASES_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$TEST_CASES_DIR/.." && pwd)"

# Project virtualenv Python (override with PYTHON=/path/to/interpreter).
PYTHON="${PYTHON:-$ROOT_DIR/.venv/bin/python3}"
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: Python interpreter not executable: $PYTHON" >&2
    echo "Create the repo .venv or set PYTHON to an executable interpreter path." >&2
    exit 1
fi

RTL_TRACE_BIN="${RTL_TRACE:-$ROOT_DIR/standalone_trace/build/rtl_trace}"
WAVE_CLI_BIN="$ROOT_DIR/waveform_explorer/build/wave_agent_cli"
[ -f "$RTL_TRACE_BIN" ] || { echo "ERROR: rtl_trace binary not found: $RTL_TRACE_BIN"; exit 1; }
[ -f "$WAVE_CLI_BIN" ] || { echo "ERROR: wave_agent_cli binary not found: $WAVE_CLI_BIN"; exit 1; }

# Build the fixture if needed (no-op when up to date). To force a rebuild:
#   test_cases/make_fixture.sh --force
"$TEST_CASES_DIR/make_fixture.sh"

echo "Running tests..."
"$PYTHON" test_tc19_direction_ranking.py

echo "Test case completed."
