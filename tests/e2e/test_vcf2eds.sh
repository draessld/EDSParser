#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
EXPECTED_DIR="$SCRIPT_DIR/expected/vcf2eds"
TOOL=$(find_tool "vcf2eds") || { echo "ERROR: vcf2eds not found"; exit 1; }
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

VCF="$DATA_DIR/small.vcf"
REF="$DATA_DIR/small.fa"

echo "=== vcf2eds ==="

test_basic_conversion() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/out.eds" -s "$TMPDIR/out.seds"
    assert_exit_code 0 $? "exits 0 on valid VCF+reference" || return 1
    assert_file_exists "$TMPDIR/out.eds" ".eds output file created" || return 1
    assert_not_empty "$TMPDIR/out.eds" ".eds output not empty" || return 1
    assert_file_equal "$TMPDIR/out.eds" "$EXPECTED_DIR/small.eds" "EDS output matches expected" || return 1
}

test_sources_file_created() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/src.eds" -s "$TMPDIR/src.seds"
    assert_exit_code 0 $? "exits 0" || return 1
    assert_file_exists "$TMPDIR/src.seds" ".seds source file created" || return 1
    assert_not_empty "$TMPDIR/src.seds" ".seds source file not empty" || return 1
    assert_file_equal "$TMPDIR/src.seds" "$EXPECTED_DIR/small.seds" "SEDS output matches expected" || return 1
}

test_eds_format_valid() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/fmt.eds"
    local content
    content=$(cat "$TMPDIR/fmt.eds")
    assert_contains "$content" "{" "output contains degenerate symbol brackets" || return 1
}

test_leds_with_context_length() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/out.leds" -l 5
    assert_exit_code 0 $? "exits 0 with -l 5" || return 1
    assert_file_exists "$TMPDIR/out.leds" ".leds output created with context length" || return 1
    assert_not_empty "$TMPDIR/out.leds" ".leds output not empty" || return 1
    assert_file_equal "$TMPDIR/out.leds" "$EXPECTED_DIR/small.l5.leds" "l-EDS output matches expected" || return 1
}

test_overlapping_variants() {
    local vcf="$DATA_DIR/test_overlaps.vcf"
    local ref="$DATA_DIR/test_overlaps.fa"
    "$TOOL" -i "$vcf" -r "$ref" -o "$TMPDIR/overlaps.eds"
    assert_exit_code 0 $? "exits 0 on overlapping variants VCF" || return 1
    assert_file_exists "$TMPDIR/overlaps.eds" ".eds output created for overlapping variants" || return 1
    assert_file_equal "$TMPDIR/overlaps.eds" "$EXPECTED_DIR/test_overlaps.eds" "overlaps EDS output matches expected" || return 1
}

test_missing_reference_fails() {
    "$TOOL" -i "$VCF" -r "$TMPDIR/nonexistent.fa" -o "$TMPDIR/x.eds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing ref — expected non-zero exit, got 0"; return 1; }
}

test_missing_input_fails() {
    "$TOOL" -i "$TMPDIR/nonexistent.vcf" -r "$REF" -o "$TMPDIR/x.eds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing input — expected non-zero exit, got 0"; return 1; }
}

test_no_args_fails() {
    "$TOOL" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: no-args — expected non-zero exit, got 0"; return 1; }
}

run_test "basic VCF→EDS conversion"         test_basic_conversion
run_test "sources file created"             test_sources_file_created
run_test "output contains EDS brackets"    test_eds_format_valid
run_test "VCF→l-EDS with -l 5"             test_leds_with_context_length
run_test "overlapping variants VCF"        test_overlapping_variants
run_test "missing reference exits non-zero" test_missing_reference_fails
run_test "missing input exits non-zero"     test_missing_input_fails
run_test "no args exits non-zero"           test_no_args_fails

print_summary
