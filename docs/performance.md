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

The genome is 4.41 Mb. Unfiltered, 5× the isolates costs 15× the EDS; filtered, the EDS is
flat. On the 100-isolate panel only 4.1 M of 48.1 M characters are the common backbone —
91% of the text sits in degenerate symbols, and the top 10 variant groups hold 89% of it.
See TODO 1a; `experiments/scripts/make_allele_subset.sh` applies the filter.

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

Haploid, one isolate per path — the same code with the pathology removed:

| Panel | l=10 | l=100 |
|---|---|---|
| 500 isolates, unfiltered | 9.0 s / 744 MB | 25.2 s / 2 092 MB |
| 500 isolates, alleles ≤ 50 bp | 0.5 s / 14 MB | 3.3 s / 111 MB |

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

Choose by path count — the ranking inverts:

| Paths | SEDS (text) | `seds.gz` | EDZ sparse | EDZ compressed |
|---:|---:|---:|---:|---:|
| 100 | 440 KB | **58 KB** | 510 KB | — |
| 500 | 9.4 MiB | 2.9 MiB | 4.5 MiB | **2.0 MiB** |
| 2 504 | 16.3 MiB | — | 7.8 MiB | **2.7 MiB** |

At 100 paths sparse EDZ is *larger* than the text it replaces, and plain gzip beats
everything. EDZ pays off from a few hundred paths up, where bitsets get sparse enough to
compress well — and the ratio improves with path count, while dense EDZ gets worse
(⌈paths/8⌉ bytes per entry). Set with `eds2leds --source-format`.

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
