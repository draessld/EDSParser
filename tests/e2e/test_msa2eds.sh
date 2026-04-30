#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
EXPECTED_DIR="$SCRIPT_DIR/expected/msa2eds"
TOOL=$(find_tool "msa2eds") || { echo "ERROR: msa2eds not found"; exit 1; }
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== msa2eds ==="

test_basic_conversion() {
    "$TOOL" -i "$DATA_DIR/small.msa" -o "$TMPDIR/out.eds" -s "$TMPDIR/out.seds"
    assert_exit_code 0 $? "exits 0 on valid MSA input" || return 1
    assert_file_exists "$TMPDIR/out.eds" ".eds output file created" || return 1
    assert_not_empty "$TMPDIR/out.eds" ".eds output file not empty" || return 1
    assert_file_equal "$TMPDIR/out.eds" "$EXPECTED_DIR/small.eds" "EDS output matches expected" || return 1
}

test_sources_file_created() {
    "$TOOL" -i "$DATA_DIR/small.msa" -o "$TMPDIR/src.eds" -s "$TMPDIR/src.seds"
    assert_exit_code 0 $? "exits 0" || return 1
    assert_file_exists "$TMPDIR/src.seds" ".seds source file created" || return 1
    assert_not_empty "$TMPDIR/src.seds" ".seds source file not empty" || return 1
    assert_file_equal "$TMPDIR/src.seds" "$EXPECTED_DIR/small.seds" "SEDS output matches expected" || return 1
}

test_eds_format_valid() {
    "$TOOL" -i "$DATA_DIR/small.msa" -o "$TMPDIR/fmt.eds"
    local content
    content=$(cat "$TMPDIR/fmt.eds")
    assert_contains "$content" "{" "output contains degenerate symbol brackets" || return 1
}

test_leds_with_context_length() {
    "$TOOL" -i "$DATA_DIR/small.msa" -o "$TMPDIR/out.leds" -l 3
    assert_exit_code 0 $? "exits 0 with -l 3" || return 1
    assert_file_exists "$TMPDIR/out.leds" ".leds output created with context length" || return 1
    assert_not_empty "$TMPDIR/out.leds" ".leds output not empty" || return 1
    assert_file_equal "$TMPDIR/out.leds" "$EXPECTED_DIR/small.l3.leds" "l-EDS output matches expected" || return 1
}

test_default_output_path() {
    local input="$TMPDIR/small.msa"
    cp "$DATA_DIR/small.msa" "$input"
    "$TOOL" -i "$input"
    assert_exit_code 0 $? "exits 0 with default output path" || return 1
    assert_file_exists "${input%.msa}.eds" "default output .eds created next to input" || return 1
    assert_file_equal "${input%.msa}.eds" "$EXPECTED_DIR/small.eds" "default output EDS matches expected" || return 1
}

test_missing_input_fails() {
    "$TOOL" -i "$TMPDIR/nonexistent.msa" -o "$TMPDIR/x.eds" 2>/dev/null
    local code=$?
    assert_exit_code 1 $code "exits non-zero on missing input file" || return 1
}

test_no_args_fails() {
    "$TOOL" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: no-args — expected non-zero exit, got 0"; return 1; }
}

run_test "basic MSA→EDS conversion"       test_basic_conversion
run_test "sources file created"           test_sources_file_created
run_test "output contains EDS brackets"  test_eds_format_valid
run_test "MSA→l-EDS with -l 3"           test_leds_with_context_length
run_test "default output path"            test_default_output_path
run_test "missing input exits non-zero"   test_missing_input_fails
run_test "no args exits non-zero"         test_no_args_fails

print_summary
