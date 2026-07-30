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

test_linear_with_edz_sources() {
    "$TOOL" \
        -i "$DATA_DIR/small.eds" \
        --edz "$DATA_DIR/small.edz" \
        -o "$TMPDIR/linear_edz.leds" \
        -l 3
    assert_exit_code 0 $? "exits 0 with --edz sources (linear merge)" || return 1
    assert_file_exists "$TMPDIR/linear_edz.leds" ".leds output created for EDZ linear merge" || return 1
    assert_not_empty "$TMPDIR/linear_edz.leds" ".leds output not empty for EDZ linear merge" || return 1
}

test_seds_and_edz_sources_agree() {
    # Same underlying source data in two different containers must produce
    # byte-identical l-EDS output.
    "$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" -o "$TMPDIR/cmp_seds.leds" -l 3 >/dev/null 2>&1
    "$TOOL" -i "$DATA_DIR/small.eds" -z "$DATA_DIR/small.edz"  -o "$TMPDIR/cmp_edz.leds"  -l 3 >/dev/null 2>&1
    assert_file_equal "$TMPDIR/cmp_seds.leds" "$TMPDIR/cmp_edz.leds" \
        "SEDS and EDZ sources produce identical l-EDS output"
}

test_long_flag_forms() {
    "$TOOL" -i "$DATA_DIR/small.eds" --seds "$DATA_DIR/small.seds" -o "$TMPDIR/longflag_seds.leds" -l 3
    assert_exit_code 0 $? "exits 0 with --seds long flag" || return 1
    "$TOOL" -i "$DATA_DIR/small.eds" --edz "$DATA_DIR/small.edz" -o "$TMPDIR/longflag_edz.leds" -l 3
    assert_exit_code 0 $? "exits 0 with --edz long flag" || return 1
}

test_seds_and_edz_mutually_exclusive() {
    "$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" -z "$DATA_DIR/small.edz" \
        -o "$TMPDIR/mutex.leds" -l 3 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: -s and -z together — expected non-zero exit, got 0"; return 1; }
}

test_edz_without_extension() {
    # -z must force EDZ interpretation even when the file isn't named .edz
    # (detect_format() dispatches purely on extension otherwise).
    cp "$DATA_DIR/small.edz" "$TMPDIR/small_noext.dat"
    "$TOOL" -i "$DATA_DIR/small.eds" -z "$TMPDIR/small_noext.dat" -o "$TMPDIR/noext.leds" -l 3
    assert_exit_code 0 $? "exits 0 with -z on EDZ file lacking .edz extension" || return 1
    "$TOOL" -i "$DATA_DIR/small.eds" -z "$DATA_DIR/small.edz" -o "$TMPDIR/withext.leds" -l 3 >/dev/null 2>&1
    assert_file_equal "$TMPDIR/noext.leds" "$TMPDIR/withext.leds" \
        "-z on misnamed EDZ file matches .edz-named EDZ output" || return 1
}

test_seds_autodetect_fails_on_misnamed_edz() {
    # Documents a known limitation: -s auto-detects format purely from
    # extension/content sniffing, which only recognizes .edz explicitly.
    # A binary EDZ file misnamed .seds is misread as text and must fail
    # rather than silently corrupt.
    cp "$DATA_DIR/small.edz" "$TMPDIR/small_wrong.seds"
    "$TOOL" -i "$DATA_DIR/small.eds" -s "$TMPDIR/small_wrong.seds" -o "$TMPDIR/wrong.leds" -l 3 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: -s on EDZ file misnamed .seds — expected non-zero exit, got 0"; return 1; }
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

test_raw_copy_passthrough_roundtrip() {
    # Golden-independent validation of the l-EDS merge pass-through raw byte-copy
    # (EDS::copy_symbol_range_to_stream) for unmodified full-bracket symbols:
    #   1. the l-EDS output must reload cleanly (proves the raw-copied temp files
    #      are valid, seekable full-bracket EDS);
    #   2. re-transforming the already-l-compliant output at the same l must
    #      converge in 0 iterations (idempotent), proving the pass-through
    #      preserved structure byte-for-byte.
    local GEN STAT
    GEN=$(find_tool "genrandomeds")     || { echo "  SKIP: genrandomeds not found"; return 0; }
    STAT=$(find_tool "edsparser-stats") || { echo "  SKIP: edsparser-stats not found"; return 0; }

    "$GEN" --ref-size-mb 1 -v 0.05 --min-context 0 --seed 123 -o "$TMPDIR/rc.eds" >/dev/null 2>&1
    assert_file_exists "$TMPDIR/rc.eds" "generated input EDS" || return 1

    "$TOOL" -i "$TMPDIR/rc.eds" -s "$TMPDIR/rc.seds" -o "$TMPDIR/rc.leds" -l 4 >/dev/null 2>&1
    assert_exit_code 0 $? "eds2leds (linear) exits 0 on generated input" || return 1
    assert_not_empty "$TMPDIR/rc.leds" "l-EDS output not empty" || return 1

    "$STAT" -i "$TMPDIR/rc.leds" >/dev/null 2>&1
    assert_exit_code 0 $? "stats reloads the l-EDS output without error" || return 1

    # eds2leds requires a .eds extension; copy before the idempotency re-run.
    cp "$TMPDIR/rc.leds" "$TMPDIR/rc_reinput.eds"
    local out
    out=$("$TOOL" -i "$TMPDIR/rc_reinput.eds" -o "$TMPDIR/rc2.leds" -l 4 2>&1)
    assert_exit_code 0 $? "re-transform of l-compliant output exits 0" || return 1
    echo "$out" | grep -q "Converged after 0 iterations" \
        || { echo -e "  ${RED}FAIL${NC}: expected 0-iteration convergence on l-compliant input"; return 1; }
    echo -e "  ${GREEN}PASS${NC}: raw-copy pass-through yields valid, idempotent l-EDS"
}

# --max-memory pre-flight guard: a generous budget must not block the transform.
test_max_memory_allows_under_budget() {
    "$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" \
        -o "$TMPDIR/mm_ok.leds" -l 3 --max-memory 450G >/dev/null 2>&1
    assert_exit_code 0 $? "exits 0 when estimate is under budget" || return 1
    assert_file_equal "$TMPDIR/mm_ok.leds" "$EXPECTED_DIR/small.linear.l3.leds" \
        "output unchanged by the (passing) memory guard" || return 1
}

# A tiny budget must refuse the transform with the dedicated exit code 3 and NOT
# write output.
test_max_memory_refuses_over_budget() {
    rm -f "$TMPDIR/mm_no.leds"
    local out
    out=$("$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" \
        -o "$TMPDIR/mm_no.leds" -l 10 --max-memory 1 2>&1)
    assert_exit_code 3 $? "exits 3 (memory-exceeded) on a tiny budget" || return 1
    assert_contains "$out" "exceeds --max-memory" "prints the over-budget reason" || return 1
    [ ! -s "$TMPDIR/mm_no.leds" ] || { echo -e "  ${RED}FAIL${NC}: refused run must not write output"; return 1; }
}

# Same input, cartesian (no sources) vs linear (sources): the cartesian bound is
# the product of cardinalities and is vastly larger, so it must be refused where
# the linear (num_paths-capped) estimate is not.
test_max_memory_cartesian_vs_linear() {
    local GEN; GEN=$(find_tool "genrandomeds") || { echo "  SKIP: genrandomeds not found"; return 0; }
    "$GEN" --ref-size-mb 1 -v 0.1 --min-context 0 --seed 7 -o "$TMPDIR/mm.eds" >/dev/null 2>&1
    assert_file_exists "$TMPDIR/mm.eds" "generated dense EDS" || return 1
    # linear: num_paths cap keeps it small → passes a modest budget
    "$TOOL" -i "$TMPDIR/mm.eds" -s "$TMPDIR/mm.seds" -o "$TMPDIR/mm_lin.leds" -l 20 --max-memory 100G >/dev/null 2>&1
    assert_exit_code 0 $? "linear estimate passes 100G" || return 1
    # cartesian: product blows past the same budget → refused
    "$TOOL" -i "$TMPDIR/mm.eds" -o "$TMPDIR/mm_cart.leds" -l 20 --max-memory 100G >/dev/null 2>&1
    assert_exit_code 3 $? "cartesian estimate refused at 100G" || return 1
}

# Malformed size string is a plain usage error (exit 1), not the memory code.
test_max_memory_bad_value() {
    "$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" \
        -o "$TMPDIR/mm_bad.leds" -l 3 --max-memory nonsense >/dev/null 2>&1
    assert_exit_code 1 $? "invalid --max-memory value exits 1" || return 1
}

# --source-format: the merge always writes dense text SEDS internally, then
# re-encodes once at the end. The re-encoded file must name itself correctly, be
# smaller for the compressed variant, and carry exactly the same source sets.
test_source_format_edz() {
    "$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" \
        -o "$TMPDIR/sf.leds" -l 3 --source-format edz >/dev/null 2>&1
    assert_exit_code 0 $? "exits 0 with --source-format edz" || return 1
    assert_file_exists "$TMPDIR/sf.edz" "sources written as .edz" || return 1
    [ ! -e "$TMPDIR/sf.seds" ] || { echo -e "  ${RED}FAIL${NC}: text SEDS left behind"; return 1; }
    # The l-EDS itself must not depend on the source encoding.
    assert_file_equal "$TMPDIR/sf.leds" "$EXPECTED_DIR/small.linear.l3.leds" \
        "l-EDS unchanged by source format" || return 1
}

# Round-trip: EDZ output → SEDS must reproduce every source set (--verify expands
# complement/universal spellings before comparing, so encoding differences are OK).
test_source_format_edz_roundtrip() {
    local XFORM; XFORM=$(find_tool "edsparser-source-transform") || {
        echo "  SKIP: edsparser-source-transform not found"; return 0; }
    "$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" \
        -o "$TMPDIR/rt.leds" -l 3 --source-format edz >/dev/null 2>&1
    local out
    out=$("$XFORM" -i "$TMPDIR/rt.edz" -o "$TMPDIR/rt_back.seds" --verify 2>&1)
    assert_exit_code 0 $? "EDZ merge output converts back to SEDS" || return 1
    assert_contains "$out" "Verification passed" "all source sets survive the round-trip" || return 1
}

# edz-compressed must be accepted (zstd builds), produce a .edz, and be readable
# by a consumer tool — the format is only useful if something can load it back.
test_source_format_edz_compressed() {
    local STATS; STATS=$(find_tool "edsparser-stats") || {
        echo "  SKIP: edsparser-stats not found"; return 0; }
    local out
    out=$("$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" \
        -o "$TMPDIR/sc.leds" -l 3 --source-format edz-compressed 2>&1)
    if echo "$out" | grep -q "requires a zstd-enabled build"; then
        echo "  SKIP: built without zstd"; return 0
    fi
    assert_file_exists "$TMPDIR/sc.edz" "compressed sources written as .edz" || return 1
    assert_contains "$out" "re-encoded as edz-compressed" "reports the re-encode" || return 1

    # Same path universe whether read from the compressed EDZ or the text SEDS.
    cp "$TMPDIR/sc.leds" "$TMPDIR/sc_as.eds"
    local via_edz via_seds
    via_edz=$("$STATS" -i "$TMPDIR/sc_as.eds" -z "$TMPDIR/sc.edz" 2>&1 | grep -i "Total paths")
    "$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" \
        -o "$TMPDIR/sc_txt.leds" -l 3 >/dev/null 2>&1
    via_seds=$("$STATS" -i "$TMPDIR/sc_as.eds" -s "$TMPDIR/sc_txt.seds" 2>&1 | grep -i "Total paths")
    [ -n "$via_edz" ] && [ "$via_edz" = "$via_seds" ] || {
        echo -e "  ${RED}FAIL${NC}: path count differs (edz='$via_edz' seds='$via_seds')"; return 1; }
}

test_source_format_rejects_unknown() {
    "$TOOL" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" \
        -o "$TMPDIR/bad.leds" -l 3 --source-format nonsense >/dev/null 2>&1
    assert_exit_code 1 $? "unknown --source-format exits 1" || return 1
}

run_test "basic EDS→l-EDS (cartesian)"          test_basic_cartesian
run_test "EDS→l-EDS with sources (linear)"      test_linear_with_sources
run_test "larger context length (-l 5)"         test_larger_context_length
run_test "full output format (--full)"          test_full_output_format
run_test "multi-pass iterative merging"          test_iterative_merging
run_test "compact EDS format as input"          test_compact_input_format
run_test "multi-pass iterative merging (linear)" test_iterative_linear_merging
run_test "EDS→l-EDS with EDZ sources (-z)"       test_linear_with_edz_sources
run_test "SEDS and EDZ sources agree"            test_seds_and_edz_sources_agree
run_test "--seds/--edz long flag forms"          test_long_flag_forms
run_test "-s and -z mutually exclusive"          test_seds_and_edz_mutually_exclusive
run_test "-z forces EDZ on misnamed file"        test_edz_without_extension
run_test "-s fails on misnamed EDZ file"         test_seds_autodetect_fails_on_misnamed_edz
run_test "raw-copy pass-through round-trip"      test_raw_copy_passthrough_roundtrip
run_test "missing -l exits non-zero"            test_missing_context_length_fails
run_test "missing input exits non-zero"         test_missing_input_fails
run_test "--max-memory allows under budget"     test_max_memory_allows_under_budget
run_test "--max-memory refuses over budget (exit 3)" test_max_memory_refuses_over_budget
run_test "--max-memory cartesian vs linear bound" test_max_memory_cartesian_vs_linear
run_test "--max-memory rejects bad value"        test_max_memory_bad_value
run_test "--source-format edz writes .edz"       test_source_format_edz
run_test "--source-format edz round-trips"       test_source_format_edz_roundtrip
run_test "--source-format edz-compressed"        test_source_format_edz_compressed
run_test "--source-format rejects unknown value" test_source_format_rejects_unknown

print_summary
