#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
EXPECTED_DIR="$SCRIPT_DIR/expected/stats"
TOOL=$(find_tool "edsparser-stats") || { echo "ERROR: edsparser-stats not found"; exit 1; }
VCF_TOOL=$(find_tool "vcf2eds") || { echo "ERROR: vcf2eds not found"; exit 1; }
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== edsparser-stats ==="

# Strip fields that vary per run or machine (paths, timing, memory usage).
normalize_json() {
    grep -v '"path"' | grep -v '"size_bytes"' | grep -v '"runtime_s"' \
    | grep -v '"peak_memory_mb"' | grep -v '"current_bytes"' \
    | grep -v '"current_mb"' | grep -v '"estimated_full_bytes"' \
    | grep -v '"estimated_full_mb"' | grep -v '"reduction_factor"' \
    | grep -v '"suggested_command"'
}

normalize_csv() {
    # Remove first column (absolute file path) and last two columns (runtime, peak_memory)
    cut -d',' -f2- | rev | cut -d',' -f3- | rev
}

test_basic_output() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/simple.eds")
    assert_exit_code 0 $? "exits 0 on valid EDS input" || return 1
    [ -n "$out" ] || { echo -e "  ${RED}FAIL${NC}: output is empty"; return 1; }
}

test_output_has_counts() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/simple.eds")
    assert_contains "$out" ":" "output contains key:value pairs" || return 1
}

test_json_output() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/simple.eds" -j)
    assert_exit_code 0 $? "exits 0 with -j flag" || return 1
    assert_contains "$out" "{" "JSON output contains opening brace" || return 1

    echo "$out" | normalize_json > "$TMPDIR/actual.json"
    normalize_json < "$EXPECTED_DIR/simple.json" > "$TMPDIR/expected.json"
    assert_file_equal "$TMPDIR/actual.json" "$TMPDIR/expected.json" "JSON structural fields match expected" || return 1
}

test_csv_output() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/simple.eds" -c)
    assert_exit_code 0 $? "exits 0 with -c flag" || return 1
    assert_contains "$out" "," "CSV output contains commas" || return 1

    echo "$out" | normalize_csv > "$TMPDIR/actual.csv"
    normalize_csv < "$EXPECTED_DIR/simple.csv" > "$TMPDIR/expected.csv"
    assert_file_equal "$TMPDIR/actual.csv" "$TMPDIR/expected.csv" "CSV structural fields match expected" || return 1
}

test_verbose_output() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/simple.eds" -v)
    assert_exit_code 0 $? "exits 0 with -v flag" || return 1
    [ -n "$out" ] || { echo -e "  ${RED}FAIL${NC}: verbose output is empty"; return 1; }
}

test_with_sources() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds")
    assert_exit_code 0 $? "exits 0 with sources file" || return 1
    [ -n "$out" ] || { echo -e "  ${RED}FAIL${NC}: output is empty with sources"; return 1; }
}

test_with_edz_sources() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/small.eds" -z "$DATA_DIR/small.edz")
    assert_exit_code 0 $? "exits 0 with -z EDZ sources file" || return 1
    [ -n "$out" ] || { echo -e "  ${RED}FAIL${NC}: output is empty with EDZ sources"; return 1; }
}

test_seds_and_edz_agree() {
    # Generate a fresh SEDS/EDZ pair from the same VCF+FASTA input (rather
    # than reusing the committed small.eds/small.seds/small.edz fixtures,
    # which can drift independently) so this test only measures whether the
    # two containers encode the same data, not fixture staleness.
    "$VCF_TOOL" -i "$DATA_DIR/small.vcf" -r "$DATA_DIR/small.fa" \
        -o "$TMPDIR/fresh.eds" -s "$TMPDIR/fresh.seds" >/dev/null 2>&1
    "$VCF_TOOL" -i "$DATA_DIR/small.vcf" -r "$DATA_DIR/small.fa" \
        -o "$TMPDIR/fresh_discard.eds" -s "$TMPDIR/fresh.edz" -z >/dev/null 2>&1
    # vcf2eds's EDS output is deterministic given the same input regardless of
    # source format, so both runs produce byte-identical .eds content; reuse
    # one filename for both stats calls so the "File:" line doesn't add noise.

    local out_seds out_edz
    out_seds=$("$TOOL" -i "$TMPDIR/fresh.eds" -s "$TMPDIR/fresh.seds" -v)
    out_edz=$("$TOOL" -i "$TMPDIR/fresh.eds" -z "$TMPDIR/fresh.edz" -v)
    if [ "$out_seds" != "$out_edz" ]; then
        echo -e "  ${RED}FAIL${NC}: SEDS and EDZ verbose stats differ"
        diff <(echo "$out_seds") <(echo "$out_edz") | head -10 | sed 's/^/    /'
        return 1
    fi
}

test_long_flag_forms() {
    "$TOOL" -i "$DATA_DIR/small.eds" --seds "$DATA_DIR/small.seds" >/dev/null
    assert_exit_code 0 $? "exits 0 with --seds long flag" || return 1
    "$TOOL" -i "$DATA_DIR/small.eds" --edz "$DATA_DIR/small.edz" >/dev/null
    assert_exit_code 0 $? "exits 0 with --edz long flag" || return 1
}

test_seds_and_edz_mutually_exclusive() {
    "$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" -z "$DATA_DIR/small.edz" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: -s and -z together — expected non-zero exit, got 0"; return 1; }
}

test_edz_without_extension() {
    # -z must force EDZ interpretation even when the file isn't named .edz.
    cp "$DATA_DIR/small.edz" "$TMPDIR/small_noext.dat"
    "$TOOL" -i "$DATA_DIR/small.eds" -z "$TMPDIR/small_noext.dat" >/dev/null
    assert_exit_code 0 $? "exits 0 with -z on EDZ file lacking .edz extension" || return 1
}

test_seds_autodetect_fails_on_misnamed_edz() {
    # A binary EDZ file misnamed .seds must fail rather than be silently
    # misparsed as text.
    cp "$DATA_DIR/small.edz" "$TMPDIR/small_wrong.seds"
    "$TOOL" -i "$DATA_DIR/small.eds" -s "$TMPDIR/small_wrong.seds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: -s on EDZ file misnamed .seds — expected non-zero exit, got 0"; return 1; }
}

test_multiple_eds_files() {
    local out
    out=$("$TOOL" -i "$DATA_DIR/test2.eds")
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
run_test "stats with EDZ sources (-z)"    test_with_edz_sources
run_test "SEDS and EDZ stats agree"       test_seds_and_edz_agree
run_test "--seds/--edz long flag forms"   test_long_flag_forms
run_test "-s and -z mutually exclusive"   test_seds_and_edz_mutually_exclusive
run_test "-z forces EDZ on misnamed file" test_edz_without_extension
run_test "-s fails on misnamed EDZ file"  test_seds_autodetect_fails_on_misnamed_edz
run_test "stats on test2.eds"             test_multiple_eds_files
run_test "missing input exits non-zero"   test_missing_input_fails

print_summary
