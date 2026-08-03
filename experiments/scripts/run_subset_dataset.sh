#!/bin/bash
# Build EDS (+ optionally l-EDS) for a sample-subsetted dataset created by
# make_sample_subset.sh. Sequential and single-process on purpose: subsetted datasets
# are small enough that the scheduling machinery in run_leds.sh is unnecessary.
#
# Usage:
#   run_subset_dataset.sh <dataset_base> [l_values...]
#
#   dataset_base   root containing vcf/ and ref/ (ref may be a symlink)
#   l_values       optional; e.g. "3 5 10". Omit to build EDS only.
#
# Every eds2leds run is wrapped in a systemd user scope with a hard memory ceiling,
# so a combinatorial blow-up is killed inside its own cgroup instead of taking the
# machine down. Override with MEM_CAP (default 20G); set MEM_CAP= to disable.
set -u

BASE="${1:?usage: run_subset_dataset.sh <dataset_base> [l_values...]}"
shift || true
L_VALUES=("$@")
MEM_CAP="${MEM_CAP-20G}"

VCF_DIR="$BASE/vcf"
REF_DIR="$BASE/ref"
EDS_DIR="$BASE/eds"
mkdir -p "$EDS_DIR"

# Wrap a command in a memory-capped scope when possible; run it directly otherwise.
capped() {
    if [[ -n "$MEM_CAP" ]] && systemd-run --user --scope -p MemoryMax=1G true 2>/dev/null; then
        systemd-run --user --scope -q -p MemoryMax="$MEM_CAP" -p MemorySwapMax=0 "$@"
    else
        "$@"
    fi
}

shopt -s nullglob
vcfs=("$VCF_DIR"/*.vcf)
shopt -u nullglob
(( ${#vcfs[@]} )) || { echo "No VCFs in $VCF_DIR" >&2; exit 1; }

echo "=== stage 1: VCF -> EDS (${#vcfs[@]} chromosomes) ==="
for v in "${vcfs[@]}"; do
    c=$(basename "$v" .vcf)
    ref="$REF_DIR/${c}.fasta"
    [[ -f "$ref" ]] || ref="$REF_DIR/${c}.fa"
    if [[ ! -f "$ref" ]]; then
        echo "[$c] SKIP — no reference at $REF_DIR/${c}.fasta" >&2
        continue
    fi
    if [[ -s "$EDS_DIR/${c}.eds" && -s "$EDS_DIR/${c}.seds" ]]; then
        echo "[$c] SKIP (EDS exists)"; continue
    fi
    echo "[$c] vcf2eds..."
    capped vcf2eds -i "$v" -r "$ref" -o "$EDS_DIR/${c}.eds" -s "$EDS_DIR/${c}.seds" \
        > "$EDS_DIR/${c}.vcf2eds.log" 2>&1 \
        || { echo "[$c] FAILED — see $EDS_DIR/${c}.vcf2eds.log" >&2; rm -f "$EDS_DIR/${c}.eds" "$EDS_DIR/${c}.seds"; }
done

(( ${#L_VALUES[@]} )) || { echo "=== EDS done (no l-values requested) ==="; exit 0; }

for l in "${L_VALUES[@]}"; do
    out_dir="$BASE/leds_l${l}"
    mkdir -p "$out_dir"
    echo "=== stage 2: EDS -> l-EDS, l=$l ==="
    for e in "$EDS_DIR"/*.eds; do
        c=$(basename "$e" .eds)
        if [[ -s "$out_dir/${c}.leds" && -s "$out_dir/${c}.seds" ]]; then
            echo "[$c l=$l] SKIP (exists)"; continue
        fi
        rm -f "$out_dir/${c}.leds" "$out_dir/${c}.seds"   # clear partial leftovers
        echo "[$c l=$l] eds2leds..."
        if capped eds2leds -i "$e" -s "$EDS_DIR/${c}.seds" -l "$l" -o "$out_dir/${c}.leds" \
               > "$out_dir/${c}_l${l}.out" 2>&1; then
            grep -h "Peak Memory" "$out_dir/${c}_l${l}.out" | sed "s/^/[$c l=$l] /"
        else
            # Exceeding MEM_CAP kills the process inside its own scope — expected for
            # dense data at high path counts; record it and keep going.
            echo "[$c l=$l] FAILED or hit the ${MEM_CAP} cap — see $out_dir/${c}_l${l}.out" >&2
            rm -f "$out_dir/${c}.leds" "$out_dir/${c}.seds"
        fi
    done
done

echo "=== all done ==="
