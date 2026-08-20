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
# Env:
#   MODES     which merges to run       (default "linear cartesian")
#   MEM_CAP   per-run memory ceiling    (default 20G; set MEM_CAP= to disable)
#
# The two merges answer different questions on the same input and both are worth
# having, so both run by default:
#
#   linear     keeps only the haplotypes the samples actually carry — sources are
#              intersected at every merge, so the l-EDS is exactly this dataset
#              realised at context length l. Writes .leds + .seds into leds_l<l>/.
#   cartesian  keeps every combination of adjacent alternatives, so it also spells
#              out recombinants nobody was sequenced with. Needs no sources and
#              writes .leds only, into leds_l<l>_cart/.
#
# Comparing the two is the point: cartesian/linear string counts measure how much
# of the l-EDS is combinations the dataset never contained, and their runtimes and
# peak RSS measure what the source bookkeeping that avoids them costs.
#
# Every eds2leds run is wrapped in a systemd user scope with a hard memory ceiling,
# so a combinatorial blow-up is killed inside its own cgroup instead of taking the
# machine down. Cartesian runs blow up first and by design — a kill at the cap is a
# result, not an error, and is recorded as such in leds_runs.tsv.
set -u

BASE="${1:?usage: run_subset_dataset.sh <dataset_base> [l_values...]}"
shift || true
L_VALUES=("$@")
MEM_CAP="${MEM_CAP-20G}"
MODES="${MODES:-linear cartesian}"

VCF_DIR="$BASE/vcf"
REF_DIR="$BASE/ref"
EDS_DIR="$BASE/eds"
mkdir -p "$EDS_DIR"

# One row per attempted eds2leds run, so the comparison can distinguish "cartesian
# was not run" from "cartesian was killed at the cap" — an outcome that otherwise
# leaves nothing behind but a missing file.
RUNS_TSV="$BASE/leds_runs.tsv"
[[ -s "$RUNS_TSV" ]] || printf 'mode\tl\tchrom\tstatus\texit_code\tseconds\n' > "$RUNS_TSV"
record_run() {   # mode l chrom status exit_code seconds
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >> "$RUNS_TSV"
}

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
    for mode in $MODES; do
        case "$mode" in
            linear)    out_dir="$BASE/leds_l${l}" ;;
            cartesian) out_dir="$BASE/leds_l${l}_cart" ;;
            *) echo "unknown MODES entry '$mode' (want linear and/or cartesian)" >&2; continue ;;
        esac
        mkdir -p "$out_dir"
        echo "=== stage 2: EDS -> l-EDS, l=$l, $mode ==="
        for e in "$EDS_DIR"/*.eds; do
            c=$(basename "$e" .eds)
            # Only a linear merge produces sources, so only it has a .seds to check.
            if [[ -s "$out_dir/${c}.leds" ]] &&
               { [[ "$mode" == cartesian ]] || [[ -s "$out_dir/${c}.seds" ]]; }; then
                echo "[$c l=$l $mode] SKIP (exists)"; continue
            fi
            rm -f "$out_dir/${c}.leds" "$out_dir/${c}.seds"   # clear partial leftovers

            args=(-i "$e" -l "$l" -o "$out_dir/${c}.leds")
            # Sources present => linear merge; absent => cartesian. That flag is the
            # only difference between the two arms; the merge groups are chosen the
            # same way, so the outputs differ only by the combinations linear prunes.
            [[ "$mode" == linear ]] && args+=(-s "$EDS_DIR/${c}.seds")

            echo "[$c l=$l $mode] eds2leds..."
            t0=$(date +%s)
            if capped eds2leds "${args[@]}" > "$out_dir/${c}_l${l}.out" 2>&1; then
                record_run "$mode" "$l" "$c" ok 0 "$(( $(date +%s) - t0 ))"
                grep -h "Peak Memory" "$out_dir/${c}_l${l}.out" | sed "s/^/[$c l=$l $mode] /"
            else
                rc=$?
                # Exceeding MEM_CAP kills the process inside its own scope — expected
                # for dense data at high path counts, and the normal cartesian outcome
                # once l passes the point where combinations explode. Record which it
                # was (137 = SIGKILL from the cgroup, 3 = eds2leds' own pre-flight
                # refusal) and keep going.
                case $rc in
                    137|9) status=mem_cap ;;
                    3)     status=refused ;;
                    *)     status=failed  ;;
                esac
                record_run "$mode" "$l" "$c" "$status" "$rc" "$(( $(date +%s) - t0 ))"
                echo "[$c l=$l $mode] $status (exit $rc) — see $out_dir/${c}_l${l}.out" >&2
                rm -f "$out_dir/${c}.leds" "$out_dir/${c}.seds"
            fi
        done
    done
done

echo "=== all done ($RUNS_TSV has $(( $(wc -l < "$RUNS_TSV") - 1 )) runs) ==="
