#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
TOOL=$(find_tool "edsparser-genpatterns") || { echo "ERROR: edsparser-genpatterns not found"; exit 1; }
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== edsparser-genpatterns ==="

test_basic_generation() {
    local n=5 l=4
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/patterns.txt" -n $n -l $l
    assert_exit_code 0 $? "exits 0 on valid EDS input" || return 1
    assert_file_exists "$TMPDIR/patterns.txt" "output patterns file created" || return 1
    assert_line_count "$TMPDIR/patterns.txt" $n "generates exactly $n patterns" || return 1
    # Every pattern must be exactly l characters long
    while IFS= read -r line; do
        if [ "${#line}" -ne $l ]; then
            echo -e "  ${RED}FAIL${NC}: pattern length — expected $l, got ${#line} (\"$line\")"
            return 1
        fi
    done < "$TMPDIR/patterns.txt"
}

test_correct_line_count() {
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/count.txt" -n 10 -l 4
    assert_exit_code 0 $? "exits 0" || return 1
    assert_line_count "$TMPDIR/count.txt" 10 "generates exactly 10 patterns" || return 1
}

test_patterns_use_acgt_alphabet() {
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/alpha.txt" -n 5 -l 4
    # Patterns should only contain ACGT characters
    if grep -qP '[^ACGT\n]' "$TMPDIR/alpha.txt" 2>/dev/null; then
        echo -e "  ${RED}FAIL${NC}: patterns use non-ACGT characters"
        return 1
    fi
}

test_default_count() {
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/default.txt"
    assert_exit_code 0 $? "exits 0 with default count" || return 1
    assert_file_exists "$TMPDIR/default.txt" "output file created with defaults" || return 1
    assert_not_empty "$TMPDIR/default.txt" "output file not empty with defaults" || return 1
}

test_single_pattern() {
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/single.txt" -n 1 -l 4
    assert_exit_code 0 $? "exits 0 with -n 1" || return 1
    assert_line_count "$TMPDIR/single.txt" 1 "generates exactly 1 pattern" || return 1
}

test_missing_output_fails() {
    "$TOOL" -i "$DATA_DIR/simple.eds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing -o — expected non-zero exit, got 0"; return 1; }
}

test_missing_input_fails() {
    "$TOOL" -i "$TMPDIR/nonexistent.eds" -o "$TMPDIR/x.txt" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing input — expected non-zero exit, got 0"; return 1; }
}

run_test "basic pattern generation"           test_basic_generation
run_test "correct line count (-n 10)"        test_correct_line_count
run_test "patterns use ACGT alphabet"        test_patterns_use_acgt_alphabet
run_test "default count and length"          test_default_count
run_test "single pattern (-n 1)"             test_single_pattern
run_test "missing -o exits non-zero"         test_missing_output_fails
run_test "missing input exits non-zero"      test_missing_input_fails

print_summary
