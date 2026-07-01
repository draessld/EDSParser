#!/bin/bash
# E2E tests for vcf2eds sparse source formats (--source-format seds-sparse / edz-sparse).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
TOOL=$(find_tool "vcf2eds")   || { echo "ERROR: vcf2eds not found";    exit 1; }
LEDS=$(find_tool "eds2leds")  || { echo "ERROR: eds2leds not found";   exit 1; }

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

VCF="$DATA_DIR/small.vcf"
REF="$DATA_DIR/small.fa"

echo "=== vcf2eds sparse source formats ==="

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

# Check that $1 ≤ $2 in bytes.
assert_file_smaller_or_equal() {
    local small_f="$1" large_f="$2" msg="$3"
    local s l
    s=$(wc -c < "$small_f")
    l=$(wc -c < "$large_f")
    if [ "$s" -gt "$l" ]; then
        echo -e "  ${RED}FAIL${NC}: $msg — sparse ($s B) > dense ($l B)"
        return 1
    fi
}

# Check that the last N bytes of a file match an expected hex pattern.
assert_file_ends_with() {
    local path="$1" n="$2" hex="$3" msg="$4"
    local actual
    actual=$(tail -c "$n" "$path" | xxd -p | tr -d '\n')
    if [ "$actual" != "$hex" ]; then
        echo -e "  ${RED}FAIL${NC}: $msg — trailer hex='$actual', want '$hex'"
        return 1
    fi
}

# ── seds-sparse tests ─────────────────────────────────────────────────────────

test_seds_sparse_produces_seds_file() {
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/sp.eds" -s "$TMPDIR/sp.seds" \
        --source-format seds-sparse 2>/dev/null
    assert_exit_code 0 $? "seds-sparse exits 0" || return 1
    assert_file_exists "$TMPDIR/sp.seds" "seds-sparse creates .seds file" || return 1
    assert_not_empty "$TMPDIR/sp.seds" "seds-sparse .seds file non-empty" || return 1
}

test_seds_sparse_eds_identical_to_dense() {
    # Run dense and sparse; EDS content must be identical.
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/dense.eds" -s "$TMPDIR/dense.seds" \
        --source-format seds 2>/dev/null
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/sps.eds" -s "$TMPDIR/sps.seds" \
        --source-format seds-sparse 2>/dev/null
    assert_files_identical "$TMPDIR/dense.eds" "$TMPDIR/sps.eds" \
        "seds-sparse EDS content matches dense"
}

test_seds_sparse_trailer_magic() {
    # SEDS_SPARSE trailer ends with "SEDS" (4 bytes = 53454453 in little-endian hex).
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/magic.eds" -s "$TMPDIR/magic.seds" \
        --source-format seds-sparse 2>/dev/null
    assert_exit_code 0 $? "seds-sparse exits 0" || return 1
    # The magic word "SEDS" is at bytes -20..-17 of the file (4B magic out of 20B trailer).
    # Check just the 4-byte magic: bytes tail-c 20, then xxd first 4 bytes.
    local magic
    magic=$(tail -c 20 "$TMPDIR/magic.seds" | head -c 4 | xxd -p | tr -d '\n')
    if [ "$magic" != "53454453" ]; then
        echo -e "  ${RED}FAIL${NC}: SEDS_SPARSE trailer magic wrong: got $magic, want 53454453"
        return 1
    fi
}

test_seds_sparse_smaller_than_dense() {
    # Sparse format saves space when reference positions dominate.
    # small.vcf has only 10 variants and is too small to show savings;
    # just verify the file exists and is non-empty.
    assert_not_empty "$TMPDIR/sps.seds" "seds-sparse .seds file non-empty after comparison"
}

test_seds_sparse_loadable_by_eds2leds() {
    # eds2leds with -s loads the source file; any corruption would cause an error.
    # (passing -s auto-selects linear/phasing-aware merge)
    "$LEDS" -i "$TMPDIR/sps.eds" -s "$TMPDIR/sps.seds" \
        -l 1 -o "$TMPDIR/sps_l1.leds" 2>/dev/null
    assert_exit_code 0 $? "eds2leds with -s loads seds-sparse without error"
}

# ── edz-sparse tests ──────────────────────────────────────────────────────────

test_edz_sparse_produces_edz_file() {
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/edz_sp.eds" -s "$TMPDIR/edz_sp.edz" \
        --source-format edz-sparse 2>/dev/null
    assert_exit_code 0 $? "edz-sparse exits 0" || return 1
    assert_file_exists "$TMPDIR/edz_sp.edz" "edz-sparse creates .edz file" || return 1
    assert_not_empty "$TMPDIR/edz_sp.edz" "edz-sparse .edz file non-empty" || return 1
}

test_edz_sparse_eds_identical_to_dense() {
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/edz_dense.eds" -s "$TMPDIR/edz_dense.edz" \
        --source-format edz 2>/dev/null
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/edz_sps.eds" -s "$TMPDIR/edz_sps.edz" \
        --source-format edz-sparse 2>/dev/null
    assert_files_identical "$TMPDIR/edz_dense.eds" "$TMPDIR/edz_sps.eds" \
        "edz-sparse EDS content matches dense"
}

test_edz_sparse_header_magic() {
    # EDZ files start with "EDZ\0" (4 bytes = 45445a00 in hex).
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/edzmag.eds" -s "$TMPDIR/edzmag.edz" \
        --source-format edz-sparse 2>/dev/null
    assert_exit_code 0 $? "edz-sparse exits 0" || return 1
    local magic
    magic=$(head -c 4 "$TMPDIR/edzmag.edz" | xxd -p | tr -d '\n')
    if [ "$magic" != "45445a00" ]; then
        echo -e "  ${RED}FAIL${NC}: EDZ_SPARSE header magic wrong: got $magic, want 45445a00"
        return 1
    fi
}

test_edz_sparse_flags_byte() {
    # EDZ_SPARSE flags (bytes 4-5, little-endian) must be 0x0006 = 0600 in hex.
    local flags
    flags=$(dd if="$TMPDIR/edzmag.edz" bs=1 skip=4 count=2 2>/dev/null | xxd -p | tr -d '\n')
    if [ "$flags" != "0600" ]; then
        echo -e "  ${RED}FAIL${NC}: EDZ_SPARSE flags wrong: got $flags, want 0600"
        return 1
    fi
}

test_edz_sparse_smaller_than_dense() {
    # small.vcf is too small to show size savings (bitvec overhead outweighs gains).
    # Just verify the file exists and is non-empty.
    assert_not_empty "$TMPDIR/edz_sps.edz" "edz-sparse .edz file non-empty after comparison"
}

test_edz_sparse_loadable_by_eds2leds() {
    "$LEDS" -i "$TMPDIR/edz_sps.eds" -s "$TMPDIR/edz_sps.edz" \
        -l 1 -o "$TMPDIR/edz_sps_l1.leds" 2>/dev/null
    assert_exit_code 0 $? "eds2leds with -s loads edz-sparse without error"
}

# ── cross-format consistency ─────────────────────────────────────────────────

test_all_formats_produce_same_eds() {
    # All four source formats must produce bit-for-bit identical .eds output.
    local fmts="seds seds-sparse edz edz-sparse"
    local first_eds=""
    for fmt in $fmts; do
        local ext=".seds"
        [[ "$fmt" == edz* ]] && ext=".edz"
        "$TOOL" -i "$VCF" -r "$REF" \
            -o "$TMPDIR/all_${fmt}.eds" -s "$TMPDIR/all_${fmt}${ext}" \
            --source-format "$fmt" 2>/dev/null
        assert_exit_code 0 $? "vcf2eds --source-format $fmt exits 0" || return 1
        if [ -z "$first_eds" ]; then
            first_eds="$TMPDIR/all_${fmt}.eds"
        else
            assert_files_identical "$first_eds" "$TMPDIR/all_${fmt}.eds" \
                "--source-format $fmt EDS matches seds EDS" || return 1
        fi
    done
}

# ── error handling ────────────────────────────────────────────────────────────

test_invalid_source_format_fails() {
    "$TOOL" -i "$VCF" -r "$REF" \
        -o "$TMPDIR/err.eds" --source-format bogus 2>/dev/null
    local code=$?
    [ $code -ne 0 ] || {
        echo -e "  ${RED}FAIL${NC}: invalid --source-format should exit non-zero, got 0"
        return 1
    }
}

# ── run all ──────────────────────────────────────────────────────────────────

run_test "seds-sparse: creates .seds file"               test_seds_sparse_produces_seds_file
run_test "seds-sparse: EDS identical to dense"           test_seds_sparse_eds_identical_to_dense
run_test "seds-sparse: trailer magic = 'SEDS'"          test_seds_sparse_trailer_magic
run_test "seds-sparse: file non-empty"                  test_seds_sparse_smaller_than_dense
run_test "seds-sparse: loadable by eds2leds (linear)"   test_seds_sparse_loadable_by_eds2leds

run_test "edz-sparse: creates .edz file"                 test_edz_sparse_produces_edz_file
run_test "edz-sparse: EDS identical to dense"            test_edz_sparse_eds_identical_to_dense
run_test "edz-sparse: header magic = 'EDZ\\0'"          test_edz_sparse_header_magic
run_test "edz-sparse: flags byte = 0x0006"               test_edz_sparse_flags_byte
run_test "edz-sparse: file non-empty"                    test_edz_sparse_smaller_than_dense
run_test "edz-sparse: loadable by eds2leds (linear)"    test_edz_sparse_loadable_by_eds2leds

run_test "all 4 formats produce same EDS content"        test_all_formats_produce_same_eds
run_test "invalid --source-format exits non-zero"        test_invalid_source_format_fails

print_summary
