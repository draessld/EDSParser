# Performance Characteristics

Memory profiles, throughput, and tuning advice for all EDSParser operations.

**Reading this document:** every table is marked either **[measured]** — taken from a real
run whose machine is named in [Hardware](#hardware) — or **[formula]** — derived from the
implementation and not yet validated end to end. Do not quote a `[formula]` number as a
result. Numbers last revised **2026-08-06**.

---

## Memory Architecture Overview

| Tool / Operation | Memory model | Peak RAM |
|:---|:---|:---|
| `msa2eds` | Reference + bit vectors in RAM; streaming output | O(alignment length) `[formula]` |
| `vcf2eds` | One genomic block at a time | 134 MB – 2.6 GB on 1000G `[measured]` |
| `eds2leds`, whole file | Metadata for the whole input + one batch of merge groups | **~74 bytes per input symbol** `[measured]` |
| `eds2leds`, `--block-size` | One block's metadata + one batch | 80 MB cartesian, independent of file size `[measured]` |
| `edsparser-stats` | Metadata only | 5 – 50 MB `[measured]` |
| `edsparser-genpatterns` | Metadata only | ~5 MB `[measured]` |
| `genrandomeds` | O(1) streaming | ~4 MB constant `[measured]` |
| `Sources` (streamed) | Byte-offset index + LRU cache | 8 bytes per string + cache `[measured]` |

**The single most important correction to earlier versions of this document:** without
`--block-size`, `eds2leds` peak RSS is **linear in the number of input symbols**, not
constant. Earlier text claimed "~200–600 MB for a 100 GB EDS", which was projected, never
measured, and wrong. Constant memory requires block mode, and even then only in cartesian
mode — see [EDS → l-EDS](#eds--l-eds-transformation).

---

## MSA Transformation

### Memory formula `[formula]`

```
Peak RAM ≈ ref_seq_size
          + 2 × bit_vectors            (≈ ref_seq_size / 4 total)
          + file_positions             (num_sequences × 8 bytes)
          + per-symbol variant buffer  (small, freed after each symbol)
```

**Example:** 1 000 sequences × 100 MB alignment ≈ 100 + 25 MB + 8 KB ≈ **125 MB**.

Independent of sequence count: adding sequences costs 8 bytes each plus per-column
comparison work; sequences are never all held in RAM.

### Scalability `[formula]`

| Scenario | Peak memory | Notes |
|----------|-------------|-------|
| 100 sequences × 1 MB alignment | ~1.3 MB | Trivial |
| 1 000 sequences × 100 MB alignment | ~125 MB | Comfortable |
| 10 000 sequences × 1 GB alignment | ~1.25 GB | Feasible |
| 100 000 sequences × 100 MB alignment | ~125 MB | Reference size dominates |

Throughput is dominated by sequential reads and is quoted elsewhere as 50–200 MB/s; that
figure is **not measured** — no `msa2eds` benchmark exists yet (TODO 3e).

---

## VCF Transformation

### Real run: 1000 Genomes phase 3 `[measured]`

24 chromosomes, 2 504 samples, hs37d5 reference, default 10 Mbp blocks:

| | |
|---|---|
| Variants read / processed | 84 801 880 / 84 785 032 (99.98%) |
| Skipped | ~16.8 K unsupported mobile-element and MT insertions |
| Wall clock | 37.0 h |
| Peak RSS | 134 MB (chrY) – 2 596 MB (chr4) |
| Throughput | 6.5 MB/s of input VCF |
| Sizes | 808 GB VCF → 3.4 GB EDS + 33.1 GB SEDS |

Note the sources dominate the output roughly 10:1. Compressing them is the open question
in TODO 3d; `--source-format edz-compressed` addresses it for `eds2leds` output today.

### Memory formula (per block) `[formula]`

```
Peak RAM ≈ block_size_bytes                     (reference region in RAM)
          + variants_in_block × ~200 bytes
          + EDS output buffer                   (flushed per block)
```

### Block size trade-offs `[formula]`

Memory only — no wall-clock measurements exist for different block sizes yet (TODO 3e).

| Block size | Peak memory | I/O ops | EDS quality |
|:----------:|:-----------:|:-------:|:-----------:|
| 1 Mbp | Very low (~10 MB) | Many | More block boundaries |
| 10 Mbp (default) | Low (~50 MB) | Moderate | Good |
| 100 Mbp | Moderate (~500 MB) | Few | Best |
| 0 (disabled) | O(total variants) | Minimal | Best |

Rule of thumb: 16 GB RAM → `--block-size 2000000`; 64 GB → default; 256 GB → `100000000`.

### What dominates EDS size

`group_overlapping_variants()` merges variants whose spans overlap, and
`merge_variant_group()` then applies **one variant at a time** to the whole group span, so
a long deletion absorbs every variant inside it and emits one full-span string per allele.
On assembly-derived data this, not the SNVs, sets the file size `[measured]`:

| M. tuberculosis panel | EDS | ctx_avg |
|---|---:|---:|
| 100 isolates, unfiltered | 46.0 MB | 241 bp |
| 100 isolates, alleles ≤ 50 bp | 4.3 MB | 229 bp |
| 500 isolates, unfiltered | 689.5 MB | 73 bp |
| 500 isolates, alleles ≤ 50 bp | 4.6 MB | 69 bp |
| 1 141 isolates, unfiltered | 2 990 MB | 40 bp |
| 1 141 isolates, alleles ≤ 50 bp | 5.0 MB | 39 bp |

The genome is 4.41 Mb. Unfiltered, 11.4× the isolates costs **65×** the EDS; filtered, the
EDS is flat at ~5 MB. On the 100-isolate panel only 4.1 M of 48.1 M characters are the common
backbone — 91% of the text sits in degenerate symbols, and the top 10 variant groups hold 89%
of it. Filtering alleles longer than 50 bp drops ~4% of sites.

See TODO 1a; `experiments/scripts/make_allele_subset.sh` applies the filter, or directly:

```bash
bcftools view -e 'strlen(REF)>50 || max(strlen(ALT))>50' in.vcf -Ov -o out.vcf
```

---

## EDS → l-EDS Transformation

### Memory: what actually scales `[measured]`

Peak RSS is linear in input symbols. Index slimming (2026-07-30) took it from ~139 to
**~74 bytes per symbol** — `base_positions` as `uint64_t` rather than 16-byte `std::streampos`,
lazy `cum_*` prefix sums, and exact `reserve`/`shrink_to_fit`:

| Input | Before | After |
|---|---:|---:|
| 1.98 M symbols, `-l 10` linear | 322 MB | 162 MB |
| 31.7 M symbols, `-l 10` linear | 4 404 MB | 2 380 MB |

`--block-size` gives the actual ceiling. Barrier cuts (a common run totalling ≥ l can never
be crossed by a merge) let each block merge in isolation, byte-identical to a whole-file
run. On the 949 MB / 31.7 M-symbol input at `-l 10`:

| Mode | Peak RSS | Wall clock |
|---|---:|---:|
| Whole file | 2 380 MB | 54 s |
| `--block-size 200M` | 1 071 MB | 140 s |
| `--block-size 50M` | 644 MB | 145 s |
| `--block-size 10M`, linear | 540 MB | 145 s |
| `--block-size 10M`, cartesian | **80 MB** | 145 s |

Cartesian block mode is a genuine ceiling — independent of file size. Linear floors at
540 MB because block mode still loads one whole-file `Sources` index to slice each block:
8 bytes per string, i.e. 509 MB for that input's 63.7 M strings. Blocks below ~50 MB
therefore stop helping in linear mode (TODO 2a).

Remaining O(n) structures in whole-file mode: input metadata (~23 B/symbol), output
metadata built inline (~19 B/symbol), the sources byte-offset index (8 B/string), and the
groups vector (~16 B/group).

### Real run: 1000 Genomes, linear `[measured]`

Per chromosome, 2 504 samples. Ranges span chrY (trivial, haploid) to the largest autosomes:

| l | chromosomes done | Runtime | Peak RSS | Output/input |
|---:|---:|---|---|---|
| 5 | 24 | 1 – 948 s | 86 MB – 29.3 GB | 0.99 – 17.2× |
| 10 | 17 | 1 – 1 803 s | 86 MB – 51.7 GB | 1.00 – 71.8× |
| 20 | 4 | 1 – 1 561 s | 85 MB – 41.2 GB | 1.02 – 212.5× |

Cost is driven by **heterozygosity**, not genome size: `vcf2eds` paths are sample-level, so
a het sample sits in both the reference and the alt string at a site and a chain of k
adjacent degenerate sites can survive up to 2^k combinations (TODO 1b). chrY, being haploid,
runs in 1.35 s at 86 MB with an average context of 996 bp.

### Real run: M. tuberculosis, linear `[measured]`

Haploid, one isolate per path — the same code with the heterozygosity pathology removed.
Three panels, alleles filtered to ≤ 50 bp, every run converging in **1 iteration**:

| Panel | ctx_avg | l=10 | l=20 | l=50 | l=100 |
|---|---:|---|---|---|---|
| 100 isolates | 229 bp | 0.16 s / 8 MB | 0.17 s / 8 MB | 0.18 s / 9 MB | 0.20 s / 9 MB |
| 500 isolates | 69 bp | 0.53 s / 14 MB | 0.56 s / 14 MB | 0.70 s / 15 MB | 3.31 s / 111 MB |
| 1 141 isolates | 39 bp | 1.07 s / 20 MB | 1.16 s / 21 MB | 5.34 s / 161 MB | killed at 20 GB |

### Choosing l: the ceiling scales with the panel `[measured]`

Output expansion is a clean function of **l / ctx_avg**, not of l on its own. The same three
panels, showing that ratio and the resulting `.leds`-over-`.eds` size:

| Panel | ctx_avg | l=10 | l=20 | l=50 | l=100 |
|---|---:|---|---|---|---|
| 100 | 229 bp | 0.04 → 1.0× | 0.09 → 1.0× | 0.22 → 1.1× | 0.44 → 1.5× |
| 500 | 69 bp | 0.14 → 1.0× | 0.29 → 1.2× | 0.72 → 2.5× | 1.44 → 13.7× |
| 1 141 | 39 bp | 0.26 → 1.1× | 0.51 → 1.7× | 1.28 → 18.3× | 2.56 → **OOM** |

Below about 0.3 the l-EDS is nearly free; around 0.5 it costs ~1.7×; past 1.0 it goes
non-linear. The 1 141-isolate run at l=100 was killed at a 20 GB cap with 213 851 of its
225 023 positions (95%) swept into merge groups — at l = 2.6 × ctx_avg almost no position is
a valid context any more, so the whole file collapses into a few enormous symbols. That is
correct behaviour for an impossible request, not a defect.

**Rule of thumb: keep l ≲ ctx_avg/2.** Read `ctx_avg` off `edsparser-stats` before choosing.
Because contexts shorten as a panel grows (229 → 69 → 39 bp here), the usable l *shrinks*
with panel size: l=50 is comfortable at 100 isolates and impossible at 1 141.

### Panel scaling and why filtering long alleles is mandatory `[measured]`

Variable sites saturate — 11.4× the isolates gives 6.0× the sites (210 → 107 sites per
isolate) — so a well-formed EDS should stay near reference size as the panel grows. Filtered,
it does. Unfiltered, it does the opposite:

| Panel | raw sequence | EDS filtered | EDS unfiltered | N characters unfiltered |
|---|---:|---:|---:|---:|
| 100 | 0.41 GB | 4.3 MB | 46.0 MB | 48.1 M |
| 500 | 2.05 GB | 4.6 MB | 689.5 MB | 722.7 M |
| 1 141 | 4.69 GB | **5.0 MB** | **2 990 MB** | **3.13 G** |

The filtered EDS is 960× smaller than the raw panel at 1 141 isolates and barely grows;
the unfiltered one reaches 3.13 billion characters for a 4.41 Mb genome (710×) and grows 65×
across the panel range. The decisive comparison is at the output: the unfiltered 1 141-isolate
l-EDS at l=100 is **8.2 GB against a 4.69 GB raw panel** — the index is larger than simply
storing every genome. The filtered equivalent at l=50 is 91.6 MB, 51× smaller than the panel.

Cause and workaround are described under [VCF Transformation](#what-dominates-eds-size).

### Throughput `[measured]`

Synthetic data via `genrandomeds --min-context 0` (which actually triggers merging),
after the EDS raw-copy pass-through of 2026-07-03:

| Variability | Cartesian | Linear |
|---|---:|---:|
| 1%, l=5 | ~25 MB/s | ~21 MB/s |
| 5%, l=5 | ~8.7 MB/s | ~5.7 MB/s |

Linear writes an extra SEDS temp file per iteration; cartesian writes no SEDS data at all,
which is the whole of the remaining gap. The raw-copy change itself measured **1.9–2.2×
faster for cartesian and 1.16–1.63× for linear** on 20 MB inputs, output byte-identical.

Key I/O optimisations, in order of impact:

1. **EDS raw-copy pass-through** (2026-07-03) — unmodified full-bracket symbols are
   byte-copied via `EDS::copy_symbol_range_to_stream()` instead of parsed and re-serialised.
2. **Sequential seek elimination + bulk read** — `read_symbol_from_stream()` skips `seekg()`
   when already positioned, and reads each symbol's byte span in one `read()` rather than
   per-byte `get()`/`peek()`.
3. **SEDS batching** — consecutive unmodified symbols bulk-copied via `copy_range_to_stream()`.
4. **No `ostringstream` accumulation** — output written straight to the file stream.

### Iterations to convergence `[measured]`

Chain-merging selection (2026-06-28) resolves a whole contiguous chain in one pass instead
of one adjacent pair per pass, which collapsed iteration counts. Earlier versions of this
document claimed 2–4 for MSA and 3–6 for VCF data; both are obsolete:

| Data | Iterations |
|------|---:|
| 1000G VCF-derived, l = 5 … 100 | **2** (every chromosome, every l) |
| M. tuberculosis VCF-derived, l = 10 … 200 | **1** |
| Synthetic, `--min-context 0`, 1–5% variability | **1** |
| Adversarial (alternating single-base) | O(l) `[formula]` |

### Correctness bound worth knowing

In linear mode a merged symbol can never contain more strings than there are paths: each
path takes exactly one alternative, so surviving combinations carry disjoint path sets.
Violating that bound was the symptom of the complement-source bug fixed in 20d8ff1 — at
50 paths one symbol reached 6 444 274 strings. If you see strings-per-symbol exceed
`num_paths`, the merge is wrong, not merely slow.

### Admission control

`--estimate-memory` prints a prediction as `KEY=BYTES` lines and exits without doing work;
`--max-memory <size>` refuses the transform with **exit code 3** if the prediction exceeds
the budget. Both are **calibrated only for haploid/low-heterozygosity data.** On 1000G chr7
at l=5 the prediction was 0.64 GiB against an actual peak of 28.3 GiB — 45× low — because
the estimator caps each group at `num_paths`, which is exactly the bound heterozygous
sample-level sources break. Do not use it for admission control on het VCF input (TODO 1d).

---

## Sources

### Format sizes `[measured]`

**The deciding factor is the allele-frequency spectrum, not the path count.** A bitset costs
⌈paths/8⌉ bytes per entry no matter how many paths actually carry the allele, while the text
encoding costs only as much as the members it lists. So bitsets win when most entries are
dense (common variants, many carriers) and lose badly when entries are sparse.

*VCF-derived population data, common variants:*

| Paths | SEDS (text) | `seds.gz` | EDZ sparse | EDZ compressed |
|---:|---:|---:|---:|---:|
| 500 | 9.4 MiB | 2.9 MiB | 4.5 MiB | **2.0 MiB** |
| 2 504 | 16.3 MiB | — | 7.8 MiB | **2.7 MiB** |

*M. tuberculosis panels, rare variants in a clonal population:*

| Paths | SEDS (text) | `seds.gz` | EDZ sparse |
|---:|---:|---:|---:|
| 100 | 464 KB | **57 KB** | 529 KB |
| 500 | 2.98 MB | **345 KB** | 8.36 MB |
| 1 141 | 7.83 MB | **1.02 MB** | 34.07 MB |

The two tables point in opposite directions. On the clonal panels EDZ sparse gets steadily
*worse* with path count — 4.4× larger than the text it replaces at 1 141 paths — because a
variant carried by three isolates costs 143 bytes as a bitset and about 12 as `{5,88,900}`.
Plain gzip of the text SEDS beats everything by roughly 8× at every panel size.

**Guidance:** if most of your variants are rare (a clonal or highly structured population),
keep text SEDS and compress the file. Reach for EDZ only when entries are genuinely dense.
`edz-compressed` has not been measured on rare-variant data — `collect_results.sh` currently
only tries `--to edz --sparse` (TODO 2e). Set the format with `eds2leds --source-format`.

### Index building `[measured]`

Bulk 64 KB chunk scanning replaced per-character `get()` + `tellg()`: a 2 MB SEDS builds
its index in ~32 `read()` calls instead of ~2 M function calls.

### LRU cache hit rate `[unverified]`

The frequently quoted "~98% hit rate at the default 10 000 entries" is **asserted, never
measured** (TODO 3e). Merges do access sources in near-linear order, so locality is real,
but treat the number as a plausible guess until instrumented.

### Streaming vs in-memory `[measured]`

| SEDS file | Stream constructor | `EDS::load` (indexed) | Reduction |
|:---------:|:---:|:---:|:---:|
| 13 GB | ~10.6 GB | ~25 MB | **420×** |
| 100 MB | ~80 MB | ~3 MB | ~27× |

---

## genrandomeds

Two independent PRNGs streaming straight to disk; **~4 MB RSS constant** whatever the
output size (1 MB, 100 MB and 10 GB outputs all peak at 4 MB) `[measured]`.

---

## Hardware

Results in this document come from two machines. Anything without a machine named here is
a `[formula]` estimate.

**Server — `DGX-A100-KTI`.** All 1000 Genomes and M. tuberculosis runs. Datasets on
`raid_storage`. *Exact CPU, RAM and storage specification still to be recorded (TODO 3a).*

**Laptop — ThinkPad T14s Gen 3.** Synthetic benchmarks, block-mode measurements and the
complement-fix figures. AMD Ryzen 7 PRO 6850U (8 cores / 16 threads), 30 GB RAM, Samsung
NVMe SSD, Linux 6.8.

Reproduce the synthetic figures with `bash tests/bench/bench.sh --size standard`
(N=3 median). `bench_plot.py` embeds machine info in plot footers automatically.

---

## Practical Tuning Guide

### "My eds2leds is using too much memory"

1. **Use `--block-size`.** This is the one setting that changes the memory *class* rather
   than the constant. Start at `10M`. Cartesian mode becomes independent of file size;
   linear mode floors at the sources index (8 B/string).
2. **Check whether the input is heterozygous diploid.** If a chain of adjacent degenerate
   sites is blowing up, no memory setting will save you — the output itself is exponential.
   Haploidise the VCF (one allele per sample) if the biology allows, e.g. for clonal or
   inbred panels.
3. **Run `--estimate-memory` first**, remembering it under-predicts badly on het data.
4. **Reduce `--threads`** — each thread holds one symbol.
5. The batch size (1 000 groups) is a compile-time constant; lowering it needs a code change.

### "My vcf2eds is using too much memory"

Reduce `--block-size`, starting at `1000000`.

### "My EDS is enormous relative to the genome"

Look for long indels. One long deletion absorbs every variant inside it and emits a
full-span string per allele. Filter with `make_allele_subset.sh`, or drop the sites with:

```bash
bcftools view -e 'strlen(REF)>50 || max(strlen(ALT))>50' in.vcf -Ov -o out.vcf
```

### "My eds2leds is slow"

1. Read the complexity warning it prints before starting.
2. `--threads N` — most of the benefit arrives by 4–8 threads on genomic data.
3. Prefer LINEAR (`-s`): pruning invalid paths produces fewer, shorter results.
4. Put `TMPDIR` on an SSD — iterations chain through temp files.

### "I need a very large SEDS LRU cache"

```cpp
auto eds = EDS::load("data.eds", "data.seds");
eds.get_sources_object()->set_cache_capacity(1'000'000);  // ~40 MB
```

`EDS::set_source_cache_capacity()` was removed; use the `Sources` object directly.

### "How do I profile memory over time?"

```cpp
#include <edsparser/memory_monitor.hpp>

MemoryMonitor mon(0.5);  // sample every 0.5 s
mon.start();
// ... your operation ...
mon.stop();

std::cout << "Peak: " << mon.get_peak_memory_mb() << " MB\n";
if (mon.detect_memory_leak(1.0))  // threshold 1 MB/s
    std::cerr << "Memory leak detected!\n";
```
