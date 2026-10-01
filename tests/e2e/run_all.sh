#!/bin/bash
# Run all EDSParser e2e tests.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
NC='\033[0m'

SUITES=(
    test_msa2eds.sh
    test_vcf2eds.sh
    test_eds2leds.sh
    test_leds_incremental.sh
    test_source_transform.sh
    test_seds_edz.sh
    test_stats.sh
    test_genpatterns.sh
    test_genrandomeds.sh
)

SUITES_PASSED=0
SUITES_FAILED=0

# Each suite's print_summary appends "passed failed skipped" here, so skips are
# totalled and reported rather than disappearing into the pass count.
export EDSPARSER_E2E_TALLY
EDSPARSER_E2E_TALLY=$(mktemp)
trap 'rm -f "$EDSPARSER_E2E_TALLY"' EXIT

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
read -r T_PASS T_FAIL T_SKIP < <(awk '{p+=$1; f+=$2; s+=$3} END {print p+0, f+0, s+0}' "$EDSPARSER_E2E_TALLY")
echo "Tests: $T_PASS passed, $T_FAIL failed, $T_SKIP skipped"
[ "$T_SKIP" -gt 0 ] && echo -e "${YELLOW}$T_SKIP test(s) SKIPPED — not verified by this run${NC}"
if [ $SUITES_FAILED -eq 0 ]; then
    echo -e "${GREEN}All $TOTAL suites passed${NC}"
    exit 0
else
    echo -e "${RED}$SUITES_FAILED/$TOTAL suites had failures${NC}"
    exit 1
fi
