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
    local n=5 l=4   # simple.eds holds 7 distinct 4-mers; 5 is within reach
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
    # simple.eds is {ACGT}{A,ACA}{CGT}{T,TG} and holds only 7 distinct 4-mers, so
    # ask for a number it can actually supply. -n 10 is covered below, where the
    # shortfall is the thing being tested.
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/count.txt" -n 5 -l 4
    assert_exit_code 0 $? "exits 0" || return 1
    assert_line_count "$TMPDIR/count.txt" 5 "generates exactly 5 patterns" || return 1
}

test_patterns_are_distinct() {
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/distinct.txt" -n 10 -l 4
    assert_exit_code 0 $? "exits 0" || return 1
    local total unique
    total=$(wc -l < "$TMPDIR/distinct.txt")
    unique=$(sort -u "$TMPDIR/distinct.txt" | wc -l)
    [ "$total" -eq "$unique" ] || {
        echo -e "  ${RED}FAIL${NC}: patterns not distinct — $total lines, $unique unique"
        return 1
    }
}

test_shortfall_is_reported() {
    # Asking for more distinct patterns than the EDS contains must warn and
    # deliver fewer, never pad the set with repeats.
    local out
    out=$("$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/short.txt" -n 100 -l 4 2>&1)
    assert_exit_code 0 $? "exits 0" || return 1
    echo "$out" | grep -qi "generated .* of .* requested" || {
        echo -e "  ${RED}FAIL${NC}: expected a shortfall warning, got: $out"
        return 1
    }
    local total
    total=$(wc -l < "$TMPDIR/short.txt")
    [ "$total" -lt 100 ] || {
        echo -e "  ${RED}FAIL${NC}: expected fewer than 100 patterns, got $total"
        return 1
    }
}

test_allow_duplicates_pads_the_set() {
    "$TOOL" -i "$DATA_DIR/simple.eds" -o "$TMPDIR/dups.txt" -n 10 -l 4 --allow-duplicates
    assert_exit_code 0 $? "exits 0 with --allow-duplicates" || return 1
    assert_line_count "$TMPDIR/dups.txt" 10 "keeps all 10 requested patterns" || return 1
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
run_test "correct line count (-n 5)"         test_correct_line_count
run_test "patterns are distinct"             test_patterns_are_distinct
run_test "shortfall is reported"             test_shortfall_is_reported
run_test "--allow-duplicates keeps repeats"  test_allow_duplicates_pads_the_set
run_test "patterns use ACGT alphabet"        test_patterns_use_acgt_alphabet
run_test "default count and length"          test_default_count
run_test "single pattern (-n 1)"             test_single_pattern
run_test "missing -o exits non-zero"         test_missing_output_fails
run_test "missing input exits non-zero"      test_missing_input_fails

print_summary
