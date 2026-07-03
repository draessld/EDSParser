# Performance Characteristics

This document covers memory profiles, throughput benchmarks, and
practical tuning advice for all EDSParser operations.

---

## Memory Architecture Overview

| Tool / Operation | Memory Model | Peak RAM |
|:---|:---:|---:|
| `msa2eds` | Reference + bit vectors in RAM; streaming output | O(alignment length) |
| `vcf2eds` | One genomic block in RAM at a time | O(variants per block) |
| `eds2leds` (streaming arch) | Metadata only + one batch of merge pairs | ~200–600 MB for 100 GB EDS |
| `edsparser-stats` | Metadata only | ~1–50 MB |
| `edsparser-genpatterns` | Metadata only | ~1–50 MB |
| `genrandomeds` | O(1) streaming | ~4 MB constant |
| `Sources` (METADATA_ONLY) | Index + LRU cache | ~25 MB for 13 GB SEDS |

---

## MSA Transformation

### Memory Formula

```
Peak RAM ≈ ref_seq_size
          + 2 × bit_vectors          (≈ ref_seq_size / 4 total)
          + file_positions             (num_sequences × 8 bytes)
          + per-symbol variant buffer  (small, freed after each symbol)
```

**Example:** 1 000 sequences × 100 MB alignment
```
  100 MB (reference)
+  25 MB (bit vectors)
+   8 KB (file positions)
+ negligible (per-symbol buffer)
= ~125 MB peak
```

This is independent of the number of sequences — adding more sequences
costs 8 bytes per sequence in file positions and a tiny per-column
comparison overhead; it does **not** accumulate all sequences in RAM.

### Throughput Characteristics

- Dominated by sequential disk reads (reference + per-symbol seeks)
- Typically 50–200 MB/s on modern SSDs
- Memory footprint: **constant relative to output size**

### Scalability

| Scenario | Peak Memory | Notes |
|----------|-------------|-------|
| 100 sequences × 1 MB alignment | ~1.3 MB | Trivial |
| 1 000 sequences × 100 MB alignment | ~125 MB | Comfortable |
| 10 000 sequences × 1 GB alignment | ~1.25 GB | Feasible |
| 100 000 sequences × 100 MB alignment | ~125 MB | Reference size dominates |

---

## VCF Block-Based Processing

### Memory Formula (per block)

```
Peak RAM ≈ block_size_bytes        (reference region in RAM)
          + variants_in_block × ~200 bytes/variant
          + EDS output buffer       (flushed per block)
```

For 1000 Genomes / UK Biobank scale VCF:
```
Default 10 Mbp block:
  10 MB (reference region)
+ ~200 bytes × (typical density ~10 variants/kbp × 10M bases) = ~20 MB
= ~30 MB per block
```

### Block Size Trade-offs

| Block size | Peak memory | I/O ops | EDS quality |
|:----------:|:-----------:|:-------:|:-----------:|
| 1 Mbp | Very low (~10 MB) | Many | More block boundaries |
| 10 Mbp (default) | Low (~50 MB) | Moderate | Good |
| 100 Mbp | Moderate (~500 MB) | Few | Best |
| 0 (disabled) | O(total variants) | Minimal | Best |

**Recommendation for large population VCF:**
- 16 GB RAM: `--block-size 2000000` (2 Mbp)
- 64 GB RAM: default `--block-size 10000000` (10 Mbp)
- 256 GB RAM: `--block-size 100000000` (100 Mbp)

### Structural Variant Memory

Structural variants (`<INV>`, `<CN2>`, etc.) may generate longer alternative
strings, but these are handled within the same block processing loop and
do not change the overall memory model.

---

## EDS → l-EDS Transformation

### Memory Comparison (old vs. new implementation)

| Scenario | Pre-2026-05 Peak | Current Peak | Reduction |
|----------|:---:|:---:|:---:|
| 1 GB EDS, 100 pairs, 4 threads | ~20 GB | ~200 MB | **100×** |
| 10 GB EDS, 500 pairs, 8 threads | ~200 GB | ~400 MB | **500×** |
| 100 GB EDS, 1000 pairs, 16 threads | ~2 TB | ~600 MB | **3000×** |

### Current Memory Formula

```
Peak RAM ≈ metadata                     (~100 MB for 100 GB EDS)
          + batch_size × merge_metadata  (1 000 pairs × ~10 KB = ~10 MB)
          + streaming_buffers            (~100 MB OS buffers)
          + threads × single_symbol      (16 × ~10 KB = negligible)
≈ 210 MB baseline for any file size
```

### Throughput

After I/O optimizations (2026-05-26):

| Method | Throughput | Notes |
|--------|:----------:|-------|
| Cartesian | ~5.4 MB/s | No SEDS temp file per iteration |
| Linear | ~4.3 MB/s | Writes extra SEDS temp file per iteration |

Ratio: **~1.26× cartesian/linear** — the remaining gap is inherent I/O.

Throughput is I/O-bound (disk seeks + sequential reads for on-demand symbol
loading). The key optimisations:

1. **Sequential seek elimination + bulk read**: `read_symbol_from_stream()`
   checks if the stream is already at position before calling `seekg()`
   (~5K seeks per iteration instead of ~400K for forward-order access), and
   reads each symbol's exact byte span in one `stream_.read()` rather than one
   `get()`/`peek()` per byte, splitting the buffer on `,` in memory (2026-07-03).

2. **SEDS batching**: consecutive unmodified symbols are bulk-copied with
   `copy_range_to_stream()` — ~2 727 calls vs ~200K for 10% variability data.

3. **EDS raw-copy pass-through** (2026-07-03): the symmetric optimisation for the
   EDS file — unmodified full-bracket symbols are byte-copied verbatim via
   `EDS::copy_symbol_range_to_stream()` (batched like the SEDS side) instead of
   parsed into a `StringSet` and re-serialised. Measured **cartesian 1.9–2.2×,
   linear 1.16–1.63×** faster on 20 MB inputs; output byte-identical.

4. **No ostringstream accumulation**: output written directly via `<<` to the
   output file stream.

### Iterations to Convergence

| Data | Typical iterations |
|------|----|
| MSA-derived (real genomic, l=5) | 2–4 |
| VCF-derived (real genomic, l=10) | 3–6 |
| Synthetic 10% variability, min-context 0 | 5–10 |
| Adversarial (alternating single-base) | O(l) |

---

## Sources Streaming Performance

### Index Building

| SEDS size | Old (per-char get+tellg) | New (64KB chunk scan) | Ratio |
|:---------:|:---:|:---:|:---:|
| 2 MB | ~2M function calls | ~32 read() calls | **62 500×** |
| 13 GB | Very slow | ~200K read() calls | **~10 000×** |

### LRU Cache Hit Rate

Measured on real merge operations (which access sources in linear order):

| File size | Cache capacity | Hit rate |
|:---------:|:---------:|:-------:|
| Any | 10 000 (default) | ~98% |
| > 100 GB SEDS | 100 000 | ~99% |

### Memory: FULL vs METADATA_ONLY

| SEDS file | FULL mode | METADATA_ONLY | Reduction |
|:---------:|:---------:|:-------------:|:---------:|
| 13 GB | ~10.6 GB | ~25 MB | **420×** |
| 100 MB | ~80 MB | ~3 MB | ~27× |

---

## genrandomeds Memory

`genrandomeds` uses two independent PRNGs streaming directly to disk.
Memory usage is constant at **~4 MB RSS** regardless of output file size.

```
Output 1 MB:   4 MB peak RSS
Output 100 MB: 4 MB peak RSS
Output 10 GB:  4 MB peak RSS
```

---

## Benchmark Baseline (2026-05 Reference System)

Measured with `bench.sh --size standard` (N=3 median, SSD):

| Scenario | Tool | Input | Throughput | Memory |
|----------|------|-------|:----------:|:------:|
| Generation | `genrandomeds` | — | ~80 MB/s | 4 MB |
| l-EDS cartesian | `eds2leds` | 10 MB, 10% var | 5.4 MB/s | ~150 MB |
| l-EDS linear | `eds2leds` | 10 MB, 10% var | 4.3 MB/s | ~160 MB |
| Statistics | `edsparser-stats` | 10 MB | >100 MB/s | ~5 MB |
| Pattern gen | `edsparser-genpatterns` | 10 MB, 1K patterns | <1s | ~5 MB |

Run `bash tests/bench/bench.sh` to reproduce on your system.

---

## Practical Tuning Guide

### "My eds2leds is using too much memory"

`eds2leds` already uses streaming architecture. If memory is unexpectedly
high:

1. Check that you are using the current build (post-2026-05 streaming arch).
2. Reduce `--threads` — each thread holds one symbol in RAM; for pathologically
   large symbols this can matter.
3. The batch size (default 1 000 pairs) is hardcoded; lower it if symbols
   are very long and you have limited RAM. This requires a code change.

### "My vcf2eds is using too much memory"

Reduce `--block-size`. Start with `--block-size 1000000` (1 Mbp) and
increase until you hit your memory budget.

### "My eds2leds is slow"

1. Check the complexity warning. High `adjacent_degenerate_pairs` means
   many iterations are needed.
2. Use `--threads N` to parallelize batch computation. Most benefit comes
   from 4–8 threads on typical genomic data.
3. Use **LINEAR** merging (provide `-s`): LINEAR prunes invalid paths,
   producing fewer merge candidates and shorter result strings.
4. Ensure the temp directory is on a fast (SSD) filesystem. The default
   is `$TMPDIR` / `/tmp`. Override by setting `TMPDIR` before running.

### "I need very large SEDS LRU cache"

```cpp
auto eds = EDS::load("data.eds", "data.seds");
eds.set_source_cache_capacity(1'000'000);  // 1M entries ≈ 40 MB
```

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
