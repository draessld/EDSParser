# EDSParser – known issues and planned work

---

## High-Priority Optimizations

These miss their intended targets on real population-scale workloads — the fast path exists in code
but never fires, so the hot case always takes the slow path.

### [ARCH] eds2leds rewrites the entire file + re-indexes the entire SEDS every merge iteration

- **Location:** `eds_to_leds_linear()` / `eds_to_leds_cartesian()` in
  `src/cpp/lib/transforms/eds_transforms.cpp`
- **Problem:** `select_independent_merge_pairs()` only picks non-overlapping adjacent pairs, so a run
  of L symbols that must collapse needs multiple iterations. Each iteration streams ALL positions to a
  fresh temp file (even the ~99% unmodified passthroughs), and linear mode re-parses the WHOLE new
  SEDS via `Sources::load()` to rebuild `base_positions_` from scratch. Total cost is
  `O(num_iterations × total_file_size)`, not `O(actual_merge_work)`. This is the dominant
  algorithmic ceiling at scale and the reason throughput sits at single-digit MB/s.
- **Idea:** resolve transitive merge chains in one pass (union-find over adjacent merge candidates)
  so the number of full-file passes is ~constant rather than proportional to chain length. Constrained
  by the constant-memory design goal — do not hold multiple blocks in RAM.

---

## Medium-Priority Optimizations

Real improvements, but not blockers on real-world use.

### ~~[I/O] `read_fasta_region()` reads the reference one base at a time~~ FIXED (2026-06-27)

Replaced the `while (fasta_stream.get(c))` loop with bulk `read()` per FASTA
line segment (O(length/line_width) reads instead of O(length) get() calls).
Handles '\r\n' CRLF line endings too. Location: `vcf_transforms.cpp:~120`.

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

### [DISK] ~2× temp footprint plus a full up-front input copy

- **Location:** `eds_to_leds_linear()` / `eds_to_leds_cartesian()`
- **Problem:** each iteration writes `iter_N.eds` while `iter_(N-1).eds` still exists (deleted only
  afterward) → peak temp ≈ 2× current file size, on top of the initial full copy of the input to
  `input.eds`. For 100 GB inputs that is a real provisioning constraint not reflected in the
  memory-only figures in `performance.md`.
- **Action:** at minimum document the temp-disk requirement; ideally delete the predecessor
  before/while writing the successor where the iteration chain allows.

### [CLEANUP] Dead/duplicated code in `eds.cpp`

- `EDS::calculate_statistics()` is now dead weight — `parse()` computes the same stats inline.
  ~100 lines that can silently diverge from `parse()`; remove or repurpose.
- `read_symbol_from_stream()` parses bracket bodies char-by-char; secondary to the items above
  but rides the same cleanup pass.

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

---

## Fixed (2026-06-14)

### [UB] Data race on `first_exception` in the OpenMP merge loop

Introduced `std::atomic<bool> exception_occurred` as the lockless early-out guard. `first_exception`
(the `exception_ptr`) is now only written/read inside `#pragma omp critical`.

### [UB] `std::isspace(ch)` on a raw `char` in bracket parsing

Full-bracket path (`read_symbol_from_stream()`) now casts to `unsigned char`, matching the compact
path. Same fix applied to the `remove_if` call in the string constructor.

### [VERIFY] VCF block-boundary variants duplicate/overlap reference

A variant whose REF span crossed the block boundary caused the next block to re-emit
`ref[block_end : variant_end)`. `generate_eds_from_variants` now returns `{num_groups,
actual_cursor}`. `parse_vcf_to_eds_streaming` threads `actual_write_pos` separately from
`current_block_start` and passes it as `start_pos` for each subsequent block. Regression test: Test
13 in `tests/unit/test_vcf.cpp`.

**Follow-up (also fixed 2026-06-14):** overlapping variants split across the block boundary were
never grouped together — V1 in block N with REF reaching past block_end, and V2 in carryover
starting inside V1's REF, produced two separate degenerate symbols and duplicated the overlapping
region. Fixed by an overlap-extension loop after the main VCF reading loop: computes `max_reach`
from block N's variants and pulls any carryover variant whose start falls before it into the current
block (reading further from the VCF stream as `max_reach` grows). Regression test: Test 14 in
`tests/unit/test_vcf.cpp`.

### [MEM] [MISSES TARGET] `std::set<int>` as the universal source representation

Replaced `std::set<int>` with `PathSet` (`using PathSet = std::vector<int>`) throughout `Sources`,
`EDS`, and `eds_transforms`. The `PathSet` invariant (sorted, deduplicated) is maintained at
construction in `read_from_seds` / `read_from_edz` (sort+unique after parse). This eliminates
red-black tree node allocation per path ID: the LRU cache, `intersect_sources`, and
`merge_adjacent_sources` all use contiguous-memory vectors. `intersect_sources` now uses
`std::set_intersection` on sorted vectors (linear instead of O(n log n) tree insertion).
The bitset fast-path check drops from O(n)-per-set to O(1) — sorted vector exposes `.back()`
for the max-element test. The bitset fast path itself still caps at 63 IDs; beyond that the
sorted-merge fallback is used (competitive with bitset for real sparse source sets).

### [CONCURRENCY] [MISSES TARGET] LINEAR-mode `--threads` was effectively serial

Every `read_source()` call inside `#pragma omp parallel for` acquired `Sources::io_mutex_`,
serialising all workers. Fix: a single-threaded pass before the parallel region now preloads all
needed source sets into `std::unordered_map<size_t, PathSet> preloaded`. Workers call
`preloaded.at(idx)` (read-only, lock-free) instead. Sequential path is unchanged.
