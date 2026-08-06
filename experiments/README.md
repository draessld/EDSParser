# Experiments

Dataset builders, sweep runners, and collected results for EDSParser.

Datasets themselves are large and live on whatever machine ran them (typically
`~/raid_storage/Data/...`). Only logs, statistics and size measurements are committed, under
`results/<dataset>/` — a few MB instead of many GB.

```
scripts/     dataset builders and runners
results/     collected bundles, one directory per dataset
datasets/    small inputs kept in-repo (synthetic, SARS-CoV-2)
```

---

## Which results are valid

The bitset fast path in the l-EDS merge misread complement source sets until **20d8ff1**
(2026-08-04), which made every run at **63 paths or fewer** produce an l-EDS containing
strings no genome carries. The boundary is exact — above 63 paths the code already took the
correct branch.

| Bundle | Paths | Status |
|---|---:|---|
| `hgp1000` | 2 504 | Valid |
| `tb_p100`, `tb_p500`, `tb_p1141` (+ `_snv50`) | 100 – 1 141 | Valid, post-fix |
| `tb_100` | 100 | Superseded by `tb_p100` |
| `yeast1011_50` | 50 | **Invalid** — measures the bug at every l |

Each bundle's `MANIFEST.txt` records the host, the binary and its mtime, the repo commit and
the path count, so validity can be checked without guessing.

---

## Building a dataset

### M. tuberculosis panels — `fetch_tb_dataset.sh`

The recommended dataset: haploid and clonal, so one isolate is one path and the merge stays
cheap; variable sites saturate as isolates accumulate, so contexts stay usable.

```bash
fetch_tb_dataset.sh [dest_base] [n_isolates]      # default ~/raid_storage/Data/tb, 100
```

Needs no root and no conda — minimap2 (with the bundled `k8` and `paftools.js`) and the NCBI
datasets CLI are fetched as static binaries into `<base>/bin`. `bcftools` is the only external
dependency. Assemblies are aligned to H37Rv with `minimap2 -x asm5` and called with
`paftools.js`, so there is no read mapping and no variant caller to install.

Layout — a shared pool plus one directory per panel size, so panels coexist and a larger panel
reuses the smaller one's downloads and alignments:

```
<base>/asm/            assemblies       (pool)
<base>/calls/          per-isolate VCFs (pool)
<base>/panel_<n>/      merged haploid VCF + reference
```

Downloads run in chunks (`CHUNK`, default 150) with retries, so a dropped connection costs one
chunk rather than the whole panel. Everything is resumable; re-run the same command.

### Derived datasets

```bash
make_sample_subset.sh <src_vcf_dir> <dest_base> <n_samples> [seed]   # fewer samples
make_allele_subset.sh <src_base> <dest_base> [max_allele_bp]         # drop long alleles
```

`make_sample_subset.sh` draws a seeded, *nested* subset so a ladder of sizes is a fair
comparison, and drops sites that are no longer polymorphic.

`make_allele_subset.sh` exists because a long deletion absorbs every variant inside it and
`vcf2eds` then emits one full-span haplotype per allele. At 1 141 isolates that takes the EDS
to 2 990 MB for a 4.41 Mb genome, while the filtered twin is 5.0 MB — see TODO 1a. Build both:
"with structural variation" and "SNV only" are a legitimate contrast.

---

## Running sweeps

```bash
run_subset_dataset.sh <dataset_base> [l_values...]   # one dataset, memory-capped
run_tb_experiment.sh  [base]                         # the full matrix
```

`run_subset_dataset.sh` runs VCF→EDS then EDS→l-EDS for each l, wrapping every run in a
systemd scope with a hard `MemoryMax` (`MEM_CAP`, default 20G) so a blow-up is a logged failure
rather than a dead machine.

`run_tb_experiment.sh` drives panel × {unfiltered, filtered} × l, collecting each. Knobs:
`PANELS`, `L_VALUES`, `MAX_ALLELE` (0 disables the filtered twin), `MEM_CAP`, `RESULTS_ROOT`,
`JOBS`, `THREADS`. It refuses to start on an `eds2leds` older than the complement fix.

### Choosing l

Output expansion tracks **l / ctx_avg**, not l. Read `ctx_avg` from `edsparser-stats` on the
EDS first; below ~0.3 the l-EDS is nearly free, past 1.0 it goes non-linear. Because contexts
shorten as a panel grows, the usable l shrinks with panel size. Measured on the TB panels:

| Panel | ctx_avg | l=10 | l=20 | l=50 | l=100 |
|---|---:|---|---|---|---|
| 100 | 229 bp | 1.0× | 1.0× | 1.1× | 1.5× |
| 500 | 69 bp | 1.0× | 1.2× | 2.5× | 13.7× |
| 1 141 | 39 bp | 1.1× | 1.7× | 18.3× | OOM at 20 GB |

---

## Collecting results

```bash
collect_results.sh <data_base> [dataset_name] [out_root]
```

Bundles the artifacts into the layout the notebooks read, and tars it:

```
<name>/vcf2eds/*.vcf2eds.log          VCF→EDS logs
<name>/vcf2eds/stats/                 edsparser-stats over the EDS + all_stats.csv
<name>/eds2leds/l<N>/*.out            EDS→l-EDS logs
<name>/eds2leds/stats/leds_l<N>.csv   stats over each l-EDS
<name>/other/                         disk, gzip and EDZ size measurements
<name>/MANIFEST.txt                   provenance and path count
```

Missing stats CSVs are generated on the fly. Switches for large runs: `GZ=0` (gzip sizing is
slow on multi-GB `.leds`), `STATS_LEDS=0`, `EDZ=0`, `TAR=0`.

Then, on the analysis machine:

```bash
scp '<server>:~/results/*_results.tar.gz' .
for f in *_results.tar.gz; do tar xzf "$f" -C <edsparser>/experiments/results/; done
```

---

## Analysis helpers

- `parse_transformation_logs.py <dataset_dir> [--output out.csv]` — turns transformation logs
  into a summary CSV (runtime, peak memory, context length, iterations).
- `notebook_to_pdf.py` — renders an evaluation notebook for inclusion in the write-up.

Each `results/<dataset>/` may also carry an evaluation notebook and a summary CSV built from
these.
