#!/bin/bash
# E2E tests for edsparser-source-transform — SEDS <-> EDZ format equivalence.
#
# Goal: a source file's *meaning* (the set of path IDs per EDS string) must be
# preserved across format conversions, in both directions:
#   - a directly-generated EDZ  == an EDZ converted from the generated SEDS
#   - a directly-generated SEDS == a SEDS converted from the generated EDZ
#
# Why not `cmp` the files directly?  SEDS and EDZ are two *representations* of the
# same sets, not the same bytes:
#   - SEDS text uses complement encoding ({0,4} = "all paths except 4"); EDZ
#     stores explicit bitsets ({1,2,3,5}).  Same set, different spelling.
#   - sparse variants omit universal {0} entries and canonicalize the full
#     universe to {0}; dense variants list every entry.
# So equality must be checked *semantically*.  We do that with a canonicalization
# oracle: convert any source file to DENSE EDZ (explicit bitset per entry, no
# sparse omissions), which is a canonical byte form — two semantically-equal
# sources produce byte-identical dense EDZ.  (This deliberately does NOT use
# `--verify`, which currently compares raw PathSets and reports false mismatches
# on complement-encoded entries.)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"
: "${YELLOW:=\033[0;33m}"   # helpers.sh defines RED/GREEN/NC but not YELLOW

DATA_DIR="$SCRIPT_DIR/data"
VCF2EDS=$(find_tool "vcf2eds")                    || { echo "ERROR: vcf2eds not found";                    exit 1; }
XFORM=$(find_tool "edsparser-source-transform")   || { echo "ERROR: edsparser-source-transform not found"; exit 1; }
GENEDS=$(find_tool "genrandomeds")                || { echo "ERROR: genrandomeds not found";               exit 1; }
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== edsparser-source-transform ==="

# ── Oracle: canonicalize any source file to dense EDZ (explicit bitsets) ───────
canon() {
    # $1 = input source file, $2 = output dense-EDZ path
    "$XFORM" -i "$1" -o "$2" --to edz >/dev/null 2>&1
}

# Assert two source files are semantically equal (same sets per entry), regardless
# of format/sparsity, by comparing their canonical dense-EDZ forms byte-for-byte.
assert_sem_equal() {
    local a="$1" b="$2" msg="$3"
    local ca="$TMPDIR/_canon_a.edz" cb="$TMPDIR/_canon_b.edz"
    if ! canon "$a" "$ca"; then
        echo -e "  ${RED}FAIL${NC}: $msg — could not canonicalize $a"; return 1
    fi
    if ! canon "$b" "$cb"; then
        echo -e "  ${RED}FAIL${NC}: $msg — could not canonicalize $b"; return 1
    fi
    if ! cmp -s "$ca" "$cb"; then
        echo -e "  ${RED}FAIL${NC}: $msg — sources differ semantically"
        echo "    (canonical dense-EDZ forms of $(basename "$a") and $(basename "$b") differ)"
        return 1
    fi
}

# ── Fixtures: generate BOTH source formats directly from the same VCF ──────────
# vcf2eds -s writes sparse text SEDS; vcf2eds -z writes sparse binary EDZ.
# These are the "generated" forms the conversions are checked against.
gen_fixtures() {
    local name="$1" vcf="$2" ref="$3"
    "$VCF2EDS" -i "$vcf" -r "$ref" -o "$TMPDIR/${name}.eds"  -s "$TMPDIR/${name}.seds" >/dev/null 2>&1 || return 1
    "$VCF2EDS" -i "$vcf" -r "$ref" -o "$TMPDIR/${name}_z.eds" -z -s "$TMPDIR/${name}.edz" >/dev/null 2>&1 || return 1
    [[ -s "$TMPDIR/${name}.seds" && -s "$TMPDIR/${name}.edz" ]]
}

DATASETS=(small variants)

# Datasets exercised: small.vcf (5 samples, complement + universal + explicit
# entries) and variants.vcf (mixed variant types).  genrandomeds adds a
# larger-path-count SEDS for multi-byte EDZ bitset coverage.

# ── Foundational: the two directly-generated forms already agree ──────────────
test_generated_seds_and_edz_agree() {
    for d in "${DATASETS[@]}"; do
        assert_sem_equal "$TMPDIR/${d}.seds" "$TMPDIR/${d}.edz" \
            "[$d] vcf2eds -s (SEDS) and vcf2eds -z (EDZ) encode the same sources" || return 1
    done
}

# ── generated EDZ  ==  EDZ converted from the generated SEDS ───────────────────
test_generated_edz_equals_seds_to_edz() {
    for d in "${DATASETS[@]}"; do
        local conv="$TMPDIR/${d}_seds2edz.edz"
        "$XFORM" -i "$TMPDIR/${d}.seds" -o "$conv" --sparse >/dev/null 2>&1 || return 1
        assert_sem_equal "$TMPDIR/${d}.edz" "$conv" \
            "[$d] generated EDZ == SEDS→EDZ conversion" || return 1
    done
}

# ── generated SEDS ==  SEDS converted from the generated EDZ (the other way) ───
test_generated_seds_equals_edz_to_seds() {
    for d in "${DATASETS[@]}"; do
        local conv="$TMPDIR/${d}_edz2seds.seds"
        "$XFORM" -i "$TMPDIR/${d}.edz" -o "$conv" --to seds --sparse >/dev/null 2>&1 || return 1
        assert_sem_equal "$TMPDIR/${d}.seds" "$conv" \
            "[$d] generated SEDS == EDZ→SEDS conversion" || return 1
    done
}

# ── Round-trip: SEDS → EDZ → SEDS reproduces the original ──────────────────────
test_roundtrip_seds_edz_seds() {
    for d in "${DATASETS[@]}"; do
        "$XFORM" -i "$TMPDIR/${d}.seds"     -o "$TMPDIR/${d}_rt.edz"  >/dev/null 2>&1 || return 1
        "$XFORM" -i "$TMPDIR/${d}_rt.edz"   -o "$TMPDIR/${d}_rt.seds" >/dev/null 2>&1 || return 1
        assert_sem_equal "$TMPDIR/${d}.seds" "$TMPDIR/${d}_rt.seds" \
            "[$d] SEDS→EDZ→SEDS round-trips" || return 1
    done
}

# ── Round-trip: EDZ → SEDS → EDZ reproduces the original ───────────────────────
test_roundtrip_edz_seds_edz() {
    for d in "${DATASETS[@]}"; do
        "$XFORM" -i "$TMPDIR/${d}.edz"      -o "$TMPDIR/${d}_rt2.seds" >/dev/null 2>&1 || return 1
        "$XFORM" -i "$TMPDIR/${d}_rt2.seds" -o "$TMPDIR/${d}_rt2.edz"  >/dev/null 2>&1 || return 1
        assert_sem_equal "$TMPDIR/${d}.edz" "$TMPDIR/${d}_rt2.edz" \
            "[$d] EDZ→SEDS→EDZ round-trips" || return 1
    done
}

# ── Sparse and dense output of the same format are semantically equal ──────────
test_sparse_and_dense_equal() {
    for d in "${DATASETS[@]}"; do
        "$XFORM" -i "$TMPDIR/${d}.seds" -o "$TMPDIR/${d}_dense.edz"           >/dev/null 2>&1 || return 1
        "$XFORM" -i "$TMPDIR/${d}.seds" -o "$TMPDIR/${d}_sparse.edz" --sparse >/dev/null 2>&1 || return 1
        assert_sem_equal "$TMPDIR/${d}_dense.edz" "$TMPDIR/${d}_sparse.edz" \
            "[$d] dense EDZ and sparse EDZ encode the same sources" || return 1
    done
}

# ── Explicit --from/--to forms behave like auto-detect ────────────────────────
test_explicit_from_to_flags() {
    local d="small"
    "$XFORM" -i "$TMPDIR/${d}.seds" --from seds_sparse --to edz_sparse -o "$TMPDIR/${d}_explicit.edz" >/dev/null 2>&1 || return 1
    assert_sem_equal "$TMPDIR/${d}.edz" "$TMPDIR/${d}_explicit.edz" \
        "[$d] explicit --from/--to matches generated EDZ" || return 1
}

# ── --verify passes on complement-encoded real data (both directions) ─────────
# Regression: before the SEDS num_paths trailer landed, --verify compared raw
# PathSets and reported false "source set mismatch" on any complement entry
# ({0,e...}), which real VCF sources are full of. It must now pass.
test_verify_passes_on_complement_data() {
    for d in "${DATASETS[@]}"; do
        local out
        out=$("$XFORM" -i "$TMPDIR/${d}.seds" -o "$TMPDIR/${d}_v.edz" --sparse --verify 2>&1)
        if ! echo "$out" | grep -q "Verification passed"; then
            echo -e "  ${RED}FAIL${NC}: [$d] --verify SEDS→EDZ did not pass: $(echo "$out" | grep -i 'mismatch\|error')"
            return 1
        fi
        out=$("$XFORM" -i "$TMPDIR/${d}.edz" -o "$TMPDIR/${d}_v.seds" --to seds --sparse --verify 2>&1)
        if ! echo "$out" | grep -q "Verification passed"; then
            echo -e "  ${RED}FAIL${NC}: [$d] --verify EDZ→SEDS did not pass: $(echo "$out" | grep -i 'mismatch\|error')"
            return 1
        fi
    done
}

# ── SEDS carries num_paths in its trailer (SED2 sparse / SEDN dense) ───────────
test_seds_trailer_records_num_paths() {
    # sparse SEDS from vcf2eds -s ends with "SED2" 28 bytes from EOF.
    local m
    m=$(tail -c 28 "$TMPDIR/small.seds" | head -c 4 | xxd -p | tr -d '\n')
    [ "$m" == "53454432" ] || { echo -e "  ${RED}FAIL${NC}: sparse SEDS trailer not SED2 (got $m)"; return 1; }
    # num_paths round-trips: converting to EDZ and reading stats agrees with the
    # source (semantic equality already covered; here just assert it loads).
    "$XFORM" -i "$TMPDIR/small.seds" -o "$TMPDIR/small_np.edz" --verify >/dev/null 2>&1 \
        || { echo -e "  ${RED}FAIL${NC}: small.seds failed --verify round-trip"; return 1; }
}

# ── Larger path count (multi-byte EDZ bitsets) via genrandomeds SEDS ───────────
# genrandomeds writes a .seds alongside its .eds; use it as a bigger fixture to
# exercise EDZ bitsets spanning >1 byte (num_paths > 8).
test_roundtrip_large_pathcount() {
    genrandomeds --ref-size-mb 1 -v 0.05 --min-context 0 --seed 7 \
        -o "$TMPDIR/big.eds" >/dev/null 2>&1 || return 1
    [[ -s "$TMPDIR/big.seds" ]] || { echo -e "  ${RED}FAIL${NC}: genrandomeds produced no .seds"; return 1; }
    "$XFORM" -i "$TMPDIR/big.seds"    -o "$TMPDIR/big_rt.edz"  >/dev/null 2>&1 || return 1
    "$XFORM" -i "$TMPDIR/big_rt.edz"  -o "$TMPDIR/big_rt.seds" >/dev/null 2>&1 || return 1
    assert_sem_equal "$TMPDIR/big.seds" "$TMPDIR/big_rt.seds" \
        "[big] genrandomeds SEDS survives SEDS→EDZ→SEDS" || return 1
}

# ── EDZ_COMPRESSED (zstd) round-trips, when the tool was built with zstd ───────
# The compressed format is an optional build feature: if zstd was absent at build
# time, the tool errors with "built without zstd" and this test is skipped.
test_roundtrip_edz_compressed() {
    local d="small"
    # Compressed EDZ uses the .edz extension like every EDZ variant; the format is
    # self-describing via magic+flags, so --to edz_compressed forces compression.
    local cz="$TMPDIR/${d}_compressed.edz"
    local probe
    probe=$("$XFORM" -i "$TMPDIR/${d}.seds" -o "$cz" --to edz_compressed 2>&1)
    if echo "$probe" | grep -qi "without zstd"; then
        echo -e "  ${YELLOW}SKIP${NC}: tool built without zstd (EDZ_COMPRESSED disabled)"
        return 0
    fi
    if ! echo "$probe" | grep -q "source sets to"; then
        echo -e "  ${RED}FAIL${NC}: [$d] SEDS→EDZ_COMPRESSED failed: $probe"; return 1
    fi
    # Compressed form must be semantically equal to the plain SEDS...
    assert_sem_equal "$TMPDIR/${d}.seds" "$cz" \
        "[$d] EDZ_COMPRESSED encodes the same sources as SEDS" || return 1
    # ...and survive a full round-trip back to SEDS (auto-detecting the .edz as
    # EDZ_COMPRESSED from its flags, no explicit --from needed).
    local vout
    vout=$("$XFORM" -i "$cz" -o "$TMPDIR/${d}_c.seds" --to seds --verify 2>&1)
    echo "$vout" | grep -q "(edz_compressed)" \
        || { echo -e "  ${RED}FAIL${NC}: [$d] compressed .edz not auto-detected as edz_compressed: $vout"; return 1; }
    echo "$vout" | grep -q "Verification passed" \
        || { echo -e "  ${RED}FAIL${NC}: [$d] EDZ_COMPRESSED→SEDS --verify failed: $vout"; return 1; }
    assert_sem_equal "$TMPDIR/${d}.seds" "$TMPDIR/${d}_c.seds" \
        "[$d] SEDS→EDZ_COMPRESSED→SEDS round-trips" || return 1
}

# ══════════════════════════════════════════════════════════════════════════════
# RUN
# ══════════════════════════════════════════════════════════════════════════════

# Build fixtures once up front; fail loudly if generation itself breaks.
for d in "${DATASETS[@]}"; do
    if ! gen_fixtures "$d" "$DATA_DIR/${d}.vcf" "$DATA_DIR/${d}.fa"; then
        echo -e "  ${RED}FAIL${NC}: could not generate SEDS+EDZ fixtures for $d"
        exit 1
    fi
done

run_test "vcf2eds -s and -z encode same sources"       test_generated_seds_and_edz_agree
run_test "generated EDZ == SEDS→EDZ"                    test_generated_edz_equals_seds_to_edz
run_test "generated SEDS == EDZ→SEDS (other way)"       test_generated_seds_equals_edz_to_seds
run_test "SEDS→EDZ→SEDS round-trips"                    test_roundtrip_seds_edz_seds
run_test "EDZ→SEDS→EDZ round-trips"                     test_roundtrip_edz_seds_edz
run_test "dense EDZ == sparse EDZ (same sources)"       test_sparse_and_dense_equal
run_test "explicit --from/--to == auto-detect"          test_explicit_from_to_flags
run_test "--verify passes on complement data"           test_verify_passes_on_complement_data
run_test "SEDS trailer records num_paths (SED2)"        test_seds_trailer_records_num_paths
run_test "large path count SEDS↔EDZ round-trip"         test_roundtrip_large_pathcount
run_test "SEDS↔EDZ_COMPRESSED round-trip (zstd)"        test_roundtrip_edz_compressed

print_summary
