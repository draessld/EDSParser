#!/bin/bash
# Create a copy of a dataset with long alleles removed.
#
# Why: the EDS is dominated by a handful of long indels, not by the SNVs. On the
# 100-isolate Mtb panel the EDS held 48.1M characters for a 4.4 Mb genome — only
# 4.1M of them the common backbone. group_overlapping_variants() merges every
# variant whose span overlaps, and merge_variant_group() then emits one full-span
# haplotype per (variant, allele), so a long deletion swallows everything inside
# it: measured on the equivalent 32-isolate panel, one 19 kb group containing 325
# variants emitted 334 haplotypes and cost 6.4 MB by itself, and the top 10 groups
# were 89% of all degenerate text.
#
# Dropping alleles over 50 bp removed 3.9% of sites and took the EDS from 18.0 MB
# to 4.3 MB — i.e. down to roughly reference size, which is what an SNV pangenome
# should look like. Keep both versions: "with structural variation" and "SNV only"
# are a legitimate contrast, and most Mtb work indexes the latter.
#
# Usage:
#   make_allele_subset.sh <src_base> <dest_base> [max_allele_bp]
#
#   src_base        dataset root containing vcf/ and ref/
#   dest_base       new dataset root
#   max_allele_bp   longest REF or ALT allele to keep (default 50)
#
# The reference is shared by symlink; only the VCFs are rewritten.
set -u

SRC="${1:?usage: make_allele_subset.sh <src_base> <dest_base> [max_allele_bp]}"
DEST="${2:?missing dest_base}"
MAXBP="${3:-50}"

command -v bcftools >/dev/null || { echo "bcftools not on PATH" >&2; exit 1; }
[[ -d "$SRC/vcf" ]] || { echo "no $SRC/vcf" >&2; exit 1; }

mkdir -p "$DEST/vcf" || exit 1

# Share the reference rather than copying it: same bytes, and vcf2eds only reads it.
if [[ -d "$SRC/ref" && ! -e "$DEST/ref" ]]; then
    ln -sfn "$(cd "$SRC/ref" && pwd)" "$DEST/ref"
fi

shopt -s nullglob
vcfs=("$SRC"/vcf/*.vcf "$SRC"/vcf/*.vcf.gz)
shopt -u nullglob
(( ${#vcfs[@]} )) || { echo "No VCFs in $SRC/vcf" >&2; exit 1; }

echo "=== allele subset: keep REF/ALT <= ${MAXBP} bp — $SRC -> $DEST ==="
total_in=0 total_out=0
for v in "${vcfs[@]}"; do
    base=$(basename "$v"); base="${base%.gz}"; base="${base%.vcf}"
    out="$DEST/vcf/${base}.vcf"
    if [[ -s "$out" ]]; then
        echo "[$base] SKIP (exists)"
        continue
    fi
    # max(strlen(ALT)) covers multi-allelic sites; -e drops the record if either
    # side exceeds the limit, so a long deletion goes together with its ALT.
    bcftools view -e "strlen(REF)>${MAXBP} || max(strlen(ALT))>${MAXBP}" \
        "$v" -Ov -o "$out.part" || { echo "[$base] FAILED" >&2; rm -f "$out.part"; continue; }
    mv "$out.part" "$out"

    n_in=$(bcftools view -H "$v" 2>/dev/null | wc -l)
    n_out=$(bcftools view -H "$out" | wc -l)
    total_in=$(( total_in + n_in )); total_out=$(( total_out + n_out ))
    printf '[%s] %d -> %d sites (%d dropped, %.1f%%)\n' \
        "$base" "$n_in" "$n_out" "$(( n_in - n_out ))" \
        "$(awk -v a="$n_in" -v b="$n_out" 'BEGIN{print a? 100*(a-b)/a : 0}')"
done

printf '=== done: %d -> %d sites across %d file(s) ===\n' \
    "$total_in" "$total_out" "${#vcfs[@]}"
echo "Next: run_subset_dataset.sh $DEST 10 20 50 100"
