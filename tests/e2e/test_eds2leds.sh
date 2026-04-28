#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
TOOL=$(find_tool "eds2leds") || { echo "ERROR: eds2leds not found"; exit 1; }
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== eds2leds ==="

test_basic_cartesian() {
    "$TOOL" -i "$DATA_DIR/eds/simple.eds" -o "$TMPDIR/out.leds" -l 3
    assert_exit_code 0 $? "exits 0 on simple EDS without sources" || return 1
    assert_file_exists "$TMPDIR/out.leds" ".leds output created" || return 1
    assert_not_empty "$TMPDIR/out.leds" ".leds output not empty" || return 1
}

test_output_contains_brackets() {
    "$TOOL" -i "$DATA_DIR/eds/simple.eds" -o "$TMPDIR/fmt.leds" -l 3
    local content
    content=$(cat "$TMPDIR/fmt.leds")
    assert_contains "$content" "{" "l-EDS output contains degenerate symbol brackets" || return 1
}

test_linear_with_sources() {
    "$TOOL" \
        -i "$DATA_DIR/vcf/small.eds" \
        -s "$DATA_DIR/vcf/small.seds" \
        -o "$TMPDIR/linear.leds" \
        -l 3
    assert_exit_code 0 $? "exits 0 with sources (linear merge)" || return 1
    assert_file_exists "$TMPDIR/linear.leds" ".leds output created for linear merge" || return 1
    assert_not_empty "$TMPDIR/linear.leds" ".leds output not empty for linear merge" || return 1
}

test_larger_context_length() {
    "$TOOL" -i "$DATA_DIR/eds/simple.eds" -o "$TMPDIR/l5.leds" -l 5
    assert_exit_code 0 $? "exits 0 with -l 5" || return 1
    assert_file_exists "$TMPDIR/l5.leds" ".leds output created with l=5" || return 1
}

test_full_output_format() {
    "$TOOL" -i "$DATA_DIR/eds/simple.eds" -o "$TMPDIR/full.leds" -l 3 --full
    assert_exit_code 0 $? "exits 0 with --full flag" || return 1
    assert_file_exists "$TMPDIR/full.leds" ".leds output created in full format" || return 1
}

test_iterative_eds() {
    "$TOOL" -i "$DATA_DIR/eds/test_iterative.eds" -o "$TMPDIR/iter.leds" -l 3
    assert_exit_code 0 $? "exits 0 on iterative EDS input" || return 1
    assert_file_exists "$TMPDIR/iter.leds" ".leds output created for iterative EDS" || return 1
}

test_missing_context_length_fails() {
    "$TOOL" -i "$DATA_DIR/eds/simple.eds" -o "$TMPDIR/x.leds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing -l — expected non-zero exit, got 0"; return 1; }
}

test_missing_input_fails() {
    "$TOOL" -i "$TMPDIR/nonexistent.eds" -o "$TMPDIR/x.leds" -l 3 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing input — expected non-zero exit, got 0"; return 1; }
}

run_test "basic EDS→l-EDS (cartesian)"          test_basic_cartesian
run_test "output contains EDS brackets"         test_output_contains_brackets
run_test "EDS→l-EDS with sources (linear)"      test_linear_with_sources
run_test "larger context length (-l 5)"         test_larger_context_length
run_test "full output format (--full)"          test_full_output_format
run_test "iterative EDS input"                  test_iterative_eds
run_test "missing -l exits non-zero"            test_missing_context_length_fails
run_test "missing input exits non-zero"         test_missing_input_fails

print_summary
