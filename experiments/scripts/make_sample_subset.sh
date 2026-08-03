#!/bin/bash
# Create a sample-subsetted copy of a per-chromosome VCF dataset.
#
# Why: peak memory of the linear (phasing-aware) l-EDS merge is driven by the number
# of source paths, super-linearly. Measured on yeast chromosome1 (230 kb, 35k variants):
#   1011 samples -> killed at a 20 GB cap, still in the first merge batch
#     50 samples -> 43.7 MB peak, 0.34 s
# Sample count is therefore the most effective knob for making a dataset tractable,
# and sweeping it gives a cost-vs-paths curve worth plotting.
#
# Usage:
#   make_sample_subset.sh <src_vcf_dir> <dest_base> <n_samples> [seed]
#
#   src_vcf_dir   directory of per-chromosome .vcf / .vcf.gz files
#   dest_base     new dataset root; VCFs are written to <dest_base>/vcf/
#   n_samples     how many samples to keep
#   seed          RNG seed for the random draw (default 42; omit randomness with
#                 seed=0, which keeps the first n samples in file order)
#
# Also drops sites that are no longer polymorphic in the subset (-c1) and unobserved
# ALT alleles at multi-allelic sites (-a). Both shrink the EDS as well as the VCF:
# a site where every retained sample is reference contributes no degenerate symbol.
set -u

SRC="${1:?usage: make_sample_subset.sh <src_vcf_dir> <dest_base> <n_samples> [seed]}"
DEST="${2:?missing dest_base}"
N="${3:?missing n_samples}"
SEED="${4:-42}"

command -v bcftools >/dev/null || { echo "bcftools not on PATH" >&2; exit 1; }

shopt -s nullglob
vcfs=("$SRC"/*.vcf "$SRC"/*.vcf.gz)
shopt -u nullglob
(( ${#vcfs[@]} )) || { echo "No .vcf/.vcf.gz files in $SRC" >&2; exit 1; }

mkdir -p "$DEST/vcf"
samples_file="$DEST/samples_${N}.txt"

# Draw the sample list once, from the first VCF, and reuse it for every chromosome —
# they must all describe the same sample set or the paths would not line up.
if [[ ! -s "$samples_file" ]]; then
    if [[ "$SEED" == "0" ]]; then
        bcftools query -l "${vcfs[0]}" | head -n "$N" > "$samples_file"
    else
        # Seeded shuffle: reproducible across runs and machines.
        bcftools query -l "${vcfs[0]}" \
            | shuf --random-source=<(yes "$SEED") -n "$N" \
            | sort > "$samples_file"
    fi
fi
have=$(wc -l < "$samples_file")
echo "=== subset: $have samples (requested $N, seed $SEED) -> $DEST ==="
echo "    sample list: $samples_file"

total_in=0 total_out=0
for v in "${vcfs[@]}"; do
    base=$(basename "$v"); base="${base%.gz}"; base="${base%.vcf}"
    out="$DEST/vcf/${base}.vcf"
    if [[ -s "$out" ]]; then
        echo "[$base] SKIP (exists)"
        continue
    fi
    echo "[$base] subsetting..."
    # -S keep these samples | -a drop ALT alleles nobody carries | -c1 drop sites that
    # are no longer variant | --force-samples tolerates names missing from a file.
    bcftools view -S "$samples_file" --force-samples -a -c1 "$v" -Ov -o "$out.part" || {
        echo "[$base] FAILED" >&2; rm -f "$out.part"; continue; }
    mv "$out.part" "$out"

    n_in=$(bcftools view -H "$v" 2>/dev/null | wc -l)
    n_out=$(bcftools view -H "$out" | wc -l)
    total_in=$(( total_in + n_in )); total_out=$(( total_out + n_out ))
    printf '[%s] %d -> %d variants (%d dropped as non-polymorphic)\n' \
        "$base" "$n_in" "$n_out" "$(( n_in - n_out ))"
done

echo "=== done: $total_in -> $total_out variants across ${#vcfs[@]} files ==="
echo "Reference: point at the originals, e.g."
echo "    ln -s <original_ref_dir> $DEST/ref"
