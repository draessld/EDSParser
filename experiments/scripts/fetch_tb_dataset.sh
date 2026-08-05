#!/bin/bash
# Build an M. tuberculosis pangenome dataset (VCF + reference) for EDSParser.
#
# Why Mtb: it is haploid and clonal, so one isolate is one path — the sample-level
# path model that makes diploid panels explode (TODO 9c) simply does not apply —
# and variable sites saturate as isolates accumulate, so contexts stay long.
# Measured on the first 32 complete genomes: 14,548 sites, ctx_avg 346 bp, and
# eds2leds at l=10..200 finishes in ~0.2 s under 40 MB. Contrast yeast at 50
# samples, where l=20 is unreachable.
#
# Usage:
#   fetch_tb_dataset.sh [dest_base] [n_isolates]
#
#   dest_base    dataset root (default $HOME/raid_storage/Data/tb)
#   n_isolates   how many complete genomes to use (default 100; 1141 = all)
#
# Env overrides: JOBS (parallel callers, default 8), THREADS (minimap2 -t, default 2).
#
# Needs no root and no conda: minimap2 + k8 + paftools.js and the NCBI datasets CLI
# are fetched as static binaries into <dest_base>/bin. bcftools is the one external
# dependency; if the machine lacks it, build htslib+bcftools into $HOME with
# `make install prefix=$HOME/.local`.
#
# Every stage is resumable — re-running skips work that already produced output.
set -u

BASE="${1:-$HOME/raid_storage/Data/tb}"
N="${2:-100}"
JOBS="${JOBS:-8}"
THREADS="${THREADS:-2}"

MM2_URL="https://github.com/lh3/minimap2/releases/download/v2.31/minimap2-2.31_x64-linux.tar.bz2"
NCBI_URL="https://ftp.ncbi.nlm.nih.gov/pub/datasets/command-line/v2/linux-amd64"
REF_ACC="GCF_000195955.2"        # H37Rv, ASM19595v2, 4,411,532 bp, contig NC_000962.3
TAXON="${TAXON:-1773}"           # Mycobacterium tuberculosis

command -v bcftools >/dev/null || { echo "bcftools not on PATH" >&2; exit 1; }

BIN="$BASE/bin"
mkdir -p "$BIN" "$BASE"/{ref,asm,vcf} || exit 1
cd "$BASE" || exit 1
export PATH="$BIN:$PATH"

# ── stage 0: static tools ────────────────────────────────────────────────────
if [[ ! -x "$BIN/minimap2" ]]; then
    echo "=== fetching minimap2 (+ k8, paftools.js) ==="
    curl -sL -o mm2.tar.bz2 "$MM2_URL" && tar xjf mm2.tar.bz2 || exit 1
    cp minimap2-*_x64-linux/{minimap2,k8,paftools.js} "$BIN/" || exit 1
    rm -rf mm2.tar.bz2 minimap2-*_x64-linux
fi
if [[ ! -x "$BIN/datasets" ]]; then
    echo "=== fetching NCBI datasets CLI ==="
    curl -sL -o "$BIN/datasets"   "$NCBI_URL/datasets"   || exit 1
    curl -sL -o "$BIN/dataformat" "$NCBI_URL/dataformat" || exit 1
fi
chmod +x "$BIN"/* 2>/dev/null

# ── stage 1: reference ───────────────────────────────────────────────────────
if [[ ! -s ref/H37Rv.fasta ]]; then
    echo "=== reference $REF_ACC ==="
    datasets download genome accession "$REF_ACC" --include genome --filename h37rv.zip >/dev/null || exit 1
    unzip -qo h37rv.zip -d h37rv_tmp && find h37rv_tmp -name '*.fna' -exec cp {} ref/H37Rv.fasta \;
    rm -rf h37rv.zip h37rv_tmp
fi
# The VCF CHROM comes from the reference contig, and run_subset_dataset.sh pairs
# vcf/<c>.vcf with ref/<c>.fasta — so name both after the contig.
CHROM=$(awk '/^>/{print substr($1,2); exit}' ref/H37Rv.fasta)
[[ -n "$CHROM" ]] || { echo "could not read contig name from ref/H37Rv.fasta" >&2; exit 1; }
cp -n ref/H37Rv.fasta "ref/${CHROM}.fasta"
echo "    reference contig: $CHROM"

# ── stage 2: accession list ──────────────────────────────────────────────────
if [[ ! -s all_acc.txt ]]; then
    echo "=== listing complete genomes for taxon $TAXON ==="
    datasets summary genome taxon "$TAXON" --assembly-level complete --as-json-lines \
        | dataformat tsv genome --fields accession | tail -n +2 > all_acc.txt || exit 1
fi
head -n "$N" all_acc.txt > acc.txt
have=$(wc -l < acc.txt)
echo "=== using $have isolates (of $(wc -l < all_acc.txt) available) ==="

# ── stage 3: assemblies ──────────────────────────────────────────────────────
missing=$(while read -r a; do
              find asm -path "*${a}*" -name '*.fna' -print -quit 2>/dev/null | grep -q . || echo "$a"
          done < acc.txt)
if [[ -n "$missing" ]]; then
    echo "=== downloading $(wc -l <<<"$missing") assemblies (~4.5 MB each) ==="
    printf '%s\n' "$missing" > acc_missing.txt
    datasets download genome accession --inputfile acc_missing.txt \
        --include genome --filename mtb.zip >/dev/null || exit 1
    unzip -qo mtb.zip -d asm && rm -f mtb.zip acc_missing.txt
fi

# ── stage 4: per-isolate variant calls ───────────────────────────────────────
# minimap2 asm5 + paftools.js call: assembly-vs-reference alignment, so no read
# mapping and no caller to install. paftools writes GT 1/1; stage 5 haploidises.
cat > "$BASE/.call_one.sh" <<EOF
#!/bin/bash
set -u
acc="\$1"
cd "$BASE" || exit 1
export PATH="$BIN:\$PATH"
[[ -s vcf/\$acc.vcf.gz ]] && exit 0
fna=\$(find asm -path "*\${acc}*" -name '*.fna' | head -1)
[[ -z "\$fna" ]] && { echo "  [\$acc] no assembly found" >&2; exit 0; }
minimap2 -cx asm5 --cs -t $THREADS ref/H37Rv.fasta "\$fna" 2>/dev/null \
  | sort -k6,6 -k8,8n \
  | k8 "$BIN/paftools.js" call -f ref/H37Rv.fasta -s "\$acc" - 2>/dev/null > vcf/\$acc.raw.vcf
if [[ -s vcf/\$acc.raw.vcf ]]; then
    bcftools sort -Oz -o vcf/\$acc.vcf.gz vcf/\$acc.raw.vcf 2>/dev/null \
      && bcftools index -f vcf/\$acc.vcf.gz
fi
rm -f vcf/\$acc.raw.vcf
EOF
chmod +x "$BASE/.call_one.sh"

todo=$(while read -r a; do [[ -s "vcf/$a.vcf.gz" ]] || echo "$a"; done < acc.txt)
if [[ -n "$todo" ]]; then
    echo "=== calling variants for $(wc -l <<<"$todo") isolates (-j$JOBS, minimap2 -t$THREADS) ==="
    printf '%s\n' "$todo" | xargs -P "$JOBS" -n1 "$BASE/.call_one.sh"
fi
called=$(ls vcf/*.vcf.gz 2>/dev/null | wc -l)
echo "    $called per-isolate VCFs present"
(( called )) || { echo "no variant calls produced" >&2; exit 1; }

# ── stage 5: merge + haploidise ──────────────────────────────────────────────
# -0 fills samples with no call at a site as reference, which is what a haploid
# clone means. The awk keeps the first allele of each GT, so one isolate becomes
# exactly one path: without it a "1/1" would place the isolate in two strings and
# the linear merge would lose its pruning.
echo "=== merging into vcf/${CHROM}.vcf ==="
sed 's|^|vcf/|; s|$|.vcf.gz|' acc.txt | grep -Ff <(ls vcf/*.vcf.gz) - > merge_list.txt
bcftools merge -0 -l merge_list.txt -Ov 2>/dev/null \
  | awk 'BEGIN{FS=OFS="\t"} /^#/{print;next}
         {for(i=10;i<=NF;i++){split($i,g,/[\/|]/); $i=g[1]} print}' \
  > "vcf/${CHROM}.vcf" || exit 1
sites=$(grep -vc '^#' "vcf/${CHROM}.vcf")
echo "    $sites variant sites across $called isolates"

cat <<EOF

=== done: $BASE ===
  ref/${CHROM}.fasta      reference (contig $CHROM)
  vcf/${CHROM}.vcf        merged, haploid, $called paths, $sites sites

Next:
  vcf2eds -i $BASE/vcf/${CHROM}.vcf -r $BASE/ref/${CHROM}.fasta \\
          -o $BASE/${CHROM}.eds -s $BASE/${CHROM}.seds
  edsparser-stats -i $BASE/${CHROM}.eds -s $BASE/${CHROM}.seds   # check ctx_avg vs your l
  eds2leds -i $BASE/${CHROM}.eds -s $BASE/${CHROM}.seds -l 20 -o $BASE/${CHROM}_l20.leds

or drive the whole l sweep with the existing runner:
  run_subset_dataset.sh $BASE 10 20 50 100

Requires eds2leds at fa0cda0 or later: below 64 paths the merge takes the bitset
fast path, which mis-read complement source sets before that commit.
EOF
