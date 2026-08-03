#!/bin/bash
# Stage 1 worker: VCF → EDS for a single chromosome.
# Called by run_eds.sh inside its own screen session.
# Usage: run_chrom_eds.sh <chrom> [--force]
#   --force  Overwrite existing output files instead of skipping.
#
# Memory: vcf2eds peak RAM ≈ 2 × (block_size × variant_density × n_samples × 8B).
# With ~2500 samples (1000 Genomes) the default 10M block is ~48 GB PER process —
# fatal when run_eds.sh spawns one screen per chromosome in parallel. We therefore
# pass a small --block-size (default 200000 ≈ 300 MB/process at 1000G density) so
# all chromosomes can run concurrently without OOM. Override with BLOCK_SIZE, e.g.
#   BLOCK_SIZE=1000000 ./run_eds.sh    # ~650 MB/process, fewer/faster blocks

chrom="$1"
force=0
[[ "$2" == "--force" ]] && force=1
BLOCK_SIZE="${BLOCK_SIZE:-200000}"
# Dataset location. OUT_BASE points at the dataset root; the three directories
# below can also be overridden individually for layouts that differ.
OUT_BASE="${OUT_BASE:-$HOME/raid_storage/Data/1000HGp3}"
VCF_DIR="${VCF_DIR:-$OUT_BASE/vcf}"
REF_DIR="${REF_DIR:-$OUT_BASE/ref}"
OUT_DIR="${OUT_DIR:-$OUT_BASE/eds}"

vcf_file="$VCF_DIR/${chrom}.vcf"
ref_file="$REF_DIR/${chrom}.fasta"
out_eds="$OUT_DIR/${chrom}.eds"
out_seds="$OUT_DIR/${chrom}.seds"
log_file="$OUT_DIR/${chrom}.out"

if [[ ! -f "$vcf_file" ]]; then
    echo "[chr${chrom}] VCF not found: $vcf_file" >&2
    exit 1
fi
if [[ ! -f "$ref_file" ]]; then
    echo "[chr${chrom}] Reference not found: $ref_file" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

if [[ -f "$out_eds" && -f "$out_seds" ]]; then
    if [[ $force -eq 0 ]]; then
        echo "[chr${chrom}] SKIP — EDS already exists: $out_eds"
        exit 0
    fi
    echo "[chr${chrom}] FORCE — overwriting existing EDS"
fi

echo "[chr${chrom}] START — $(date '+%Y-%m-%d %H:%M:%S') — block-size ${BLOCK_SIZE}"
vcf2eds \
    -i "$vcf_file" \
    -r "$ref_file" \
    -o "$out_eds" \
    -s "$out_seds" \
    --block-size "$BLOCK_SIZE" \
    &> "$log_file"

exit_code=$?
if [[ $exit_code -ne 0 ]]; then
    echo "[chr${chrom}] FAIL — exit code $exit_code — see $log_file" >&2
    # Remove partial output so the skip-check above doesn't hide the failure
    rm -f "$out_eds" "$out_seds"
    exit $exit_code
fi

echo "[chr${chrom}] DONE — $(date '+%Y-%m-%d %H:%M:%S') — $(du -sh "$out_eds" | cut -f1) EDS"
