#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
EXPECTED_DIR="$SCRIPT_DIR/expected/eds2leds"
TOOL=$(find_tool "eds2leds") || { echo "ERROR: eds2leds not found"; exit 1; }
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== eds2leds ==="

test_basic_cartesian() {
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/out.leds" -l 3
    assert_exit_code 0 $? "exits 0 on simple EDS without sources" || return 1
    assert_file_exists "$TMPDIR/out.leds" ".leds output created" || return 1
    assert_not_empty "$TMPDIR/out.leds" ".leds output not empty" || return 1
    assert_file_equal "$TMPDIR/out.leds" "$EXPECTED_DIR/simple.l3.leds" "l-EDS output matches expected" || return 1
}

test_linear_with_sources() {
    "$TOOL" \
        -i "$DATA_DIR/small.eds" \
        -s "$DATA_DIR/small.seds" \
        -o "$TMPDIR/linear.leds" \
        -l 3
    assert_exit_code 0 $? "exits 0 with sources (linear merge)" || return 1
    assert_file_exists "$TMPDIR/linear.leds" ".leds output created for linear merge" || return 1
    assert_not_empty "$TMPDIR/linear.leds" ".leds output not empty for linear merge" || return 1
    assert_file_equal "$TMPDIR/linear.leds" "$EXPECTED_DIR/small.linear.l3.leds" "linear l-EDS output matches expected" || return 1
}

test_larger_context_length() {
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/l5.leds" -l 5
    assert_exit_code 0 $? "exits 0 with -l 5" || return 1
    assert_file_exists "$TMPDIR/l5.leds" ".leds output created with l=5" || return 1
    assert_file_equal "$TMPDIR/l5.leds" "$EXPECTED_DIR/simple.l5.leds" "l=5 output matches expected" || return 1
}

test_full_output_format() {
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/full.leds" -l 3 --full
    assert_exit_code 0 $? "exits 0 with --full flag" || return 1
    assert_file_exists "$TMPDIR/full.leds" ".leds output created in full format" || return 1
    assert_file_equal "$TMPDIR/full.leds" "$EXPECTED_DIR/simple.l3.full.leds" "full-format output matches expected" || return 1
}

test_iterative_merging() {
    # EDS where every common region between degenerate symbols is shorter than l,
    # forcing the algorithm to merge symbols across multiple passes.
    "$TOOL" -i "$DATA_DIR/test_iterative.eds" -o "$TMPDIR/iter.leds" -l 3
    assert_exit_code 0 $? "exits 0 on multi-pass merge input" || return 1
    assert_file_exists "$TMPDIR/iter.leds" ".leds output created" || return 1
    assert_file_equal "$TMPDIR/iter.leds" "$EXPECTED_DIR/test_iterative.l3.leds" "multi-pass merge output matches expected" || return 1
}

test_compact_input_format() {
    # eds2leds must accept compact EDS input (no brackets around common regions).
    "$TOOL" -i "$DATA_DIR/test_compact_input.eds" -o "$TMPDIR/compact.leds" -l 3
    assert_exit_code 0 $? "exits 0 on compact-format EDS input" || return 1
    assert_file_exists "$TMPDIR/compact.leds" ".leds output created" || return 1
    assert_file_equal "$TMPDIR/compact.leds" "$EXPECTED_DIR/test_compact_input.l3.leds" "compact-format input output matches expected" || return 1
}

test_iterative_linear_merging() {
    # EDS where all common regions are shorter than l, using linear (source-aware) merge.
    # 2 paths: path 1 always takes A alternatives, path 2 always takes T alternatives.
    # Forces 3 merge iterations; result must differ from cartesian (2 strings vs 8).
    "$TOOL" \
        -i "$DATA_DIR/test_iterative_linear.eds" \
        -s "$DATA_DIR/test_iterative_linear.seds" \
        -o "$TMPDIR/iter_linear.leds" \
        -l 3
    assert_exit_code 0 $? "exits 0 on multi-pass linear merge input" || return 1
    assert_file_exists "$TMPDIR/iter_linear.leds" ".leds output created" || return 1
    assert_file_equal "$TMPDIR/iter_linear.leds" "$EXPECTED_DIR/test_iterative_linear.l3.leds" "multi-pass linear merge output matches expected" || return 1
    assert_file_equal "$TMPDIR/iter_linear.seds" "$EXPECTED_DIR/test_iterative_linear.l3.seds" "multi-pass linear seds matches expected" || return 1
}

test_missing_context_length_fails() {
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/x.leds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing -l — expected non-zero exit, got 0"; return 1; }
}

test_missing_input_fails() {
    "$TOOL" -i "$TMPDIR/nonexistent.eds" -o "$TMPDIR/x.leds" -l 3 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing input — expected non-zero exit, got 0"; return 1; }
}

run_test "basic EDS→l-EDS (cartesian)"          test_basic_cartesian
run_test "EDS→l-EDS with sources (linear)"      test_linear_with_sources
run_test "larger context length (-l 5)"         test_larger_context_length
run_test "full output format (--full)"          test_full_output_format
run_test "multi-pass iterative merging"          test_iterative_merging
run_test "compact EDS format as input"          test_compact_input_format
run_test "multi-pass iterative merging (linear)" test_iterative_linear_merging
run_test "missing -l exits non-zero"            test_missing_context_length_fails
run_test "missing input exits non-zero"         test_missing_input_fails

print_summary
