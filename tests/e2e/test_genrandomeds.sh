#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

TOOL=$(find_tool "genrandomeds") || { echo "ERROR: genrandomeds not found"; exit 1; }
STATS=$(find_tool "edsparser-stats") || { echo "ERROR: edsparser-stats not found"; exit 1; }
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== genrandomeds ==="

test_basic_generation() {
    "$TOOL" --ref-size-mb 1 -o "$TMPDIR/basic.eds" --seed 42 2>/dev/null
    assert_exit_code 0 $? "exits 0 on valid arguments" || return 1
    assert_file_exists "$TMPDIR/basic.eds" ".eds output created" || return 1
    assert_not_empty "$TMPDIR/basic.eds" ".eds output not empty" || return 1
}

test_seds_created_alongside() {
    "$TOOL" --ref-size-mb 1 -o "$TMPDIR/paired.eds" --seed 42 2>/dev/null
    assert_exit_code 0 $? "exits 0" || return 1
    assert_file_exists "$TMPDIR/paired.seds" ".seds created automatically alongside .eds" || return 1
    assert_not_empty "$TMPDIR/paired.seds" ".seds not empty" || return 1
}

test_reproducible_with_seed() {
    "$TOOL" --ref-size-mb 1 -o "$TMPDIR/repro1.eds" --seed 99 2>/dev/null
    "$TOOL" --ref-size-mb 1 -o "$TMPDIR/repro2.eds" --seed 99 2>/dev/null
    if ! diff -q "$TMPDIR/repro1.eds" "$TMPDIR/repro2.eds" >/dev/null 2>&1; then
        echo -e "  ${RED}FAIL${NC}: reproducible seed — two runs with --seed 99 produced different output"
        return 1
    fi
    if ! diff -q "$TMPDIR/repro1.seds" "$TMPDIR/repro2.seds" >/dev/null 2>&1; then
        echo -e "  ${RED}FAIL${NC}: reproducible seed — .seds files differ between runs"
        return 1
    fi
}

test_different_seeds_differ() {
    "$TOOL" --ref-size-mb 1 -o "$TMPDIR/seed1.eds" --seed 1 2>/dev/null
    "$TOOL" --ref-size-mb 1 -o "$TMPDIR/seed2.eds" --seed 2 2>/dev/null
    if diff -q "$TMPDIR/seed1.eds" "$TMPDIR/seed2.eds" >/dev/null 2>&1; then
        echo -e "  ${RED}FAIL${NC}: different seeds — outputs were identical (expected different)"
        return 1
    fi
}

test_zero_variability_no_degenerate() {
    "$TOOL" --ref-size-mb 1 -o "$TMPDIR/novar.eds" --seed 42 --variability 0 2>/dev/null
    assert_exit_code 0 $? "exits 0 with --variability 0" || return 1
    local degen
    degen=$("$STATS" -i "$TMPDIR/novar.eds" --json 2>/dev/null \
        | grep -oP '"degenerate_symbols":\s*\K[0-9]+')
    if [ "$degen" -ne 0 ]; then
        echo -e "  ${RED}FAIL${NC}: --variability 0 — expected 0 degenerate symbols, got $degen"
        return 1
    fi
}

test_min_context_leds_compliant() {
    local l=5
    "$TOOL" --ref-size-mb 1 -o "$TMPDIR/leds.eds" --seed 42 --variability 0.01 --min-context $l 2>/dev/null
    assert_exit_code 0 $? "exits 0 with --min-context $l" || return 1
    local min_ctx
    min_ctx=$("$STATS" -i "$TMPDIR/leds.eds" --json 2>/dev/null \
        | grep -oP '"min_context_length":\s*\K[0-9]+')
    if [ "$min_ctx" -lt $l ]; then
        echo -e "  ${RED}FAIL${NC}: --min-context $l — min context in output is $min_ctx (expected ≥ $l)"
        return 1
    fi
}

test_eds_acgt_alphabet() {
    "$TOOL" --ref-size-mb 1 -o "$TMPDIR/alpha.eds" --seed 42 2>/dev/null
    # Strip EDS structure characters, only ACGT should remain
    if tr -d 'ACGT{},\n' < "$TMPDIR/alpha.eds" | grep -q '.'; then
        echo -e "  ${RED}FAIL${NC}: output contains characters outside ACGT + EDS structure"
        return 1
    fi
}

test_missing_output_fails() {
    "$TOOL" --ref-size-mb 1 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing -o — expected non-zero exit, got 0"; return 1; }
}

test_missing_ref_size_fails() {
    "$TOOL" -o "$TMPDIR/x.eds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing --ref-size-mb — expected non-zero exit, got 0"; return 1; }
}

run_test "basic generation"                    test_basic_generation
run_test ".seds created automatically"         test_seds_created_alongside
run_test "reproducible with --seed"            test_reproducible_with_seed
run_test "different seeds produce different output" test_different_seeds_differ
run_test "--variability 0 → no degenerate symbols" test_zero_variability_no_degenerate
run_test "--min-context produces l-EDS compliant output" test_min_context_leds_compliant
run_test "output uses ACGT alphabet only"      test_eds_acgt_alphabet
run_test "missing -o exits non-zero"           test_missing_output_fails
run_test "missing --ref-size-mb exits non-zero" test_missing_ref_size_fails

print_summary