#!/bin/bash
# Run all test cases in the test suite
# Usage: ./run_all_tests.sh

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
    
    if cd "$tc_name" && ./run_test.sh; then
        echo ""
        echo -e "${GREEN}✓ ${tc_name}: PASSED${NC}"
        PASSED=$((PASSED + 1))
    else
        echo ""
        echo -e "${RED}✗ ${tc_name}: FAILED${NC}"
        FAILED=$((FAILED + 1))
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
echo -e "Failed: ${RED}${FAILED}${NC}"
echo "=========================================="

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
fi
