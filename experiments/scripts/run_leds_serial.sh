#!/bin/bash
# Stage 2 (serial, l-major): build all l-EDS one process at a time.
#
# Order: ALL chromosomes at the smallest l first, then all at the next l, etc.
#   l=5:  chr1 chr2 ... chrN
#   l=10: chr1 chr2 ... chrN
#   l=20: ...
#
# No screens, no parallelism, no memory cap: exactly one eds2leds runs at a time,
# so total RAM is just that single process. Run inside a screen so it survives a
# disconnect, e.g.:  screen -S run_leds_serial ./run_leds_serial.sh
#
# Two build modes — both produce byte-identical output (verified by
# tests/e2e/test_leds_incremental.sh across cartesian, linear, fragmented-context
# and randomized inputs):
#   INCREMENTAL=1 (DEFAULT) — build each l from the largest already-computed SMALLER
#                      l-EDS (raw EDS only for the smallest l). Faster: re-merges a
#                      small, already-partly-merged input instead of the full EDS.
#   INCREMENTAL=0    — build every l straight from the raw EDS. Slower; use only if
#                      you want each l independent of the others' on-disk outputs.
#
# (Equivalence relies on the fix in eds_transforms.cpp that measures "short context"
# against the whole contiguous common run, not a single fragmented common symbol;
# before that fix incremental and from-EDS could differ on VCF/MSA-shaped EDS.)
#
# Env:
#   L_VALUES     space-separated context lengths (default "5 10 20 30 40 50 100");
#                always processed in ascending order regardless of listing.
#   CHROMS       optional space-separated subset of chromosomes (default: every
#                <chrom>.eds in the EDS dir, natural-sorted).
#   INCREMENTAL  1 = reuse smaller l-EDS as the base (default); 0 = always from EDS.
#   MAX_MEMORY   pre-flight budget passed to `eds2leds --max-memory` (default 450G).
#                eds2leds estimates worst-case merge RAM from metadata BEFORE merging
#                and refuses (exit 3) if it would exceed this — so a too-heavy
#                chromosome is marked too-intensive and skipped without ever starting
#                the expensive work. Set empty to disable the pre-flight guard.
#   MEM_KILL_GB  runtime backstop: if total *used* RAM reaches this while a build runs
#                (e.g. the estimate under-counted), kill it and mark the chromosome
#                too-intensive (default 450).
#   MEM_CHECK_INTERVAL  seconds between memory polls while a build runs (default 15).

set -u

INCREMENTAL="${INCREMENTAL:-1}"
MAX_MEMORY="${MAX_MEMORY-450G}"   # note: MAX_MEMORY= (empty) disables the pre-flight
MEM_KILL_GB="${MEM_KILL_GB:-450}"
MEM_CHECK_INTERVAL="${MEM_CHECK_INTERVAL:-15}"

OUT_BASE="${OUT_BASE:-$HOME/raid_storage/Data/1000HGp3}"
EDS_DIR="${EDS_DIR:-$OUT_BASE/eds}"
read -ra L_VALUES <<< "${L_VALUES:-5 10 20 30 40 50 100}"

# Ascending l so each build can reuse the smaller results produced earlier.
IFS=$'\n' read -rd '' -a L_SORTED < <(printf '%s\n' "${L_VALUES[@]}" | sort -n && printf '\0')

# Chromosome list: explicit CHROMS, else every <chrom>.eds, natural-sorted (chr2 < chr10).
if [[ -n "${CHROMS:-}" ]]; then
    read -ra CHROM_LIST <<< "$CHROMS"
else
    shopt -s nullglob
    eds_files=("$EDS_DIR"/*.eds)
    shopt -u nullglob
    if (( ${#eds_files[@]} == 0 )); then
        echo "No EDS files in $EDS_DIR — run run_eds.sh first" >&2
        exit 1
    fi
    CHROM_LIST=()
    while IFS= read -r f; do
        CHROM_LIST+=("$(basename "$f" .eds)")
    done < <(printf '%s\n' "${eds_files[@]}" | sort -V)
fi

echo "=== EDS → l-EDS serial run (l-major) — $(date '+%Y-%m-%d %H:%M:%S') ==="
echo "    l-values (ascending): ${L_SORTED[*]}"
echo "    chromosomes: ${CHROM_LIST[*]}"
if (( INCREMENTAL )); then
    echo "    mode: incremental (build l from largest smaller l-EDS — equivalent to canonical, faster)"
else
    echo "    mode: from-EDS (every l built from raw EDS)"
fi
if [[ -n "$MAX_MEMORY" ]]; then
    echo "    memory pre-flight: refuse + mark too-intensive if est. merge RAM > ${MAX_MEMORY} (eds2leds --max-memory)"
else
    echo "    memory pre-flight: disabled (MAX_MEMORY empty)"
fi
echo "    memory backstop: kill + mark too-intensive at used >= ${MEM_KILL_GB} GiB (poll every ${MEM_CHECK_INTERVAL}s)"

# eds2leds requires the -i input to have a .eds extension, but our incremental base
# is a .leds file (byte-for-byte the same format). Feed it through a temporary .eds
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

# Total *used* RAM in GiB (the 'used' column, excludes buff/cache).
used_gb() { free -g | awk '/^Mem:/{print $3+0}'; }

# Chromosomes killed by the memory cap are recorded here; a per-chrom marker makes
# the l-major loop skip that chromosome's remaining l-values, and the log is the
# list to retry later with more resources / special handling.
TOO_INTENSIVE_DIR="$OUT_BASE/too_intensive"
TOO_INTENSIVE_LOG="$TOO_INTENSIVE_DIR/too_intensive.log"

fail_count=0
killed_count=0

for l in "${L_SORTED[@]}"; do
    out_dir="$OUT_BASE/leds_l${l}"
    mkdir -p "$out_dir"
    echo "--- l=${l} — $(date '+%Y-%m-%d %H:%M:%S') ---"

    for chrom in "${CHROM_LIST[@]}"; do
        # Skip chromosomes a previous l killed for exceeding the memory cap.
        if [[ -f "$TOO_INTENSIVE_DIR/${chrom}.abort" ]]; then
            echo "[chr${chrom} l=${l}] SKIP — marked too-intensive ($TOO_INTENSIVE_DIR/${chrom}.abort)"
            continue
        fi

        base_eds="$EDS_DIR/${chrom}.eds"
        base_seds="$EDS_DIR/${chrom}.seds"

        if [[ ! -f "$base_eds" || ! -f "$base_seds" ]]; then
            echo "[chr${chrom} l=${l}] SKIP — missing stage-1 EDS/SEDS ($base_eds)" >&2
            continue
        fi

        out_leds="$out_dir/${chrom}.leds"
        out_seds="$out_dir/${chrom}.seds"
        log_file="$out_dir/${chrom}_l${l}.out"

        if [[ -f "$out_leds" && -f "$out_seds" ]]; then
            echo "[chr${chrom} l=${l}] SKIP (output exists)"
            continue
        fi

        # Safe mode: always build from the raw EDS (canonical output).
        # Incremental mode: base = largest already-computed l-EDS with l' < l.
        in_eds="$base_eds"; in_seds="$base_seds"; base_desc="EDS"
        if (( INCREMENTAL )); then
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
        fi

        echo "[chr${chrom} l=${l}] START (from ${base_desc}) — $(date '+%Y-%m-%d %H:%M:%S')"
        # --max-memory makes eds2leds estimate worst-case merge RAM up front and exit
        # 3 without doing any work if it would blow the budget; we treat that exactly
        # like a runtime kill (mark too-intensive, skip the rest of this chromosome).
        mem_args=()
        [[ -n "$MAX_MEMORY" ]] && mem_args=(--max-memory "$MAX_MEMORY")

        # Run eds2leds in the background so we can also watch total RAM while it works
        # and kill it if the box gets close to full (backstop for an under-estimate).
        # eds2leds auto-names the output SEDS as <out_leds_stem>.seds.
        eds2leds \
            -i "$(eds_input_for "$in_eds")" \
            -s "$in_seds" \
            -l "$l" \
            -o "$out_leds" \
            "${mem_args[@]}" \
            &> "$log_file" &
        eds_pid=$!

        killed_mem=0
        while kill -0 "$eds_pid" 2>/dev/null; do
            used=$(used_gb); used=${used:-0}
            if (( used >= MEM_KILL_GB )); then
                echo "[MEMCAP] used=${used}GiB >= ${MEM_KILL_GB} — killing chr${chrom} l=${l} (pid ${eds_pid})" >&2
                kill -TERM "$eds_pid" 2>/dev/null
                for _ in 1 2 3 4 5; do kill -0 "$eds_pid" 2>/dev/null || break; sleep 1; done
                kill -KILL "$eds_pid" 2>/dev/null
                killed_mem=1
                break
            fi
            sleep "$MEM_CHECK_INTERVAL"
        done
        wait "$eds_pid" 2>/dev/null
        exit_code=$?

        # exit 3 = eds2leds' pre-flight memory guard refused before doing any work.
        if (( killed_mem )) || [[ $exit_code -eq 3 ]]; then
            mkdir -p "$TOO_INTENSIVE_DIR"
            touch "$TOO_INTENSIVE_DIR/${chrom}.abort"
            if (( killed_mem )); then
                reason="killed at ${used}GiB (runtime)"
            else
                reason="refused by --max-memory pre-flight (est. > ${MAX_MEMORY})"
            fi
            echo "$(date '+%Y-%m-%d %H:%M:%S') chr${chrom} l=${l} too-intensive: ${reason}" >> "$TOO_INTENSIVE_LOG"
            echo "[chr${chrom} l=${l}] TOO INTENSIVE — ${reason}, marked for later; skipping this chromosome's remaining l-values" >&2
            rm -f "$out_leds" "$out_seds"
            (( killed_count++ ))
        elif [[ $exit_code -ne 0 ]]; then
            echo "[chr${chrom} l=${l}] FAIL — exit code $exit_code — see $log_file" >&2
            rm -f "$out_leds" "$out_seds"
            (( fail_count++ ))
        else
            echo "[chr${chrom} l=${l}] DONE (from ${base_desc}) — $(date '+%Y-%m-%d %H:%M:%S')"
        fi
    done

    echo "--- l=${l} complete — $(date '+%Y-%m-%d %H:%M:%S') ---"
done

echo "=== SERIAL RUN DONE — $(date '+%Y-%m-%d %H:%M:%S') — ${fail_count} failure(s), ${killed_count} too-intensive ==="
echo "l-EDS files in: $OUT_BASE/leds_l*/"
if (( killed_count )); then
    echo "too-intensive chromosomes (killed at memory cap) listed in: $TOO_INTENSIVE_LOG"
    echo "  retry one with more resources after clearing its marker, e.g.:"
    echo "  rm $TOO_INTENSIVE_DIR/<chrom>.abort && CHROMS=<chrom> $0"
fi
(( fail_count == 0 ))
