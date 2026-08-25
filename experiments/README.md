# Experiments

Dataset builders, benchmark specs, and collected results for EDSParser.

Datasets themselves are large and live on whatever machine ran them (typically
`~/raid_storage/Data/...`). Only logs, statistics and size measurements are committed, under
`results/<dataset>/` — a few MB instead of many GB.

```
specs/       xbench specs — what is measured, declaratively
run.sh       driver: locates the harness and runs a spec
scripts/     dataset builders (and the older shell sweep runners)

Data lives outside the working tree, under ~/Data:

  ~/Data/<dataset>/                 inputs (covid/, synthetic/, tb/)
  ~/Data/experiments/edsparser/runs      xbench run directories
  ~/Data/experiments/edsparser/results   collected bundles, one per dataset

Only the description of an experiment is versioned; its inputs and outputs are
not. Override the run location with XBENCH_RUNS.
```

## Measuring: xbench

Sweeps are declared as specs and run by [xbench](../../xbench), a separate harness so other
projects can use it (BIO-FMI does). Specs live here, with the project they measure; only the
engine lives there.

```bash
./experiments/run.sh                        # list specs
./experiments/run.sh merge_mode --dry-run   # resolved plan, no work
./experiments/run.sh merge_mode --dataset covid_20A
```

| Spec | Question |
|---|---|
| `merge_mode` | LINEAR vs CARTESIAN across an l sweep — what this dataset's realisation costs and buys |
| `vcf2eds` | VCF→EDS cost and the `--block-size` memory/wall-clock trade-off |
| `source_formats` | SEDS vs every EDZ variant vs gzip |

What the harness supplies, which the shell pipeline did by hand:

- **Refuses to measure a stale binary.** Tools resolve `build/tools` before `PATH`, and
  `eds2leds` must stamp a commit date after the complement fix or the run aborts before doing
  any work.
- **Blow-ups are rows.** A run over its cap is killed and written with `status=oom` and the peak
  it reached, which is what `leds_runs.tsv` was invented for.
- **Sizes come free.** The gather phase records raw, gzip and per-extension sizes for every input
  and artifact, replacing `collect_results.sh`'s size passes.
- **Extractors are re-runnable.** `xbench analyze <run> --reextract` rebuilds the metrics from
  archived stdout/stderr in a second when a regex turns out to be wrong, instead of re-running.

A dataset whose inputs are missing is reported and skipped, so the TB entries can stay declared
on a machine that has not copied the panels yet.

**Building datasets is not xbench's job.** Fetching assemblies, calling variants and subsetting
panels stay in `scripts/`; so does VCF→EDS when you just want the `.eds`/`.seds` that
`merge_mode` consumes (`run_subset_dataset.sh <panel>` with no l values runs stage 1 only).

### Mapping from the shell pipeline

| Was | Now |
|---|---|
| `run_subset_dataset.sh` stage 2, `MODES` | `specs/merge_mode.yaml` |
| `run_subset_dataset.sh` stage 1 | `specs/vcf2eds.yaml` (or the script, to build inputs) |
| `collect_results.sh` sizes + stats | gather phase + `stats` stage → `summary.csv` |
| `leds_runs.tsv` | `status` column (`ok`/`oom`/`timeout`/`error`) |
| `compare_merge_modes.py` | `summary.csv` joined on `tool`, plus the plots |
| `parse_transformation_logs.py` | `extract:` blocks |

The shell runners still work and still matter for the 1000G-scale runs on the server, where the
screen-based scheduling in `run_leds.sh` does things xbench does not. `compare_merge_modes.py`
also still reads the existing `results/` bundles, which predate the specs.

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
`PANELS`, `L_VALUES`, `MODES`, `MAX_ALLELE` (0 disables the filtered twin), `MEM_CAP`,
`RESULTS_ROOT`, `JOBS`, `THREADS`. It refuses to start on an `eds2leds` older than the
complement fix, gating on the commit date the binary reports through `--version`.

### Linear vs cartesian — what the dataset's own realisation buys

Both merges run by default (`MODES="linear cartesian"`). They pick the same merge groups and
differ only in what survives inside one:

| | keeps | needs sources | writes | goes to |
|---|---|---|---|---|
| `linear` | only combinations some sample carries | yes | `.leds` + `.seds` | `leds_l<N>/` |
| `cartesian` | every combination of adjacent alternatives | no | `.leds` | `leds_l<N>_cart/` |

So a linear l-EDS *is* this dataset at context length l, while a cartesian one also spells out
recombinants nobody was sequenced with. The two arms bound the same question from both sides:

- **cartesian / linear strings** — how much of the cartesian language is unobserved. This is the
  price of *not* tracking sources.
- **linear runtime, peak RSS, and `.seds` bytes vs cartesian** — the price of tracking them.
  Linear has to intersect source sets at every fold and carry a `.seds` that is often larger than
  the `.leds` it annotates, so it is not the cheap arm at low l.

A cartesian run killed at `MEM_CAP` is a result, not a failure — it says the combinations this
dataset excludes are what made the transform feasible at all. `run_subset_dataset.sh` records
every attempt in `leds_runs.tsv` (`ok` / `mem_cap` / `refused` / `failed`), which
`collect_results.sh` bundles, so those cells survive into the analysis instead of going missing.

The same distinction shows up on the query side: `edsparser-genpatterns` without sources samples
the cartesian language, and on the 294-sequence SARS-CoV-2 panel only 158/200 of those patterns
occur in the linear l-EDS, against 200/200 for source-aware patterns.

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
<name>/eds2leds/l<N>/*.out            EDS→l-EDS logs (linear)
<name>/eds2leds/l<N>_cart/*.out       the same, cartesian merge
<name>/eds2leds/stats/leds_l<N>.csv   stats over each l-EDS (…_cart.csv for cartesian)
<name>/other/leds_runs.tsv            per-run outcome, incl. runs killed at the cap
<name>/other/                         disk, gzip and EDZ size measurements
<name>/MANIFEST.txt                   provenance, path count and merge modes
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

- `compare_merge_modes.py <results_dir>... [--output merge_modes.csv]` — joins the linear and
  cartesian arms of a bundle into one table: string and byte ratios (how much of the cartesian
  language is unobserved) beside runtime and peak-RSS ratios (what pruning it costs). Reads
  `leds_runs.tsv` too, so a cartesian run killed at the cap is reported as `mem_cap` rather than
  silently dropped. Degrades to a linear-only table on bundles collected before the cartesian arm
  existed.
- `parse_transformation_logs.py <dataset_dir> [--output out.csv]` — turns transformation logs
  into a summary CSV (runtime, peak memory, context length, iterations).
- `notebook_to_pdf.py` — renders an evaluation notebook for inclusion in the write-up.

Each `results/<dataset>/` may also carry an evaluation notebook and a summary CSV built from
these.
