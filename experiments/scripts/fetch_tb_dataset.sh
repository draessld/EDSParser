#!/bin/bash
# Build an M. tuberculosis pangenome dataset (VCF + reference) for EDSParser.
#
# Why Mtb: it is haploid and clonal, so one isolate is one path — the sample-level
# path model that makes diploid panels explode (TODO 9c) simply does not apply —
# and variable sites saturate as isolates accumulate, so contexts stay long.
# Measured at 100 isolates: 21,023 sites, ctx_avg 241 bp, and eds2leds at
# l=10..100 converges in 1 iteration in ~0.6 s under 69 MB. Contrast yeast at 50
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
# Layout — a shared pool plus one directory per panel size, so panels coexist and
# a larger panel reuses everything the smaller one already downloaded and called:
#
#   <base>/bin/            static tools
#   <base>/asm/            downloaded assemblies      (pool, shared)
#   <base>/calls/          per-isolate VCFs           (pool, shared)
#   <base>/panel_<n>/vcf/  merged haploid VCF         (one dataset per panel size)
#   <base>/panel_<n>/ref/  reference, named to match
#
# Each panel_<n> is exactly what run_subset_dataset.sh and collect_results.sh expect.
#
# Needs no root and no conda: minimap2 + k8 + paftools.js and the NCBI datasets CLI
# are fetched as static binaries into <base>/bin. bcftools is the one external
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
mkdir -p "$BIN" "$BASE"/{ref,asm,calls} || exit 1
cd "$BASE" || exit 1
export PATH="$BIN:$PATH"

# Pre-panel layout kept per-isolate calls in vcf/ next to the merged VCF. Move them
# into the pool so an existing run is reused rather than recalled from scratch.
if compgen -G "vcf/*.vcf.gz" >/dev/null; then
    echo "=== migrating per-isolate calls from vcf/ to calls/ ==="
    mv vcf/*.vcf.gz vcf/*.vcf.gz.csi calls/ 2>/dev/null
fi

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
echo "    reference contig: $CHROM"

# ── stage 2: accession list ──────────────────────────────────────────────────
if [[ ! -s all_acc.txt ]]; then
    echo "=== listing complete genomes for taxon $TAXON ==="
    datasets summary genome taxon "$TAXON" --assembly-level complete --as-json-lines \
        | dataformat tsv genome --fields accession | tail -n +2 > all_acc.txt || exit 1
fi
avail=$(wc -l < all_acc.txt)
(( N > avail )) && { echo "    only $avail available, using all"; N=$avail; }
head -n "$N" all_acc.txt > "acc_${N}.txt"
echo "=== panel of $N isolates (of $avail available) ==="

# ── stage 3: assemblies ──────────────────────────────────────────────────────
# Downloaded in chunks rather than one request. A single archive for the whole
# 1141-genome panel is ~4.9 GB, and one timeout, dropped connection or full disk
# then loses the entire download — which is how the first 1141 attempt failed,
# while the 400-assembly request for panel 500 went through. Chunking caps peak
# disk at one archive, lets each chunk retry independently, and makes an
# interrupted run resume at chunk granularity.
CHUNK="${CHUNK:-150}"          # ~650 MB per archive
RETRIES="${RETRIES:-3}"

missing=$(while read -r a; do
              find asm -path "*${a}*" -name '*.fna' -print -quit 2>/dev/null | grep -q . || echo "$a"
          done < "acc_${N}.txt")
if [[ -n "$missing" ]]; then
    n_missing=$(wc -l <<<"$missing")
    need_mb=$(( n_missing * 5 ))
    free_mb=$(df -Pm . | awk 'NR==2{print $4}')
    echo "=== downloading $n_missing assemblies (~4.5 MB each, ~${need_mb} MB; ${free_mb} MB free) ==="
    (( free_mb < need_mb + 1024 )) &&
        echo "    WARNING: free space is tight — the pool needs ~${need_mb} MB plus room for EDS output" >&2

    printf '%s\n' "$missing" > acc_missing.txt
    split -l "$CHUNK" -d acc_missing.txt .chunk_
    failed_chunks=0
    for chunk in .chunk_*; do
        got=0
        for attempt in $(seq "$RETRIES"); do
            # datasets reports failures on stdout, so keep both streams.
            if datasets download genome accession --inputfile "$chunk" \
                   --include genome --filename chunk.zip >chunk.err 2>&1 \
               && unzip -qo chunk.zip -d asm 2>>chunk.err; then
                got=1; break
            fi
            echo "    chunk ${chunk#.chunk_} attempt $attempt/$RETRIES failed: $(grep -iE 'error|fail|no such|denied|space' chunk.err | tail -1)" >&2
            rm -f chunk.zip
            sleep $(( attempt * 5 ))
        done
        rm -f chunk.zip chunk.err
        if (( got )); then
            echo "    chunk ${chunk#.chunk_}: $(wc -l < "$chunk") assemblies"
            rm -f "$chunk"
        else
            failed_chunks=$(( failed_chunks + 1 ))
        fi
    done
    rm -f acc_missing.txt
    (( failed_chunks )) &&
        echo "    $failed_chunks chunk(s) still missing — the panel will be built from what arrived;" \
             "re-run to retry them" >&2
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
[[ -s calls/\$acc.vcf.gz ]] && exit 0
fna=\$(find asm -path "*\${acc}*" -name '*.fna' | head -1)
[[ -z "\$fna" ]] && { echo "  [\$acc] no assembly found" >&2; exit 0; }
minimap2 -cx asm5 --cs -t $THREADS ref/H37Rv.fasta "\$fna" 2>/dev/null \
  | sort -k6,6 -k8,8n \
  | k8 "$BIN/paftools.js" call -f ref/H37Rv.fasta -s "\$acc" - 2>/dev/null > calls/\$acc.raw.vcf
if [[ -s calls/\$acc.raw.vcf ]]; then
    bcftools sort -Oz -o calls/\$acc.vcf.gz calls/\$acc.raw.vcf 2>/dev/null \
      && bcftools index -f calls/\$acc.vcf.gz
fi
rm -f calls/\$acc.raw.vcf
EOF
chmod +x "$BASE/.call_one.sh"

todo=$(while read -r a; do [[ -s "calls/$a.vcf.gz" ]] || echo "$a"; done < "acc_${N}.txt")
if [[ -n "$todo" ]]; then
    echo "=== calling variants for $(wc -l <<<"$todo") isolates (-j$JOBS, minimap2 -t$THREADS) ==="
    printf '%s\n' "$todo" | xargs -P "$JOBS" -n1 "$BASE/.call_one.sh"
fi

# ── stage 5: merge + haploidise into panel_<N> ───────────────────────────────
# -0 fills samples with no call at a site as reference, which is what a haploid
# clone means. The awk keeps the first allele of each GT, so one isolate becomes
# exactly one path: without it a "1/1" would place the isolate in two strings and
# the linear merge would lose its pruning.
PANEL="$BASE/panel_${N}"
mkdir -p "$PANEL"/{vcf,ref}
ln -sf "$BASE/ref/H37Rv.fasta" "$PANEL/ref/${CHROM}.fasta"

sed 's|^|calls/|; s|$|.vcf.gz|' "acc_${N}.txt" > want.txt
ls calls/*.vcf.gz 2>/dev/null | sort > have.txt
comm -12 <(sort want.txt) have.txt > "merge_list_${N}.txt"
called=$(wc -l < "merge_list_${N}.txt")
rm -f want.txt have.txt
(( called )) || { echo "no variant calls available to merge" >&2; exit 1; }
# The directory is named for the requested size, but the panel is only as big as
# the isolates that actually made it through download and calling. Say so — the
# path count is what every downstream number depends on.
(( called < N )) &&
    echo "    WARNING: panel_${N} holds $called isolates, not $N — re-run to fill the gap" >&2

echo "=== merging $called isolates → panel_${N}/vcf/${CHROM}.vcf ==="
bcftools merge -0 -l "merge_list_${N}.txt" -Ov 2>/dev/null \
  | awk 'BEGIN{FS=OFS="\t"} /^#/{print;next}
         {for(i=10;i<=NF;i++){split($i,g,/[\/|]/); $i=g[1]} print}' \
  > "$PANEL/vcf/${CHROM}.vcf" || exit 1
sites=$(grep -vc '^#' "$PANEL/vcf/${CHROM}.vcf")
echo "    $sites variant sites across $called isolates"

cat <<EOF

=== done: $PANEL ===
  ref/${CHROM}.fasta      reference (contig $CHROM)
  vcf/${CHROM}.vcf        merged, haploid, $called paths, $sites sites

Next:
  make_allele_subset.sh $PANEL ${PANEL}_snv50 50    # drop long indels (see below)
  run_subset_dataset.sh $PANEL 10 20 50 100
  collect_results.sh    $PANEL tb_p${N}

Long alleles dominate the EDS: at 100 isolates the top 10 variant groups were 89%
of all degenerate text, because group_overlapping_variants absorbs every variant
inside a long deletion and emits one full-span haplotype per allele. Filtering
alleles over 50 bp dropped ~4% of sites and shrank the EDS 4.2x. Build both.

Requires eds2leds at fa0cda0 or later: below 64 paths the merge takes the bitset
fast path, which mis-read complement source sets before that commit.
EOF
