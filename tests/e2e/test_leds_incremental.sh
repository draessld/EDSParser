#!/bin/bash
# Guards the invariant behind the incremental l-EDS build (INCREMENTAL=1 in
# run_leds_serial.sh / run_chrom_leds_incremental.sh): building l=B by re-merging an
# already-computed l=A output (A < B) MUST be byte-identical (.leds and .seds) to
# building l=B straight from the raw EDS.
#
# This held for cartesian/single-context inputs but used to FAIL when the context
# between degenerates was fragmented into several adjacent common symbols — exactly
# what VCF/MSA-derived EDS look like. Root cause: needs_merge() judged "short
# context" against a single common symbol's length, so a short fragment made the
# greedy chain bridge across an otherwise long-enough context. Fixed by measuring
# the whole contiguous common-run length (eds_transforms.cpp select_merge_groups /
# needs_merge). These cases lock that fix in; if one regresses, the invariant broke
# again.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/helpers.sh"

DATA_DIR="$SCRIPT_DIR/data"
TOOL=$(find_tool "eds2leds") || { echo "ERROR: eds2leds not found"; exit 1; }
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== eds2leds incremental l-EDS characterisation ==="

# eds2leds requires a .eds input extension; a .leds base is the same format, so we
# feed it through a .eds symlink — exactly what the incremental runners do.
run_leds() {  # <in_eds_or_leds> <seds|-> <l> <out_leds>
    local in="$1" seds="$2" l="$3" out="$4" in_eds="$1"
    if [[ "$in" != *.eds ]]; then
        in_eds="$TMPDIR/$(basename "${in%.*}").as_eds.eds"
        ln -sf "$in" "$in_eds"
    fi
    if [[ "$seds" == "-" ]]; then
        "$TOOL" -i "$in_eds" -l "$l" -o "$out" >/dev/null 2>&1
    else
        "$TOOL" -i "$in_eds" -s "$seds" -l "$l" -o "$out" >/dev/null 2>&1
    fi
}

# Build l=<last> both ways (direct-from-EDS vs incremental chain) and echo the
# result of comparing them: "same" or "differ" for .leds, likewise for .seds.
#   $1 tag  $2 input.eds  $3 seds|-  $4.. ascending l chain (last = target)
# Sets globals: CMP_LEDS, CMP_SEDS (each "same"/"differ"/"n/a").
build_and_compare() {
    local tag="$1" eds="$2" seds="$3"; shift 3
    local chain=("$@")
    local target="${chain[-1]}"
    local has_src=1; [[ "$seds" == "-" ]] && has_src=0

    local direct="$TMPDIR/${tag}_direct.leds"
    run_leds "$eds" "$seds" "$target" "$direct" || { CMP_LEDS=err; CMP_SEDS=err; return 1; }

    local prev_leds="$eds" prev_seds="$seds" cur_leds l
    for l in "${chain[@]}"; do
        cur_leds="$TMPDIR/${tag}_inc_l${l}.leds"
        run_leds "$prev_leds" "$prev_seds" "$l" "$cur_leds" || { CMP_LEDS=err; CMP_SEDS=err; return 1; }
        prev_leds="$cur_leds"
        (( has_src )) && prev_seds="${cur_leds%.leds}.seds"
    done

    cmp -s "$direct" "$prev_leds" && CMP_LEDS=same || CMP_LEDS=differ
    if (( has_src )); then
        cmp -s "${direct%.leds}.seds" "$prev_seds" && CMP_SEDS=same || CMP_SEDS=differ
    else
        CMP_SEDS=n/a
    fi
    return 0
}

# --- Equivalence cases: incremental MUST match direct -------------------------

# Cartesian (no sources): simple.eds merges further at l=5 than l=3.
test_cartesian_equiv() {
    build_and_compare "cartesian" "$DATA_DIR/simple.eds" "-" 3 5 || return 1
    [[ "$CMP_LEDS" == same ]] || { echo -e "  ${RED}FAIL${NC}: cartesian .leds $CMP_LEDS (expected same)"; return 1; }
}

# Multi-pass merge input, no sources: fully merged by l=3, longer chain is a no-op.
test_iterative_equiv() {
    build_and_compare "iterative" "$DATA_DIR/test_iterative.eds" "-" 3 5 10 || return 1
    [[ "$CMP_LEDS" == same ]] || { echo -e "  ${RED}FAIL${NC}: iterative .leds $CMP_LEDS (expected same)"; return 1; }
}

# Single common symbol between degenerates, with sources: no fragmentation, so
# incremental still matches direct (guards that plain sourced reuse is fine).
test_linear_unified_context_equiv() {
    printf '{A,C}TAAGCTTACGATCGATCG{A,C}' > "$TMPDIR/uni.eds"
    printf '{1}{2}{0}{1}{2}' > "$TMPDIR/uni.seds"
    build_and_compare "uni" "$TMPDIR/uni.eds" "$TMPDIR/uni.seds" 5 10 || return 1
    [[ "$CMP_LEDS" == same && "$CMP_SEDS" == same ]] || {
        echo -e "  ${RED}FAIL${NC}: unified-context .leds=$CMP_LEDS .seds=$CMP_SEDS (expected same/same)"; return 1; }
}

# --- Regression cases: the fragmented-context bug that used to diverge --------

# Fragmented context (11+1+6 adjacent commons) with sources: used to over-merge in
# direct at l=10 (bridging the 18-char run); must now match incremental exactly.
test_linear_fragmented_context_equiv() {
    printf '{A,C}TAAGCTTACGA{T}CGATCG{A,C}' > "$TMPDIR/frag.eds"
    printf '{1}{2}{0}{0}{0}{1}{2}' > "$TMPDIR/frag.seds"
    build_and_compare "frag" "$TMPDIR/frag.eds" "$TMPDIR/frag.seds" 5 10 || return 1
    [[ "$CMP_LEDS" == same && "$CMP_SEDS" == same ]] || {
        echo -e "  ${RED}FAIL${NC}: fragmented-context .leds=$CMP_LEDS .seds=$CMP_SEDS (expected same/same)"; return 1; }
}

# small.eds is the real-world-shaped fixture with fragmented contexts + phasing.
test_small_eds_equiv() {
    build_and_compare "small" "$DATA_DIR/small.eds" "$DATA_DIR/small.seds" 3 5 10 || return 1
    [[ "$CMP_LEDS" == same && "$CMP_SEDS" == same ]] || {
        echo -e "  ${RED}FAIL${NC}: small.eds .leds=$CMP_LEDS .seds=$CMP_SEDS (expected same/same)"; return 1; }
}

run_test "equiv: cartesian incremental == direct"             test_cartesian_equiv
run_test "equiv: iterative incremental == direct"             test_iterative_equiv
run_test "equiv: linear single-context incremental == direct" test_linear_unified_context_equiv
run_test "equiv: linear fragmented-context incremental == direct" test_linear_fragmented_context_equiv
run_test "equiv: small.eds incremental == direct"             test_small_eds_equiv

print_summary
