#!/bin/bash
# E2E tests for SEDS/EDZ source-file *robustness* and cross-tool consistency.
#
# The per-tool suites (test_eds2leds/test_stats/test_vcf2eds/test_source_transform)
# already cover the happy paths: -s/-z flags, format round-trips, sparse/dense,
# num_paths trailers, compression.  This suite fills the gaps between them —
# behaviours that only surface when a source file is loaded *against an EDS* by a
# consumer tool, and that must hold identically no matter which tool loads it:
#
#   1. Cardinality validation — EDS::load(eds, seds) throws when the source file's
#      entry count doesn't match the EDS's string count.  A stale/mismatched
#      .seds or .edz must be rejected (non-zero exit), not silently misread.
#   2. Legacy trailerless SEDS — plain-text SEDS with no binary num_paths trailer
#      (files predating the SED2/SEDN trailer) must still load, with num_paths
#      inferred from the largest path-ID token, and still drive correct linear
#      (phasing-aware) merging.
#   3. Universal/complement expansion — {0} (universal) and {0,e...} (complement)
#      SEDS entries must expand to the correct path count when loaded.
#   4. Malformed/truncated source files must be rejected, not crash or corrupt.
#   5. msa2eds's dense text SEDS must be loadable by downstream consumers.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
VCF_EXPECTED="$SCRIPT_DIR/expected/vcf2eds"
STATS=$(find_tool "edsparser-stats")             || { echo "ERROR: edsparser-stats not found"; exit 1; }
LEDS=$(find_tool "eds2leds")                      || { echo "ERROR: eds2leds not found"; exit 1; }
MSA=$(find_tool "msa2eds")                        || { echo "ERROR: msa2eds not found"; exit 1; }
XFORM=$(find_tool "edsparser-source-transform")   || { echo "ERROR: edsparser-source-transform not found"; exit 1; }
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== SEDS/EDZ robustness & cross-tool consistency ==="

# ── 1. Cardinality validation ─────────────────────────────────────────────────
# small.eds has 31 strings; variants.seds describes a 30-string EDS.  Loading the
# wrong-sized source against small.eds must fail in every consumer.  This guards
# the EDS::load cardinality check that catches stale/mismatched source files.

test_cardinality_mismatch_rejected_by_stats() {
    local out
    out=$("$STATS" -i "$DATA_DIR/small.eds" -s "$VCF_EXPECTED/variants.seds" 2>&1)
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: mismatched SEDS accepted by stats (exit 0)"; return 1; }
    assert_contains "$out" "cardinality" "stats reports a cardinality error" || return 1
}

test_cardinality_mismatch_rejected_by_eds2leds() {
    "$LEDS" -i "$DATA_DIR/small.eds" -s "$VCF_EXPECTED/variants.seds" \
        -o "$TMPDIR/mismatch.leds" -l 3 >/dev/null 2>&1
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: mismatched SEDS accepted by eds2leds (exit 0)"; return 1; }
}

test_cardinality_mismatch_rejected_edz() {
    # Same mismatch through the EDZ container: convert the wrong-sized SEDS to EDZ,
    # then load it against small.eds.  The check is on entry count, so the binary
    # container must reject it just like the text one.
    "$XFORM" -i "$VCF_EXPECTED/variants.seds" -o "$TMPDIR/variants.edz" >/dev/null 2>&1 || return 1
    "$STATS" -i "$DATA_DIR/small.eds" -z "$TMPDIR/variants.edz" >/dev/null 2>&1
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: mismatched EDZ accepted by stats -z (exit 0)"; return 1; }
}

# ── 2. Legacy trailerless SEDS ────────────────────────────────────────────────
# A hand-written text SEDS with no binary trailer (the pre-SED2/SEDN on-disk form).
# EDS `{AC}{A,C}{GT}{T,G}` = 6 strings; sources put common symbols at universal {0}
# and phase the two alternatives onto path 1 / path 2.  num_paths must be inferred
# from the largest token (2), and linear merge must honour the phasing.
setup_legacy_fixture() {
    printf '{AC}{A,C}{GT}{T,G}'    > "$TMPDIR/legacy.eds"
    printf '{0}{1}{2}{0}{1}{2}'    > "$TMPDIR/legacy.seds"   # no trailer bytes
}

test_legacy_trailerless_seds_loads() {
    setup_legacy_fixture
    local out
    out=$("$STATS" -i "$TMPDIR/legacy.eds" -s "$TMPDIR/legacy.seds" -v 2>&1)
    assert_exit_code 0 $? "stats loads trailerless SEDS" || return 1
    # num_paths inferred from the largest path-ID token (2), not a trailer.
    assert_contains "$out" "Total paths (genomes):                   2" \
        "num_paths inferred as 2 from largest token" || return 1
}

test_legacy_trailerless_linear_merge() {
    # Linear (phasing-aware) merge on the trailerless SEDS must keep only the two
    # valid haplotypes: path 1 = A…T (AGTT), path 2 = C…G (CGTG).  Cartesian would
    # emit four strings; getting exactly {AGTT,CGTG} proves the source was both
    # loaded and applied.
    setup_legacy_fixture
    "$LEDS" -i "$TMPDIR/legacy.eds" -s "$TMPDIR/legacy.seds" \
        -o "$TMPDIR/legacy.leds" -l 3 >/dev/null 2>&1
    assert_exit_code 0 $? "eds2leds linear merge on trailerless SEDS exits 0" || return 1
    local content
    content=$(cat "$TMPDIR/legacy.leds")
    [ "$content" == "AC{AGTT,CGTG}" ] || {
        echo -e "  ${RED}FAIL${NC}: expected 'AC{AGTT,CGTG}' (phased), got '$content'"; return 1; }
}

# ── 3. Universal/complement expansion ─────────────────────────────────────────
# small.seds is full of universal ({0}) and complement ({0,e...}) entries over a
# 5-path universe.  A consumer must expand those to exactly 5 distinct paths, not
# undercount (the complement-expansion bug fixed 2026-07-02) nor count the literal
# {0} token as a path.
test_complement_and_universal_expand_to_correct_path_count() {
    local out
    out=$("$STATS" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" -v 2>&1)
    assert_exit_code 0 $? "stats loads complement-heavy SEDS" || return 1
    assert_contains "$out" "Total paths (genomes):                   5" \
        "universal/complement entries expand to 5 paths" || return 1
}

test_complement_path_count_matches_across_seds_and_edz() {
    # The same path universe must be reported whether the sources come from text
    # SEDS (complement encoding) or binary EDZ (explicit bitsets).
    local n_seds n_edz
    n_seds=$("$STATS" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" -v 2>&1 | grep "Total paths")
    n_edz=$( "$STATS" -i "$DATA_DIR/small.eds" -z "$DATA_DIR/small.edz"  -v 2>&1 | grep "Total paths")
    [ -n "$n_seds" ] && [ "$n_seds" == "$n_edz" ] || {
        echo -e "  ${RED}FAIL${NC}: path count differs SEDS='$n_seds' vs EDZ='$n_edz'"; return 1; }
}

# ── 4. Malformed / truncated source files ─────────────────────────────────────
test_truncated_edz_rejected() {
    # A binary EDZ cut to a stub (magic only, no body) must be rejected, not crash.
    head -c 6 "$DATA_DIR/small.edz" > "$TMPDIR/trunc.edz"
    "$STATS" -i "$DATA_DIR/small.eds" -z "$TMPDIR/trunc.edz" >/dev/null 2>&1
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: truncated EDZ accepted (exit 0)"; return 1; }
}

test_garbage_seds_text_rejected() {
    # Non-SEDS text (no {...} entries) must fail rather than be silently misparsed.
    printf 'this is not a valid seds file at all\n' > "$TMPDIR/garbage.seds"
    "$STATS" -i "$DATA_DIR/small.eds" -s "$TMPDIR/garbage.seds" >/dev/null 2>&1
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: garbage SEDS accepted (exit 0)"; return 1; }
}

# Every EDZ variant shares the .edz extension and identifies itself through the
# header flags, so -z (which forces "this is EDZ") must accept a compressed one
# rather than rejecting it for having the compression flag set.  Regression: -z
# used to hard-force the uncompressed parser, making EDZ_COMPRESSED sources —
# e.g. eds2leds --source-format edz-compressed output — unreadable via -z.
test_stats_z_accepts_compressed_edz() {
    "$XFORM" -i "$DATA_DIR/small.seds" -o "$TMPDIR/comp.edz" --compress >/dev/null 2>&1 || {
        echo "  SKIP: built without zstd"; return 0; }
    local via_z via_s
    via_z=$("$STATS" -i "$DATA_DIR/small.eds" -z "$TMPDIR/comp.edz" 2>&1)
    assert_exit_code 0 $? "stats -z loads EDZ_COMPRESSED" || return 1
    via_z=$(echo "$via_z" | grep -i "Total paths")
    via_s=$("$STATS" -i "$DATA_DIR/small.eds" -s "$DATA_DIR/small.seds" 2>&1 | grep -i "Total paths")
    [ -n "$via_z" ] && [ "$via_z" = "$via_s" ] || {
        echo -e "  ${RED}FAIL${NC}: compressed EDZ path count '$via_z' != SEDS '$via_s'"; return 1; }
}

# ── 5. msa2eds dense SEDS is consumable downstream ────────────────────────────
# test_msa2eds only golden-diffs the .seds bytes; it never feeds them back to a
# consumer.  Confirm the dense text SEDS msa2eds writes loads cleanly (cardinality
# matches, entries parse) in both a stats read and an eds2leds linear merge.
test_msa_seds_loadable_downstream() {
    "$MSA" -i "$DATA_DIR/small.msa" -o "$TMPDIR/msa.eds" -s "$TMPDIR/msa.seds" >/dev/null 2>&1 || return 1
    [[ -s "$TMPDIR/msa.seds" ]] || { echo -e "  ${RED}FAIL${NC}: msa2eds produced no .seds"; return 1; }
    "$STATS" -i "$TMPDIR/msa.eds" -s "$TMPDIR/msa.seds" >/dev/null 2>&1
    assert_exit_code 0 $? "stats loads msa2eds SEDS" || return 1
    "$LEDS" -i "$TMPDIR/msa.eds" -s "$TMPDIR/msa.seds" -o "$TMPDIR/msa.leds" -l 3 >/dev/null 2>&1
    assert_exit_code 0 $? "eds2leds linear merge loads msa2eds SEDS" || return 1
    assert_not_empty "$TMPDIR/msa.leds" "l-EDS from msa2eds SEDS not empty" || return 1
}

# ══════════════════════════════════════════════════════════════════════════════
# RUN
# ══════════════════════════════════════════════════════════════════════════════
run_test "cardinality mismatch rejected (stats -s)"     test_cardinality_mismatch_rejected_by_stats
run_test "cardinality mismatch rejected (eds2leds -s)"  test_cardinality_mismatch_rejected_by_eds2leds
run_test "cardinality mismatch rejected (stats -z EDZ)" test_cardinality_mismatch_rejected_edz
run_test "legacy trailerless SEDS loads"                test_legacy_trailerless_seds_loads
run_test "legacy trailerless SEDS drives linear merge"  test_legacy_trailerless_linear_merge
run_test "universal/complement expand to 5 paths"       test_complement_and_universal_expand_to_correct_path_count
run_test "path count agrees across SEDS and EDZ"        test_complement_path_count_matches_across_seds_and_edz
run_test "stats -z accepts EDZ_COMPRESSED"              test_stats_z_accepts_compressed_edz
run_test "truncated EDZ rejected"                       test_truncated_edz_rejected
run_test "garbage SEDS text rejected"                   test_garbage_seds_text_rejected
run_test "msa2eds dense SEDS loadable downstream"       test_msa_seds_loadable_downstream

print_summary
