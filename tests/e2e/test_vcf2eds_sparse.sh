#!/bin/bash
# E2E tests for vcf2eds source formats: -s (SEDS, sparse) and -z (EDZ, sparse).
# Both are always written in sparse form (universal {0} entries omitted);
# there is no CLI-selectable dense mode.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
TOOL=$(find_tool "vcf2eds")   || { echo "ERROR: vcf2eds not found";    exit 1; }
LEDS=$(find_tool "eds2leds")  || { echo "ERROR: eds2leds not found";   exit 1; }
STATS=$(find_tool "edsparser-stats") || { echo "ERROR: edsparser-stats not found"; exit 1; }

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

VCF="$DATA_DIR/small.vcf"
REF="$DATA_DIR/small.fa"

echo "=== vcf2eds source formats (SEDS/-s vs EDZ/-z, both sparse) ==="

# ── helpers ──────────────────────────────────────────────────────────────────

# Check that two files are identical in content.
assert_files_identical() {
    local a="$1" b="$2" msg="$3"
    if ! diff -q "$a" "$b" >/dev/null 2>&1; then
        echo -e "  ${RED}FAIL${NC}: $msg — files differ"
        diff "$a" "$b" | head -10 | sed 's/^/    /'
        return 1
    fi
}

# ── SEDS (default, sparse) tests ────────────────────────────────────────────

test_seds_produces_seds_file() {
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/sp.eds" -s "$TMPDIR/sp.seds" 2>/dev/null
    assert_exit_code 0 $? "default (SEDS) exits 0" || return 1
    assert_file_exists "$TMPDIR/sp.seds" "default creates .seds file" || return 1
    assert_not_empty "$TMPDIR/sp.seds" "default .seds file non-empty" || return 1
}

test_seds_trailer_magic() {
    # SEDS_SPARSE trailer ends with "SEDS" (4 bytes = 53454453 in little-endian hex).
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/magic.eds" -s "$TMPDIR/magic.seds" 2>/dev/null
    assert_exit_code 0 $? "default (SEDS) exits 0" || return 1
    # The magic word "SEDS" is at bytes -20..-17 of the file (4B magic out of 20B trailer).
    local magic
    magic=$(tail -c 20 "$TMPDIR/magic.seds" | head -c 4 | xxd -p | tr -d '\n')
    if [ "$magic" != "53454453" ]; then
        echo -e "  ${RED}FAIL${NC}: SEDS_SPARSE trailer magic wrong: got $magic, want 53454453"
        return 1
    fi
}

test_seds_loadable_by_eds2leds() {
    # eds2leds with -s loads the source file; any corruption would cause an error.
    # (passing -s auto-selects linear/phasing-aware merge)
    "$LEDS" -i "$TMPDIR/sp.eds" -s "$TMPDIR/sp.seds" \
        -l 1 -o "$TMPDIR/sp_l1.leds" 2>/dev/null
    assert_exit_code 0 $? "eds2leds with -s loads SEDS_SPARSE without error"
}

# ── EDZ (-z, sparse) tests ───────────────────────────────────────────────────

test_edz_produces_edz_file() {
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/edz_sp.eds" -s "$TMPDIR/edz_sp.edz" -z 2>/dev/null
    assert_exit_code 0 $? "-z exits 0" || return 1
    assert_file_exists "$TMPDIR/edz_sp.edz" "-z creates .edz file" || return 1
    assert_not_empty "$TMPDIR/edz_sp.edz" "-z .edz file non-empty" || return 1
}

test_edz_header_magic() {
    # EDZ files start with "EDZ\0" (4 bytes = 45445a00 in hex).
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/edzmag.eds" -s "$TMPDIR/edzmag.edz" -z 2>/dev/null
    assert_exit_code 0 $? "-z exits 0" || return 1
    local magic
    magic=$(head -c 4 "$TMPDIR/edzmag.edz" | xxd -p | tr -d '\n')
    if [ "$magic" != "45445a00" ]; then
        echo -e "  ${RED}FAIL${NC}: EDZ_SPARSE header magic wrong: got $magic, want 45445a00"
        return 1
    fi
}

test_edz_flags_byte() {
    # EDZ_SPARSE flags (bytes 4-5, little-endian) must be 0x0006 = 0600 in hex.
    local flags
    flags=$(dd if="$TMPDIR/edzmag.edz" bs=1 skip=4 count=2 2>/dev/null | xxd -p | tr -d '\n')
    if [ "$flags" != "0600" ]; then
        echo -e "  ${RED}FAIL${NC}: EDZ_SPARSE flags wrong: got $flags, want 0600"
        return 1
    fi
}

test_edz_loadable_by_eds2leds() {
    "$LEDS" -i "$TMPDIR/edz_sp.eds" -z "$TMPDIR/edz_sp.edz" \
        -l 1 -o "$TMPDIR/edz_sp_l1.leds" 2>/dev/null
    assert_exit_code 0 $? "eds2leds with -z loads EDZ_SPARSE without error"
}

# ── cross-format consistency ─────────────────────────────────────────────────

test_seds_and_edz_produce_same_eds() {
    # -s and -z only change the source container; .eds output must match.
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/cmp_seds.eds" -s "$TMPDIR/cmp_seds.seds" 2>/dev/null
    assert_exit_code 0 $? "vcf2eds (default) exits 0" || return 1
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/cmp_edz.eds" -s "$TMPDIR/cmp_edz.edz" -z 2>/dev/null
    assert_exit_code 0 $? "vcf2eds -z exits 0" || return 1
    assert_files_identical "$TMPDIR/cmp_seds.eds" "$TMPDIR/cmp_edz.eds" \
        "-z EDS content matches default (-s) EDS"
}

test_seds_and_edz_stats_agree() {
    # Regression test for a real bug found in this format: PathSet uses
    # complement encoding ({0,e1,..} = all paths except e1,..), which SEDS
    # uses opportunistically (>50% frequency) but EDZ's bitset decode never
    # produces except for the pure-universal {0} case. edsparser-stats must
    # expand both representations to the same true path count.
    local out_seds out_edz
    out_seds=$("$STATS" -i "$TMPDIR/sp.eds" -s "$TMPDIR/sp.seds" -v)
    out_edz=$("$STATS" -i "$TMPDIR/sp.eds" -z "$TMPDIR/edz_sp.edz" -v)
    if [ "$out_seds" != "$out_edz" ]; then
        echo -e "  ${RED}FAIL${NC}: SEDS and EDZ verbose stats differ"
        diff <(echo "$out_seds") <(echo "$out_edz") | head -10 | sed 's/^/    /'
        return 1
    fi
}

test_long_flag_forms() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/lf_seds.eds" --seds "$TMPDIR/lf_seds.seds" 2>/dev/null
    assert_exit_code 0 $? "vcf2eds --seds exits 0" || return 1
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/lf_edz.eds" --seds "$TMPDIR/lf_edz.edz" --edz 2>/dev/null
    assert_exit_code 0 $? "vcf2eds --edz exits 0" || return 1
}

# ── l-EDS (-l) mode: sources are always dense text SEDS ────────────────────
# The EDS→l-EDS merge pipeline has no sparse/EDZ writer, so -l always writes
# dense text SEDS regardless of -z (see vcf2eds.cpp). This is the exact
# scenario that used to produce a corrupted output: -z + -l wrote plain text
# into a file extension-named ".edz".

test_leds_default_sources_are_dense() {
    "$TOOL" -i "$VCF" -r "$REF" -l 5 -o "$TMPDIR/leds_default.leds" \
        -s "$TMPDIR/leds_default.seds" 2>/dev/null
    assert_exit_code 0 $? "vcf2eds -l 5 (default) exits 0" || return 1
    # Dense SEDS has no "SEDS" sparse trailer magic in its last 20 bytes.
    local magic
    magic=$(tail -c 20 "$TMPDIR/leds_default.seds" | head -c 4 | xxd -p | tr -d '\n')
    if [ "$magic" == "53454453" ]; then
        echo -e "  ${RED}FAIL${NC}: l-EDS default sources unexpectedly sparse (SEDS trailer found)"
        return 1
    fi
}

test_leds_with_edz_flag_warns_and_falls_back() {
    # -z combined with -l must not corrupt the output: it should warn on
    # stderr and produce valid dense text SEDS (readable, not binary EDZ).
    local stderr_out
    stderr_out=$("$TOOL" -i "$VCF" -r "$REF" -l 5 -o "$TMPDIR/leds_edz.leds" \
        -s "$TMPDIR/leds_edz.edz" -z 2>&1 >/dev/null)
    assert_exit_code 0 $? "vcf2eds -l 5 -z exits 0" || return 1
    assert_contains "$stderr_out" "not supported in l-EDS" "warns that -z is ignored in l-EDS mode" || return 1

    # Must NOT start with the EDZ magic bytes — content is plain text SEDS
    # even though the file is named .edz.
    local magic
    magic=$(head -c 4 "$TMPDIR/leds_edz.edz" | xxd -p | tr -d '\n')
    if [ "$magic" == "45445a00" ]; then
        echo -e "  ${RED}FAIL${NC}: l-EDS -z output has EDZ magic bytes but pipeline can't write EDZ"
        return 1
    fi

    # Must be readable as plain SEDS text once given a .seds extension (proves
    # it's genuinely text, not corrupted binary mislabeled with a .edz
    # extension). -s on the original .edz-named path would still route to
    # EDZ parsing by extension regardless of the actual content, so the
    # content itself must be checked via a correctly-named copy.
    cp "$TMPDIR/leds_edz.edz" "$TMPDIR/leds_edz_copy.seds"
    "$STATS" -i "$TMPDIR/leds_edz.leds" -s "$TMPDIR/leds_edz_copy.seds" >/dev/null 2>&1
    assert_exit_code 0 $? "l-EDS -z output is readable as plain SEDS text once renamed .seds"
}

# ── run all ──────────────────────────────────────────────────────────────────

run_test "SEDS (default): creates .seds file"           test_seds_produces_seds_file
run_test "SEDS (default): trailer magic = 'SEDS'"        test_seds_trailer_magic
run_test "SEDS (default): loadable by eds2leds (linear)" test_seds_loadable_by_eds2leds

run_test "EDZ (-z): creates .edz file"                   test_edz_produces_edz_file
run_test "EDZ (-z): header magic = 'EDZ\\0'"             test_edz_header_magic
run_test "EDZ (-z): flags byte = 0x0006"                 test_edz_flags_byte
run_test "EDZ (-z): loadable by eds2leds (linear, -z)"   test_edz_loadable_by_eds2leds

run_test "-s and -z produce same EDS content"            test_seds_and_edz_produce_same_eds
run_test "-s and -z produce same stats"                  test_seds_and_edz_stats_agree
run_test "--seds/--edz long flag forms"                  test_long_flag_forms

run_test "l-EDS (-l): default sources are dense"         test_leds_default_sources_are_dense
run_test "l-EDS (-l) + -z: warns and falls back safely"  test_leds_with_edz_flag_warns_and_falls_back

print_summary
