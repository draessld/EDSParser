#!/bin/bash
# Stage 2 worker (incremental): build each l-EDS for one chromosome from the
# largest already-computed SMALLER l-EDS instead of from the raw EDS.
# Called by run_leds_incremental.sh inside its own screen session.
# Usage: run_chrom_leds_incremental.sh <chrom>
#
# Why this is correct: the context-merge constraint is monotone — a larger l
# merges a superset of the boundaries a smaller l merges. So T_l(EDS) can be
# obtained by re-merging any T_l'(EDS) with l' < l (e.g. l=100 from l=50). The
# result is equivalent to building from the raw EDS, but starts from a much
# smaller, already-partly-merged input, so it is faster. The raw EDS is used
# only for the smallest l (or when no smaller l-EDS exists yet).
#
# Expects stage-1 output at $OUT_BASE/eds/<chrom>.eds + .seds
# (produced by run_chrom_eds.sh / run_eds.sh).
#
# Env:
#   L_VALUES   space-separated list of context lengths (default "5 10 20 30 40 50 100").
#              Processed in ascending order regardless of how they are listed, so
#              each l can reuse the smaller ones built earlier in the same run.

chrom="$1"
OUT_BASE="${OUT_BASE:-$HOME/raid_storage/Data/1000HGp3}"
EDS_DIR="$OUT_BASE/eds"
read -ra L_VALUES <<< "${L_VALUES:-5 10 20 30 40 50 100}"
MAX_MEMORY="${MAX_MEMORY-}"
BLOCK_SIZE_LEDS="${BLOCK_SIZE_LEDS-}"

# Optional per-run guards (same semantics as run_chrom_leds.sh):
#   MAX_MEMORY       -> eds2leds --max-memory: refuse (exit 3) before doing any work
#   BLOCK_SIZE_LEDS  -> eds2leds --block-size: bound peak RAM to ~one block
extra_args=()
[[ -n "$MAX_MEMORY" ]]      && extra_args+=(--max-memory "$MAX_MEMORY")
[[ -n "$BLOCK_SIZE_LEDS" ]] && extra_args+=(--block-size "$BLOCK_SIZE_LEDS")

# run_leds_incremental.sh's memory cap drops this marker when it kills our eds2leds
# run; we then stop the remaining l-values so the chromosome can be retried later.
TOO_INTENSIVE_DIR="${TOO_INTENSIVE_DIR:-$OUT_BASE/too_intensive}"
abort_flag="$TOO_INTENSIVE_DIR/${chrom}.abort"

base_eds="$EDS_DIR/${chrom}.eds"
base_seds="$EDS_DIR/${chrom}.seds"

# eds2leds requires the -i input to have a .eds extension, but our incremental base
# is a .leds file (byte-for-byte the same format). Feed it via a temporary .eds
# symlink so no large file is copied. Cleaned up on exit.
TMP_LINK_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_LINK_DIR"' EXIT
eds_input_for() {  # echo a .eds path for $1, symlinking if it is a .leds
    local path="$1"
    if [[ "$path" == *.eds ]]; then printf '%s' "$path"; return; fi
    local link="$TMP_LINK_DIR/$(basename "${path%.*}").eds"
    ln -sf "$path" "$link"
    printf '%s' "$link"
}

if [[ ! -f "$base_eds" ]]; then
    echo "[chr${chrom}] EDS not found: $base_eds — run stage 1 first (run_eds.sh)" >&2
    exit 1
fi
if [[ ! -f "$base_seds" ]]; then
    echo "[chr${chrom}] SEDS not found: $base_seds — run stage 1 first (run_eds.sh)" >&2
    exit 1
fi

# Incremental reuse requires ascending l so smaller results exist before larger ones.
IFS=$'\n' read -rd '' -a L_SORTED < <(printf '%s\n' "${L_VALUES[@]}" | sort -n && printf '\0')

for l in "${L_SORTED[@]}"; do
    if [[ -f "$abort_flag" ]]; then
        echo "[chr${chrom}] ABORT — marked too intensive by memory cap, stopping remaining l-values ($abort_flag)" >&2
        exit 2
    fi

    out_dir="$OUT_BASE/leds_l${l}"
    mkdir -p "$out_dir"

    out_leds="$out_dir/${chrom}.leds"
    out_seds="$out_dir/${chrom}.seds"
    log_file="$out_dir/${chrom}_l${l}.out"

    # -s, not -f: a run killed mid-write (memory cap, SIGKILL, crashed screen) leaves
    # zero-byte .leds/.seds behind, and -f would treat that wreckage as finished work —
    # which is exactly how a whole batch can "complete" in seconds without doing
    # anything. FORCE=1 recomputes even complete outputs.
    if [[ -z "${FORCE:-}" && -s "$out_leds" && -s "$out_seds" ]]; then
        echo "[chr${chrom} l=${l}] SKIP (output exists)"
        continue
    fi
    # Anything left here is either incomplete (a run killed mid-write leaves zero-byte
    # files) or a forced redo: clear it and fall through to recompute.
    if [[ -e "$out_leds" || -e "$out_seds" ]]; then
        echo "[chr${chrom} l=${l}] REDO (clearing incomplete or forced output)"
        rm -f "$out_leds" "$out_seds"
    fi

    # Pick the base: the largest already-computed l-EDS with l' < l, else raw EDS.
    in_eds="$base_eds"; in_seds="$base_seds"; base_desc="EDS"
    for (( j=${#L_SORTED[@]}-1; j>=0; j-- )); do
        pl="${L_SORTED[j]}"
        (( pl < l )) || continue
        cand_leds="$OUT_BASE/leds_l${pl}/${chrom}.leds"
        cand_seds="$OUT_BASE/leds_l${pl}/${chrom}.seds"
        if [[ -f "$cand_leds" && -f "$cand_seds" ]]; then
            in_eds="$cand_leds"; in_seds="$cand_seds"; base_desc="l=${pl}"
            break
        fi
    done

    echo "[chr${chrom} l=${l}] START (from ${base_desc}) — $(date '+%Y-%m-%d %H:%M:%S')"
    # eds2leds auto-names the output SEDS as <out_leds_stem>.seds, so $out_seds
    # will be created at $out_dir/${chrom}.seds without any extra steps.
    eds2leds \
        -i "$(eds_input_for "$in_eds")" \
        -s "$in_seds" \
        -l "$l" \
        -o "$out_leds" \
        "${extra_args[@]}" \
        &> "$log_file"

    exit_code=$?
    if [[ $exit_code -eq 3 ]]; then
        # --max-memory pre-flight refused this input; nothing was written and no
        # expensive work was done. Mark too-intensive and stop this chromosome.
        mkdir -p "$TOO_INTENSIVE_DIR"
        touch "$abort_flag"
        echo "$(date '+%Y-%m-%d %H:%M:%S') chr${chrom} l=${l} refused by --max-memory pre-flight (${MAX_MEMORY})" \
            >> "$TOO_INTENSIVE_DIR/too_intensive.log"
        echo "[chr${chrom} l=${l}] TOO INTENSIVE — refused by --max-memory pre-flight (${MAX_MEMORY}), see $log_file" >&2
        rm -f "$out_leds" "$out_seds"
        exit 3
    elif [[ $exit_code -ne 0 ]]; then
        echo "[chr${chrom} l=${l}] FAIL — exit code $exit_code — see $log_file" >&2
        rm -f "$out_leds" "$out_seds"
    else
        echo "[chr${chrom} l=${l}] DONE (from ${base_desc}) — $(date '+%Y-%m-%d %H:%M:%S')"
    fi
done

echo "[chr${chrom}] ALL L-VALUES DONE — $(date '+%Y-%m-%d %H:%M:%S')"
