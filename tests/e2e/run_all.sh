#!/bin/bash
# Run all EDSParser e2e tests.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

SUITES=(
    test_msa2eds.sh
    test_vcf2eds.sh
    test_eds2leds.sh
    test_stats.sh
    test_genpatterns.sh
    test_genrandomeds.sh
)

SUITES_PASSED=0
SUITES_FAILED=0

echo -e "${BLUE}==============================${NC}"
echo -e "${BLUE}EDSParser e2e tests${NC}"
echo -e "${BLUE}==============================${NC}"
echo ""

for suite in "${SUITES[@]}"; do
    bash "$SCRIPT_DIR/$suite"
    if [ $? -eq 0 ]; then
        SUITES_PASSED=$((SUITES_PASSED + 1))
    else
        SUITES_FAILED=$((SUITES_FAILED + 1))
    fi
    echo ""
done

echo -e "${BLUE}==============================${NC}"
TOTAL=$((SUITES_PASSED + SUITES_FAILED))
if [ $SUITES_FAILED -eq 0 ]; then
    echo -e "${GREEN}All $TOTAL suites passed${NC}"
    exit 0
else
    echo -e "${RED}$SUITES_FAILED/$TOTAL suites had failures${NC}"
    exit 1
fi
