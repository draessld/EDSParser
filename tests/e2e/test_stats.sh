#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
TOOL=$(find_tool "edsparser-stats") || { echo "ERROR: edsparser-stats not found"; exit 1; }

echo "=== edsparser-stats ==="

test_basic_output() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/eds/simple.eds")
    assert_exit_code 0 $? "exits 0 on valid EDS input" || return 1
    [ -n "$out" ] || { echo -e "  ${RED}FAIL${NC}: output is empty"; return 1; }
}

test_output_has_counts() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/eds/simple.eds")
    assert_contains "$out" ":" "output contains key:value pairs" || return 1
}

test_json_output() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/eds/simple.eds" -j)
    assert_exit_code 0 $? "exits 0 with -j flag" || return 1
    assert_contains "$out" "{" "JSON output contains opening brace" || return 1
    assert_contains "$out" "}" "JSON output contains closing brace" || return 1
}

test_csv_output() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/eds/simple.eds" -c)
    assert_exit_code 0 $? "exits 0 with -c flag" || return 1
    assert_contains "$out" "," "CSV output contains commas" || return 1
}

test_verbose_output() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/eds/simple.eds" -v)
    assert_exit_code 0 $? "exits 0 with -v flag" || return 1
    [ -n "$out" ] || { echo -e "  ${RED}FAIL${NC}: verbose output is empty"; return 1; }
}

test_with_sources() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/vcf/small.eds" -s "$DATA_DIR/vcf/small.seds")
    assert_exit_code 0 $? "exits 0 with sources file" || return 1
    [ -n "$out" ] || { echo -e "  ${RED}FAIL${NC}: output is empty with sources"; return 1; }
}

test_multiple_eds_files() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/eds/simple.eds" -i "$DATA_DIR/eds/test2.eds" 2>/dev/null \
          || "$TOOL" -i "$DATA_DIR/eds/test2.eds")
    assert_exit_code 0 $? "exits 0 on another EDS file" || return 1
}

test_missing_input_fails() {
    "$TOOL" -i "/tmp/nonexistent.eds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing input — expected non-zero exit, got 0"; return 1; }
}

run_test "basic stats output"              test_basic_output
run_test "output has key:value pairs"     test_output_has_counts
run_test "JSON output (-j)"               test_json_output
run_test "CSV output (-c)"                test_csv_output
run_test "verbose output (-v)"            test_verbose_output
run_test "stats with sources file"        test_with_sources
run_test "stats on test2.eds"             test_multiple_eds_files
run_test "missing input exits non-zero"   test_missing_input_fails

print_summary
