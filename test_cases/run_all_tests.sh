#!/bin/bash
# Run all test cases in the test suite
# Usage: ./run_all_tests.sh
#
# Exit status: 0 if every test passed (or was explicitly skipped), 1 if any
# test failed. A test's run_test.sh may exit with status 77 to report SKIPPED
# (distinct from failure).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Counters
TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0
FAILED_NAMES=()

echo "=========================================="
echo "  Auto Waveform Debugger - Test Suite"
echo "=========================================="
echo ""

# Get all test case directories
TEST_DIRS=( $(ls -d tc*/ 2>/dev/null | sort -V) )

if [ ${#TEST_DIRS[@]} -eq 0 ]; then
    echo -e "${RED}Error: No test case directories found${NC}"
    exit 1
fi

echo "Found ${#TEST_DIRS[@]} test cases"
echo ""

# Run each test case
for tc_dir in "${TEST_DIRS[@]}"; do
    tc_name="${tc_dir%/}"
    TOTAL=$((TOTAL + 1))
    
    echo "=========================================="
    echo -e "${YELLOW}[${TOTAL}] Running: ${tc_name}${NC}"
    echo "=========================================="
    
    rc=0
    (cd "$tc_name" && ./run_test.sh) || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo ""
        echo -e "${GREEN}✓ ${tc_name}: PASSED${NC}"
        PASSED=$((PASSED + 1))
    elif [ "$rc" -eq 77 ]; then
        echo ""
        echo -e "${YELLOW}- ${tc_name}: SKIPPED${NC}"
        SKIPPED=$((SKIPPED + 1))
    else
        echo ""
        echo -e "${RED}✗ ${tc_name}: FAILED (exit code ${rc})${NC}"
        FAILED=$((FAILED + 1))
        FAILED_NAMES+=("$tc_name")
    fi

    cd "$SCRIPT_DIR"
    echo ""
done

# Summary
echo "=========================================="
echo "  Test Summary"
echo "=========================================="
echo -e "Total:  ${TOTAL}"
echo -e "Passed: ${GREEN}${PASSED}${NC}"
echo -e "Skipped: ${YELLOW}${SKIPPED}${NC}"
echo -e "Failed: ${RED}${FAILED}${NC}"
if [ "$FAILED" -gt 0 ]; then
    echo "Failed tests: ${FAILED_NAMES[*]}"
fi
echo "=========================================="

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
