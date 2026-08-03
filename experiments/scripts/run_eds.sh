#!/bin/bash
# Stage 1 master: spawns one screen per chromosome to run VCF → EDS.
# Run this inside a screen session, e.g.:  screen -S run_eds
#
# Options:
#   --force   Overwrite existing EDS output instead of skipping.
#
# Env:
#   OUT_BASE     dataset root (default $HOME/raid_storage/Data/1000HGp3); VCF_DIR,
#                REF_DIR and OUT_DIR default to <OUT_BASE>/{vcf,ref,eds} and can each
#                be overridden individually.
#   BLOCK_SIZE   vcf2eds genomic window in bases (default 200000). Controls peak
#                RAM per process; smaller = less memory. All chromosomes run in
#                parallel, so keep this small for high-sample VCFs (see
#                run_chrom_eds.sh). Example: BLOCK_SIZE=1000000 ./run_eds.sh
#
# After all chromosomes finish, run run_leds.sh to do EDS → l-EDS.

force_flag=""
for arg in "$@"; do
    [[ "$arg" == "--force" ]] && force_flag="--force"
done

# Export so the value reaches each worker spawned via `screen`.
export BLOCK_SIZE="${BLOCK_SIZE:-200000}"

# Dataset location, exported so each screen-spawned run_chrom_eds.sh inherits it.
export OUT_BASE="${OUT_BASE:-$HOME/raid_storage/Data/1000HGp3}"
export VCF_DIR="${VCF_DIR:-$OUT_BASE/vcf}"
export REF_DIR="${REF_DIR:-$OUT_BASE/ref}"
export OUT_DIR="${OUT_DIR:-$OUT_BASE/eds}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MASTER_SCREEN="run_eds"
SCREEN_PREFIX="vcf2eds"

echo "=== VCF → EDS batch run — $(date '+%Y-%m-%d %H:%M:%S') ==="
echo "    block-size: ${BLOCK_SIZE} bases (per-process memory control)"

spawned=()
for vcf_file in "$VCF_DIR"/*.vcf; do
    [[ -f "$vcf_file" ]] || { echo "No VCF files found in $VCF_DIR"; exit 1; }
    chrom=$(basename "$vcf_file" .vcf)
    ref_file="$REF_DIR/${chrom}.fasta"

    if [[ ! -f "$ref_file" ]]; then
        echo "[WARN] No reference for chr${chrom}, skipping"
        continue
    fi

    screen_name="${SCREEN_PREFIX}_${chrom}"
    screen -dmS "$screen_name" bash "$SCRIPT_DIR/run_chrom_eds.sh" "$chrom" $force_flag
    echo "[SPAWN] screen '$screen_name' for chr${chrom}"
    spawned+=("$screen_name")
done

if [[ ${#spawned[@]} -eq 0 ]]; then
    echo "Nothing to run."
    exit 0
fi

echo "--- Spawned ${#spawned[@]} screens, waiting for completion ---"

while true; do
    still_running=()
    for sname in "${spawned[@]}"; do
        if screen -ls | grep -q "$sname"; then
            still_running+=("$sname")
        fi
    done

    if [[ ${#still_running[@]} -eq 0 ]]; then
        break
    fi

    echo "[$(date '+%H:%M:%S')] Still running: ${still_running[*]}"
    sleep 120
done

echo "=== STAGE 1 ALL DONE — $(date '+%Y-%m-%d %H:%M:%S') ==="
echo "EDS files in: $OUT_DIR/"
echo "Next step: run run_leds.sh to create l-EDS"
screen -S "$MASTER_SCREEN" -X quit
