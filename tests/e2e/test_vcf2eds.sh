#!/bin/bash
# E2E tests for the vcf2eds tool — single unified suite.
#
# Sections:
#   1. Basic VCF→EDS / l-EDS conversion and core error paths
#   2. CLI arguments — every option (short + long forms), defaults, validation,
#      and --block-size semantics
#   3. Variant types & processing statistics (data/variants.vcf)
#   4. Source formats — SEDS (-s) vs EDZ (-z), both sparse, and l-EDS fallback
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
EXPECTED_DIR="$SCRIPT_DIR/expected/vcf2eds"
TOOL=$(find_tool "vcf2eds")          || { echo "ERROR: vcf2eds not found";          exit 1; }
LEDS=$(find_tool "eds2leds")         || { echo "ERROR: eds2leds not found";         exit 1; }
STATS=$(find_tool "edsparser-stats") || { echo "ERROR: edsparser-stats not found";  exit 1; }
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

VCF="$DATA_DIR/small.vcf"
REF="$DATA_DIR/small.fa"
VARIANTS_VCF="$DATA_DIR/variants.vcf"
VARIANTS_REF="$DATA_DIR/variants.fa"

echo "=== vcf2eds ==="

# Check that two files are identical in content.
assert_files_identical() {
    local a="$1" b="$2" msg="$3"
    if ! diff -q "$a" "$b" >/dev/null 2>&1; then
        echo -e "  ${RED}FAIL${NC}: $msg — files differ"
        diff "$a" "$b" | head -10 | sed 's/^/    /'
        return 1
    fi
}

# ══════════════════════════════════════════════════════════════════════════════
# 1. BASIC CONVERSION & CORE ERROR PATHS
# ══════════════════════════════════════════════════════════════════════════════

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

test_missing_reference_file_fails() {
    # -r points at a nonexistent FASTA file (arg present, file absent).
    "$TOOL" -i "$VCF" -r "$TMPDIR/nonexistent.fa" -o "$TMPDIR/x.eds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing ref file — expected non-zero exit, got 0"; return 1; }
}

test_missing_input_file_fails() {
    # -i points at a nonexistent VCF file (has .vcf ext, so passes ext check).
    "$TOOL" -i "$TMPDIR/nonexistent.vcf" -r "$REF" -o "$TMPDIR/x.eds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing input file — expected non-zero exit, got 0"; return 1; }
}

test_no_args_fails() {
    "$TOOL" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: no-args — expected non-zero exit, got 0"; return 1; }
}

# ══════════════════════════════════════════════════════════════════════════════
# 2. CLI ARGUMENTS — every option, short + long, defaults, validation
# ══════════════════════════════════════════════════════════════════════════════

test_help_long() {
    local out
    out=$("$TOOL" --help 2>&1)
    assert_exit_code 0 $? "--help exits 0" || return 1
    assert_contains "$out" "vcf2eds" "help mentions the tool name" || return 1
    assert_contains "$out" "SUPPORTED VARIANTS" "help lists supported variants" || return 1
    assert_contains "$out" "EXAMPLES" "help shows examples" || return 1
}

test_help_short() {
    local out
    out=$("$TOOL" -h 2>&1)
    assert_exit_code 0 $? "-h exits 0" || return 1
    assert_contains "$out" "vcf2eds" "-h mentions the tool name" || return 1
}

test_help_lists_every_option() {
    # Boost renders options as "-i [ --input ] arg"; grep needs -F -- so the
    # leading dashes in the pattern are not parsed as grep options.
    local out
    out=$("$TOOL" --help 2>&1)
    for opt in "--input" "--reference" "--output" "--seds" "--context-length" \
               "--block-size" "--edz" "--keep-eds" "--help"; do
        if ! printf '%s' "$out" | grep -qF -- "$opt"; then
            echo -e "  ${RED}FAIL${NC}: help does not document $opt"
            return 1
        fi
    done
}

test_short_flags_all() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/sf.eds" -s "$TMPDIR/sf.seds" -b 10000000 2>/dev/null
    assert_exit_code 0 $? "all short flags (-i -r -o -s -b) exit 0" || return 1
    assert_file_exists "$TMPDIR/sf.eds" "short-flag run produces .eds" || return 1
}

test_long_flags_all() {
    "$TOOL" --input "$VCF" --reference "$REF" --output "$TMPDIR/lf.eds" \
            --seds "$TMPDIR/lf.seds" --block-size 10000000 2>/dev/null
    assert_exit_code 0 $? "all long flags exit 0" || return 1
    assert_file_exists "$TMPDIR/lf.eds" "long-flag run produces .eds" || return 1
}

test_short_and_long_flags_identical() {
    # -i/-r/-o/-s must be exact synonyms for --input/--reference/--output/--seds.
    assert_files_identical "$TMPDIR/sf.eds"  "$TMPDIR/lf.eds"  "short/long .eds identical" || return 1
    assert_files_identical "$TMPDIR/sf.seds" "$TMPDIR/lf.seds" "short/long .seds identical" || return 1
}

test_context_length_long_form() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/cl.leds" --context-length 5 2>/dev/null
    assert_exit_code 0 $? "--context-length 5 exits 0" || return 1
    assert_not_empty "$TMPDIR/cl.leds" "--context-length output non-empty" || return 1
}

test_default_eds_paths() {
    # With no -o/-s, EDS output lands next to the input: <stem>.eds + <stem>.seds.
    cp "$VCF" "$TMPDIR/sample.vcf"
    ( cd "$TMPDIR" && "$TOOL" -i sample.vcf -r "$REF" >/dev/null 2>&1 )
    assert_exit_code 0 $? "default-path run exits 0" || return 1
    assert_file_exists "$TMPDIR/sample.eds"  "default derives <stem>.eds" || return 1
    assert_file_exists "$TMPDIR/sample.seds" "default derives <stem>.seds" || return 1
}

test_default_leds_paths() {
    # With -l and no -o, output is <stem>_l<N>.leds + <stem>_l<N>.seds.
    cp "$VCF" "$TMPDIR/sample2.vcf"
    ( cd "$TMPDIR" && "$TOOL" -i sample2.vcf -r "$REF" -l 5 >/dev/null 2>&1 )
    assert_exit_code 0 $? "default l-EDS path run exits 0" || return 1
    assert_file_exists "$TMPDIR/sample2_l5.leds" "default derives <stem>_l5.leds" || return 1
    assert_file_exists "$TMPDIR/sample2_l5.seds" "default derives <stem>_l5.seds" || return 1
}

test_default_seds_next_to_output() {
    # -o given but no -s: sources land next to the -o output with a .seds ext.
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/custom.eds" >/dev/null 2>&1
    assert_exit_code 0 $? "run with -o but no -s exits 0" || return 1
    assert_file_exists "$TMPDIR/custom.seds" "derives <output-stem>.seds next to -o output" || return 1
}

test_context_length_zero_is_regular_eds() {
    # -l 0 must behave exactly like omitting -l (regular EDS, not l-EDS).
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/l0.eds" -s "$TMPDIR/l0.seds" -l 0 2>/dev/null
    assert_exit_code 0 $? "-l 0 exits 0" || return 1
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/nol.eds" -s "$TMPDIR/nol.seds" 2>/dev/null
    assert_files_identical "$TMPDIR/l0.eds" "$TMPDIR/nol.eds" "-l 0 equals no-l regular EDS" || return 1
}

test_block_size_default_equals_legacy() {
    # Default (10M) and legacy -b 0 (load-all) must produce identical output when
    # the whole reference fits in one block.
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/def.eds" 2>/dev/null
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/leg.eds" -b 0 2>/dev/null
    assert_files_identical "$TMPDIR/def.eds" "$TMPDIR/leg.eds" "default block size == -b 0 (legacy)" || return 1
}

test_block_size_large_equals_default() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/big.eds" -b 10000000 2>/dev/null
    assert_files_identical "$TMPDIR/def.eds" "$TMPDIR/big.eds" "explicit large block == default" || return 1
}

test_block_size_small_still_valid() {
    # A tiny block size regroups symbols at boundaries but must still produce a
    # non-empty, parseable EDS+SEDS.
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/tiny.eds" -s "$TMPDIR/tiny.seds" -b 1 2>/dev/null
    assert_exit_code 0 $? "-b 1 exits 0" || return 1
    assert_not_empty "$TMPDIR/tiny.eds" "-b 1 produces non-empty EDS" || return 1
    "$STATS" -i "$TMPDIR/tiny.eds" -s "$TMPDIR/tiny.seds" >/dev/null 2>&1
    assert_exit_code 0 $? "-b 1 output is parseable by edsparser-stats" || return 1
}

test_block_size_variant_count_invariant() {
    # Block size only affects symbol grouping, never which variants are processed.
    local c_default c_small c_legacy
    c_default=$("$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/bc.eds" 2>&1 | grep "Successfully processed")
    c_small=$("$TOOL"   -i "$VCF" -r "$REF" -o "$TMPDIR/bc.eds" -b 1 2>&1 | grep "Successfully processed")
    c_legacy=$("$TOOL"  -i "$VCF" -r "$REF" -o "$TMPDIR/bc.eds" -b 0 2>&1 | grep "Successfully processed")
    if [ "$c_default" != "$c_small" ] || [ "$c_default" != "$c_legacy" ]; then
        echo -e "  ${RED}FAIL${NC}: processed-variant count varies with block size"
        echo "    default: $c_default"; echo "    small:   $c_small"; echo "    legacy:  $c_legacy"
        return 1
    fi
}

test_non_vcf_extension_fails() {
    cp "$VCF" "$TMPDIR/notvcf.txt"
    local out
    out=$("$TOOL" -i "$TMPDIR/notvcf.txt" -r "$REF" -o "$TMPDIR/x.eds" 2>&1)
    assert_exit_code 1 $? "non-.vcf input exits 1" || return 1
    assert_contains "$out" "must be a VCF" "reports the extension requirement" || return 1
}

test_invalid_block_size_fails() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/x.eds" -b notanumber 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: -b notanumber — expected non-zero exit, got 0"; return 1; }
}

test_invalid_context_length_fails() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/x.eds" -l notanumber 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: -l notanumber — expected non-zero exit, got 0"; return 1; }
}

test_unknown_option_fails() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/x.eds" --bogus-option 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: --bogus-option — expected non-zero exit, got 0"; return 1; }
}

test_missing_required_reference_arg_fails() {
    # --reference option omitted entirely (boost 'required' error).
    "$TOOL" --input "$VCF" -o "$TMPDIR/x.eds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing --reference arg — expected non-zero exit, got 0"; return 1; }
}

test_missing_required_input_arg_fails() {
    # --input option omitted entirely (boost 'required' error).
    "$TOOL" --reference "$REF" -o "$TMPDIR/x.eds" 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || { echo -e "  ${RED}FAIL${NC}: missing --input arg — expected non-zero exit, got 0"; return 1; }
}

# ══════════════════════════════════════════════════════════════════════════════
# 3. VARIANT TYPES & PROCESSING STATISTICS  (data/variants.vcf)
# ══════════════════════════════════════════════════════════════════════════════
# variants.vcf exercises every documented variant class against an ACGT-repeat
# reference: SNP, insertion, deletion, <DEL>, <INS>, <INV>, <CN0/1/2>,
# multi-allelic (all processed); <BND> (unsupported SV), chrX (wrong chrom), and
# a truncated line (malformed) — all skipped.

VARIANTS_STDOUT=$("$TOOL" -i "$VARIANTS_VCF" -r "$VARIANTS_REF" \
                          -o "$TMPDIR/v.eds" -s "$TMPDIR/v.seds" 2>/dev/null)
VARIANTS_RC=$?

test_variants_runs_successfully() {
    assert_exit_code 0 $VARIANTS_RC "vcf2eds on mixed-variant VCF exits 0" || return 1
    assert_file_exists "$TMPDIR/v.eds"  ".eds created" || return 1
    assert_file_exists "$TMPDIR/v.seds" ".seds created" || return 1
}

test_variants_total_counted() {
    assert_contains "$VARIANTS_STDOUT" "Total variants read:        13" "counts all 13 records" || return 1
}

test_variants_processed_count() {
    assert_contains "$VARIANTS_STDOUT" "Successfully processed:     10" "processes the 10 valid variants" || return 1
}

test_variants_malformed_skipped() {
    assert_contains "$VARIANTS_STDOUT" "Skipped (malformed):        1" "flags the truncated line as malformed" || return 1
}

test_variants_unsupported_sv_skipped() {
    assert_contains "$VARIANTS_STDOUT" "Skipped (unsupported SV):   1" "flags <BND> as unsupported SV" || return 1
}

test_variants_wrong_chrom_skipped() {
    assert_contains "$VARIANTS_STDOUT" "Skipped (wrong chromosome): 1" "flags the chrX record as wrong chromosome" || return 1
}

test_variants_success_rate() {
    assert_contains "$VARIANTS_STDOUT" "Success rate:               76.9%" "reports 10/13 = 76.9% success" || return 1
}

test_variants_eds_matches_golden() {
    assert_file_equal "$TMPDIR/v.eds" "$EXPECTED_DIR/variants.eds" "variant EDS matches golden" || return 1
}

test_variants_seds_matches_golden() {
    assert_file_equal "$TMPDIR/v.seds" "$EXPECTED_DIR/variants.seds" "variant SEDS matches golden" || return 1
}

test_variants_multiallelic_expands() {
    # pos45 A→C,G against ref base A must yield a 3-way set {A,C,G}.
    assert_file_contains "$TMPDIR/v.eds" "{A,C,G}" "multi-allelic site expands to all three alleles" || return 1
}

test_variants_deletion_empty_alt() {
    # A <DEL> / <CN0> deletion introduces an empty-string alternative, e.g. {G,}.
    if ! grep -qE '\{[A-Z]+,\}' "$TMPDIR/v.eds"; then
        echo -e "  ${RED}FAIL${NC}: expected an empty-alternative (deletion) set like {G,} in output"
        return 1
    fi
}

test_variants_golden_roundtrips() {
    "$STATS" -i "$EXPECTED_DIR/variants.eds" -s "$EXPECTED_DIR/variants.seds" >/dev/null 2>&1
    assert_exit_code 0 $? "golden variant EDS/SEDS loads in edsparser-stats" || return 1
}

# ══════════════════════════════════════════════════════════════════════════════
# 4. SOURCE FORMATS — SEDS (-s) vs EDZ (-z), both sparse; l-EDS fallback
# ══════════════════════════════════════════════════════════════════════════════
# Both -s and -z always write sparse form (universal {0} entries omitted); there
# is no CLI-selectable dense mode. In l-EDS (-l) mode sources are always dense
# text SEDS regardless of -z (the merge pipeline has no sparse/EDZ writer).

test_seds_produces_seds_file() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/sp.eds" -s "$TMPDIR/sp.seds" 2>/dev/null
    assert_exit_code 0 $? "default (SEDS) exits 0" || return 1
    assert_file_exists "$TMPDIR/sp.seds" "default creates .seds file" || return 1
    assert_not_empty "$TMPDIR/sp.seds" "default .seds file non-empty" || return 1
}

test_seds_trailer_magic() {
    # SEDS_SPARSE now uses a 28-byte trailer carrying num_paths:
    #   bitvec | "SED2"(4) | card(8) | m_degen(8) | num_paths(8)
    # so the magic "SED2" (53454432 hex) sits 28 bytes from EOF.
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/magic.eds" -s "$TMPDIR/magic.seds" 2>/dev/null
    assert_exit_code 0 $? "default (SEDS) exits 0" || return 1
    local magic
    magic=$(tail -c 28 "$TMPDIR/magic.seds" | head -c 4 | xxd -p | tr -d '\n')
    if [ "$magic" != "53454432" ]; then
        echo -e "  ${RED}FAIL${NC}: SEDS_SPARSE trailer magic wrong: got $magic, want 53454432 (SED2)"
        return 1
    fi
}

test_seds_loadable_by_eds2leds() {
    # eds2leds -s loads the source file (auto-selects linear merge); corruption errors.
    "$LEDS" -i "$TMPDIR/sp.eds" -s "$TMPDIR/sp.seds" -l 1 -o "$TMPDIR/sp_l1.leds" 2>/dev/null
    assert_exit_code 0 $? "eds2leds with -s loads SEDS_SPARSE without error"
}

test_edz_produces_edz_file() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/edz_sp.eds" -s "$TMPDIR/edz_sp.edz" -z 2>/dev/null
    assert_exit_code 0 $? "-z exits 0" || return 1
    assert_file_exists "$TMPDIR/edz_sp.edz" "-z creates .edz file" || return 1
    assert_not_empty "$TMPDIR/edz_sp.edz" "-z .edz file non-empty" || return 1
}

test_edz_header_magic() {
    # EDZ files start with "EDZ\0" (4 bytes = 45445a00 hex).
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/edzmag.eds" -s "$TMPDIR/edzmag.edz" -z 2>/dev/null
    assert_exit_code 0 $? "-z exits 0" || return 1
    local magic
    magic=$(head -c 4 "$TMPDIR/edzmag.edz" | xxd -p | tr -d '\n')
    if [ "$magic" != "45445a00" ]; then
        echo -e "  ${RED}FAIL${NC}: EDZ_SPARSE header magic wrong: got $magic, want 45445a00"
        return 1
    fi
}

test_edz_flags_byte() {
    # EDZ_SPARSE flags (bytes 4-5, little-endian) must be 0x0006 = 0600 hex.
    local flags
    flags=$(dd if="$TMPDIR/edzmag.edz" bs=1 skip=4 count=2 2>/dev/null | xxd -p | tr -d '\n')
    if [ "$flags" != "0600" ]; then
        echo -e "  ${RED}FAIL${NC}: EDZ_SPARSE flags wrong: got $flags, want 0600"
        return 1
    fi
}

test_edz_loadable_by_eds2leds() {
    "$LEDS" -i "$TMPDIR/edz_sp.eds" -z "$TMPDIR/edz_sp.edz" -l 1 -o "$TMPDIR/edz_sp_l1.leds" 2>/dev/null
    assert_exit_code 0 $? "eds2leds with -z loads EDZ_SPARSE without error"
}

test_seds_and_edz_produce_same_eds() {
    # -s and -z only change the source container; .eds output must match.
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/cmp_seds.eds" -s "$TMPDIR/cmp_seds.seds" 2>/dev/null
    assert_exit_code 0 $? "vcf2eds (default) exits 0" || return 1
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/cmp_edz.eds" -s "$TMPDIR/cmp_edz.edz" -z 2>/dev/null
    assert_exit_code 0 $? "vcf2eds -z exits 0" || return 1
    assert_files_identical "$TMPDIR/cmp_seds.eds" "$TMPDIR/cmp_edz.eds" "-z EDS content matches default (-s) EDS"
}

test_seds_and_edz_stats_agree() {
    # Regression: complement encoding ({0,e1,..}) in SEDS vs bitset in EDZ must
    # expand to the same true path count in edsparser-stats.
    local out_seds out_edz
    out_seds=$("$STATS" -i "$TMPDIR/sp.eds" -s "$TMPDIR/sp.seds" -v)
    out_edz=$("$STATS" -i "$TMPDIR/sp.eds" -z "$TMPDIR/edz_sp.edz" -v)
    if [ "$out_seds" != "$out_edz" ]; then
        echo -e "  ${RED}FAIL${NC}: SEDS and EDZ verbose stats differ"
        diff <(echo "$out_seds") <(echo "$out_edz") | head -10 | sed 's/^/    /'
        return 1
    fi
}

test_long_flag_forms_seds_edz() {
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/lf_seds.eds" --seds "$TMPDIR/lf_seds.seds" 2>/dev/null
    assert_exit_code 0 $? "vcf2eds --seds exits 0" || return 1
    "$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/lf_edz.eds" --seds "$TMPDIR/lf_edz.edz" --edz 2>/dev/null
    assert_exit_code 0 $? "vcf2eds --edz exits 0" || return 1
}

test_leds_default_sources_are_dense() {
    "$TOOL" -i "$VCF" -r "$REF" -l 5 -o "$TMPDIR/leds_default.leds" -s "$TMPDIR/leds_default.seds" 2>/dev/null
    assert_exit_code 0 $? "vcf2eds -l 5 (default) exits 0" || return 1
    # Dense SEDS ends with the "SEDN" trailer (5345444e), not the sparse "SED2".
    local magic
    magic=$(tail -c 20 "$TMPDIR/leds_default.seds" | head -c 4 | xxd -p | tr -d '\n')
    if [ "$magic" != "5345444e" ]; then
        echo -e "  ${RED}FAIL${NC}: l-EDS default sources not dense SEDN trailer (got $magic)"
        return 1
    fi
}

test_leds_with_edz_flag_warns_and_falls_back() {
    # -z + -l must not corrupt output: warn on stderr, write valid dense text SEDS.
    local stderr_out
    stderr_out=$("$TOOL" -i "$VCF" -r "$REF" -l 5 -o "$TMPDIR/leds_edz.leds" \
        -s "$TMPDIR/leds_edz.edz" -z 2>&1 >/dev/null)
    assert_exit_code 0 $? "vcf2eds -l 5 -z exits 0" || return 1
    assert_contains "$stderr_out" "not supported in l-EDS" "warns that -z is ignored in l-EDS mode" || return 1

    # Must NOT start with EDZ magic — content is plain text SEDS despite .edz name.
    local magic
    magic=$(head -c 4 "$TMPDIR/leds_edz.edz" | xxd -p | tr -d '\n')
    if [ "$magic" == "45445a00" ]; then
        echo -e "  ${RED}FAIL${NC}: l-EDS -z output has EDZ magic bytes but pipeline can't write EDZ"
        return 1
    fi

    # Readable as plain SEDS text once given a .seds extension.
    cp "$TMPDIR/leds_edz.edz" "$TMPDIR/leds_edz_copy.seds"
    "$STATS" -i "$TMPDIR/leds_edz.leds" -s "$TMPDIR/leds_edz_copy.seds" >/dev/null 2>&1
    assert_exit_code 0 $? "l-EDS -z output is readable as plain SEDS text once renamed .seds"
}

test_leds_keep_eds_emits_intermediate() {
    # --keep-eds with -l must also write the stage-1 EDS/SEDS to <base>.eds/.seds,
    # and that EDS must be byte-identical to a plain (no -l) VCF→EDS run.
    local d="$TMPDIR/keep"
    mkdir -p "$d"
    # Plain EDS reference (dense SEDS to match the intermediate, which is always dense).
    "$TOOL" -i "$VCF" -r "$REF" -o "$d/plain.eds" -s "$d/plain.seds" >/dev/null 2>&1
    # l-EDS with --keep-eds; the intermediate is named from the INPUT stem
    # (small.vcf -> small.eds) next to the l-EDS output directory.
    "$TOOL" -i "$VCF" -r "$REF" -l 5 -o "$d/kept_l5.leds" -s "$d/kept_l5.seds" \
        --keep-eds >/dev/null 2>&1
    assert_exit_code 0 $? "vcf2eds -l 5 --keep-eds exits 0" || return 1
    assert_file_exists "$d/small.eds"  "intermediate <input>.eds emitted"  || return 1
    assert_file_exists "$d/small.seds" "intermediate <input>.seds emitted" || return 1
    assert_file_exists "$d/kept_l5.leds" "l-EDS still produced"            || return 1
    assert_files_identical "$d/small.eds" "$d/plain.eds" \
        "intermediate EDS is byte-identical to a plain VCF→EDS run" || return 1
}

test_keep_eds_without_l_warns() {
    # --keep-eds is a no-op without -l; it should warn, not error.
    local stderr_out
    stderr_out=$("$TOOL" -i "$VCF" -r "$REF" -o "$TMPDIR/nokl.eds" \
        -s "$TMPDIR/nokl.seds" --keep-eds 2>&1 >/dev/null)
    assert_exit_code 0 $? "vcf2eds --keep-eds without -l exits 0" || return 1
    assert_contains "$stderr_out" "no effect without -l" \
        "warns that --keep-eds is a no-op without -l" || return 1
}

# ══════════════════════════════════════════════════════════════════════════════
# RUN
# ══════════════════════════════════════════════════════════════════════════════

echo "-- basic conversion & errors --"
run_test "basic VCF→EDS conversion"                test_basic_conversion
run_test "sources file created"                    test_sources_file_created
run_test "output contains EDS brackets"            test_eds_format_valid
run_test "VCF→l-EDS with -l 5"                     test_leds_with_context_length
run_test "overlapping variants VCF"                test_overlapping_variants
run_test "missing reference file exits non-zero"   test_missing_reference_file_fails
run_test "missing input file exits non-zero"       test_missing_input_file_fails
run_test "no args exits non-zero"                  test_no_args_fails

echo "-- CLI arguments --"
run_test "--help exits 0 with usage"               test_help_long
run_test "-h exits 0 with usage"                   test_help_short
run_test "help documents every option"             test_help_lists_every_option
run_test "all short flags (-i -r -o -s -b)"        test_short_flags_all
run_test "all long flags (--input …)"              test_long_flags_all
run_test "short and long flags identical output"   test_short_and_long_flags_identical
run_test "--context-length long form"              test_context_length_long_form
run_test "default derives <stem>.eds/.seds"        test_default_eds_paths
run_test "default derives <stem>_l5.leds/.seds"    test_default_leds_paths
run_test "-o without -s derives .seds"             test_default_seds_next_to_output
run_test "-l 0 equals regular EDS"                 test_context_length_zero_is_regular_eds
run_test "block size: default == legacy (-b 0)"    test_block_size_default_equals_legacy
run_test "block size: large == default"            test_block_size_large_equals_default
run_test "block size: -b 1 still valid+parseable"  test_block_size_small_still_valid
run_test "block size: variant count invariant"     test_block_size_variant_count_invariant
run_test "non-.vcf extension exits 1"              test_non_vcf_extension_fails
run_test "invalid --block-size exits non-zero"     test_invalid_block_size_fails
run_test "invalid --context-length exits non-zero" test_invalid_context_length_fails
run_test "unknown option exits non-zero"           test_unknown_option_fails
run_test "missing --reference arg exits non-zero"  test_missing_required_reference_arg_fails
run_test "missing --input arg exits non-zero"      test_missing_required_input_arg_fails

echo "-- variant types & statistics --"
run_test "mixed-variant VCF runs successfully"     test_variants_runs_successfully
run_test "total variants counted (13)"             test_variants_total_counted
run_test "10 valid variants processed"             test_variants_processed_count
run_test "malformed line skipped (1)"              test_variants_malformed_skipped
run_test "unsupported SV <BND> skipped (1)"        test_variants_unsupported_sv_skipped
run_test "wrong-chromosome record skipped (1)"     test_variants_wrong_chrom_skipped
run_test "success rate reported (76.9%)"           test_variants_success_rate
run_test "EDS output matches golden"               test_variants_eds_matches_golden
run_test "SEDS output matches golden"              test_variants_seds_matches_golden
run_test "multi-allelic expands to {A,C,G}"        test_variants_multiallelic_expands
run_test "deletion yields empty-alt set"           test_variants_deletion_empty_alt
run_test "golden round-trips through stats"         test_variants_golden_roundtrips

echo "-- source formats (SEDS/-s vs EDZ/-z) --"
run_test "SEDS (default): creates .seds file"           test_seds_produces_seds_file
run_test "SEDS (default): trailer magic = 'SEDS'"       test_seds_trailer_magic
run_test "SEDS (default): loadable by eds2leds"         test_seds_loadable_by_eds2leds
run_test "EDZ (-z): creates .edz file"                  test_edz_produces_edz_file
run_test "EDZ (-z): header magic = 'EDZ\\0'"            test_edz_header_magic
run_test "EDZ (-z): flags byte = 0x0006"                test_edz_flags_byte
run_test "EDZ (-z): loadable by eds2leds (-z)"          test_edz_loadable_by_eds2leds
run_test "-s and -z produce same EDS content"           test_seds_and_edz_produce_same_eds
run_test "-s and -z produce same stats"                 test_seds_and_edz_stats_agree
run_test "--seds/--edz long flag forms"                 test_long_flag_forms_seds_edz
run_test "l-EDS (-l): default sources are dense"        test_leds_default_sources_are_dense
run_test "l-EDS (-l) + -z: warns and falls back safely" test_leds_with_edz_flag_warns_and_falls_back
run_test "l-EDS (-l) --keep-eds: emits intermediate EDS" test_leds_keep_eds_emits_intermediate
run_test "--keep-eds without -l: warns (no-op)"          test_keep_eds_without_l_warns

print_summary
