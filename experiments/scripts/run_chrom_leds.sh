#!/bin/bash
# Stage 2 worker: EDS → l-EDS for a single chromosome, all l-values.
# Called by run_leds.sh inside its own screen session.
# Usage: run_chrom_leds.sh <chrom>
#
# Expects stage-1 output at $OUT_BASE/eds/<chrom>.eds + .seds
# (produced by run_chrom_eds.sh / run_eds.sh).
#
# Env:
#   L_VALUES    space-separated list of context lengths (default "5 10 20 30 40 50 100").
#               Example: L_VALUES="3 5 8" ./run_leds.sh
#   FORCE       set to 1 to recompute outputs that already exist (default: skip them).
#   MAX_MEMORY  per-run pre-flight budget passed to `eds2leds --max-memory` (e.g. 100G;
#               empty = no pre-flight). eds2leds estimates worst-case merge RAM from
#               metadata BEFORE merging and exits 3 without doing any work if it would
#               exceed the budget; we treat exit 3 exactly like a memory-cap kill —
#               mark the chromosome too-intensive and stop its remaining l-values.
#   BLOCK_SIZE_LEDS  passed to `eds2leds --block-size` (e.g. 200M). Bounds peak RAM to
#               roughly one block instead of the whole file, at ~2.7x wall-clock. The
#               output is byte-identical to a whole-file run.

chrom="$1"
OUT_BASE="${OUT_BASE:-$HOME/raid_storage/Data/1000HGp3}"
EDS_DIR="$OUT_BASE/eds"
read -ra L_VALUES <<< "${L_VALUES:-5 10 20 30 40 50 100}"
MAX_MEMORY="${MAX_MEMORY-}"
BLOCK_SIZE_LEDS="${BLOCK_SIZE_LEDS-}"

# Optional per-run guards. --max-memory refuses (exit 3) before doing any work;
# --block-size caps how much of the input is resident at once.
extra_args=()
[[ -n "$MAX_MEMORY" ]]      && extra_args+=(--max-memory "$MAX_MEMORY")
[[ -n "$BLOCK_SIZE_LEDS" ]] && extra_args+=(--block-size "$BLOCK_SIZE_LEDS")

# run_leds.sh's memory cap drops this marker when it kills our eds2leds run; we then
# stop the remaining l-values so the whole chromosome can be retried later as one process.
TOO_INTENSIVE_DIR="${TOO_INTENSIVE_DIR:-$OUT_BASE/too_intensive}"
abort_flag="$TOO_INTENSIVE_DIR/${chrom}.abort"

in_eds="$EDS_DIR/${chrom}.eds"
in_seds="$EDS_DIR/${chrom}.seds"

if [[ ! -f "$in_eds" ]]; then
    echo "[chr${chrom}] EDS not found: $in_eds — run stage 1 first (run_eds.sh)" >&2
    exit 1
fi
if [[ ! -f "$in_seds" ]]; then
    echo "[chr${chrom}] SEDS not found: $in_seds — run stage 1 first (run_eds.sh)" >&2
    exit 1
fi

for l in "${L_VALUES[@]}"; do
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

    echo "[chr${chrom} l=${l}] START — $(date '+%Y-%m-%d %H:%M:%S')"
    # eds2leds auto-names the output SEDS as <out_leds_stem>.seds, so $out_seds
    # will be created at $out_dir/${chrom}.seds without any extra steps.
    eds2leds \
        -i "$in_eds" \
        -s "$in_seds" \
        -l "$l" \
        -o "$out_leds" \
        "${extra_args[@]}" \
        &> "$log_file"

    exit_code=$?
    if [[ $exit_code -eq 3 ]]; then
        # Exit 3 = --max-memory pre-flight refused this input. Nothing was written and
        # no expensive work was done. Same handling as a memory-cap kill: record it and
        # stop this chromosome so it can be retried deliberately (bigger budget,
        # BLOCK_SIZE_LEDS, or alone on the box).
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
        echo "[chr${chrom} l=${l}] DONE — $(date '+%Y-%m-%d %H:%M:%S')"
    fi
done

echo "[chr${chrom}] ALL L-VALUES DONE — $(date '+%Y-%m-%d %H:%M:%S')"
