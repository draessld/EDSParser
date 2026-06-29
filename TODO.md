# EDSParser – known issues and planned work

---

## High-Priority Optimizations

These miss their intended targets on real population-scale workloads — the fast path exists in code
but never fires, so the hot case always takes the slow path.

### ~~[ARCH] eds2leds rewrites the entire file + re-indexes the entire SEDS every merge iteration~~ FIXED (2026-06-28)

`select_independent_merge_pairs()` replaced by `select_merge_groups()`:
detects maximal contiguous chains where every adjacent pair needs merging and
emits each chain as one `MergeGroup` (k positions → one output symbol in one
pass).  `compute_merge_metadata()` folds k positions iteratively (length sums;
bitset source intersections).  `stream_merged_symbols_to_file()` reads all k
symbols sequentially and concatenates via `valid_indices_flat[m*k+j]`.
Result: a chain of L degenerate positions that previously required L−1 full-file
passes now completes in **1 iteration**.

Bench validation (`tests/bench/scenarios/scenario_chain_merging.sh`, `--min-context 0`
inputs, `--size standard`): 1 iteration (was 3-4 with old pairwise code); cartesian
**2.1×** faster (22 MB/s vs 10 MB/s at 1% var), **2.4×** at 5% var.  All 7 ctest
tests pass.

---

## Medium-Priority Optimizations

Real improvements, but not blockers on real-world use.

### [MEM] `all_metadata` accumulates a whole iteration's merge results in RAM

- **Location:** merge loop in `eds_to_leds_linear()` / `eds_to_leds_cartesian()` in
  `src/cpp/lib/transforms/eds_transforms.cpp`
- **Problem:** the `BATCH_SIZE` batching comment claims it "controls parallel memory", but every
  pair's `MergeMetadata` is appended to `all_metadata` and retained for the entire iteration before
  streaming. For CARTESIAN merges each entry stores `merged_string_lengths` + `valid_indices` sized
  `set1×set2`; in a dense/exponential region this is the dominant allocation and is **not** bounded
  by `BATCH_SIZE` — batching only caps the transient compute working set.
- **Idea:** stream each batch's results to the output as they are computed instead of accumulating
  all of them, or cap retained metadata and spill.

### [MEM] `read_symbol()` copies the whole symbol by value

- **Location:** `EDS::read_symbol()` in `src/cpp/lib/formats/eds.cpp`
- **Problem:** returns `StringSet` (a `vector<string>`) by value; in FULL mode `return sets_[pos]`
  is a full copy. Hot loops (final serialization, `print()`, `stream_merged_symbols_to_file()`) copy
  every symbol's strings on each access.
- **Idea:** add a `const StringSet&` overload for in-memory mode; keep the by-value path only for
  METADATA_ONLY where the set is freshly constructed.

---

## Planned Features

### Format Genericity — EDZ_COMPRESSED

EDZ (binary, uncompressed) is fully implemented: `parse_edz()`, `read_from_edz()`, `save_edz()`,
the guard in `load()` is lifted, and 6 unit tests cover basic load, auto-detect, save/load
roundtrip, multi-byte varints, LRU cache eviction, and error handling.

**Remaining:** EDZ_COMPRESSED follows the same pattern but steps 2/4/5 additionally wrap data blobs
with zstd block compression; the index stores compressed block boundaries instead of raw per-entry
offsets. The `parse_edz_compressed`, `read_from_edz_compressed`, and `save_edz_compressed` stubs in
`sources.cpp` are the entry points.

The `copy_range_to_stream()` slow fallback (re-serialise via `read_source`) is the correct interim
path for both EDZ and EDZ_COMPRESSED — it is only called from `eds2leds --linear` SEDS output, so
no format-specific fast path is needed until EDZ output mode is added.

---

## Experiments / Validation

All items below correspond to numbers in `docs/performance.md` that are either theoretical estimates
or measured only on synthetic/unspecified hardware. Each experiment should produce a plot or table
that replaces the placeholder claim. Start with #8 (hardware docs) to make all other results
reproducible.

### 8. Document hardware for all benchmarks *(do first)*

The "Benchmark Baseline (2026-05 Reference System)" table in `performance.md` does not specify the
machine. All numbers are therefore not reproducible.

Re-run `bench.sh --size standard` on a documented machine (CPU model, core count, RAM, storage
type). Add a "Hardware" section to `performance.md`. Use `bench_plot.py` — it now embeds machine
info in plot footers automatically.

### 5. OpenMP thread-count scaling *(measure improvement from concurrency fix)*

The `--threads` mutex contention fix (2026-06-14) preloads all source sets before the parallel
region so workers never take `Sources::io_mutex_`. Throughput scaling with thread count has not yet
been measured post-fix.

Benchmark `eds2leds -s file.seds --threads 1,2,4,8,16` on a fixed 1 GB input. Plot throughput vs
thread count before and after the fix. Identify the saturation point and confirm linear speedup in
the low-thread regime.

### 6. LRU cache hit rate measurement *(motivates [MEM] std::set fix)*

`performance.md` claims "~98% hit rate" but this is asserted, not measured. The claim also notes
"> 100 GB SEDS → 99% with 100K cache" — also unmeasured.

Instrument `Sources::read_source()` with a hit/miss counter (behind a compile-time flag). Run
`eds2leds --linear` on real data with varying cache capacities (1K, 10K, 100K). Record actual hit
rate. Update the table.

### 4. eds2leds memory validation on large files

The old-vs-new memory table (100×–3000× reduction) is projected from the architecture analysis, not
from actually running 1 GB / 10 GB / 100 GB inputs.

Run `eds2leds` (cartesian and linear) on inputs of 100 MB, 1 GB, 10 GB with `MemoryMonitor` or
`/usr/bin/time -v`. Record actual peak RSS and compare to the formula. Update the table with real
numbers and document the machine used.

### 1. Verify eds2leds convergence claim on real genomic data

`docs/algorithms.md` and `performance.md` state iteration counts for MSA-derived (2–4) and
VCF-derived (3–6) data. Never validated — only synthetic EDS tested.

Run `eds2leds` on representative real datasets (e.g. 1000 Genomes VCF→EDS, human MSA) and record
iteration count per l value. Update both files.

### 7. Linear vs Cartesian on real genomic data

The 1.26× cartesian/linear throughput ratio was measured on synthetic data (10% variability, 4-path
round-robin, `genrandomeds`). Real VCF/MSA data may have different alternative counts and source
distributions.

Run both modes on real data. Record throughput ratio and output size ratio (linear prunes invalid
paths — quantify the compression).

### 2. MSA throughput and memory vs alignment size / sequence count

`performance.md` claims "50–200 MB/s" and the scalability table is pure calculation. No actual
`msa2eds` benchmark exists.

Benchmark: vary alignment length (1 MB, 10 MB, 100 MB, 1 GB) and sequence count (100, 1K, 10K) —
measure runtime, throughput (MB/s), and peak RSS. Confirm the memory formula matches observed values.

### 3. VCF block size trade-off: memory vs throughput

The block-size table in `performance.md` is theoretical (memory formula only). No throughput or
wall-clock measurements exist for different `--block-size` values.

Benchmark `vcf2eds` on a real or large synthetic VCF with block sizes 1M, 5M, 10M (default), 50M,
100M — measure peak RSS and runtime. Produce a plot of memory vs block-size and time vs block-size.

