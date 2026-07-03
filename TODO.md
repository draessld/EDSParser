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

### ~~[ARCH] EDS pass-through parses + re-serialises every unmodified symbol (should raw-copy)~~ FIXED (2026-07-03)

BRANCH B of `stream_merged_symbols_to_file()` now raw-copies unmodified
full-bracket symbols instead of parsing + re-serialising them. Implementation:
- New `EDS::copy_symbol_range_to_stream(start, count, out)`
  (`src/cpp/lib/formats/eds.cpp`) mirrors `Sources::copy_range_to_stream`: bulk
  byte range `[base_positions[start], base_positions[start+count])` copied in
  64 KB chunks, with an EOF/`stream_file_size()` fallback for a range reaching
  the last symbol. File size is derived from the open stream (not the path) so it
  works on unlinked-but-open temp files.
- BRANCH B batches consecutive raw-copyable symbols and flushes them in one call,
  exactly like the existing SEDS batch. A symbol is raw-copyable when its input
  span equals its full-bracket byte length (`Σ string_lengths + sym_size + 1`) —
  true for every intermediate temp file (iteration ≥ 1) and for full-bracket,
  whitespace-free input at iteration 0. Compact input, inter-symbol whitespace,
  or the final symbol fall back to the (now bulk-read) parse-and-reserialise path.
- Output byte offsets are tracked manually (`out_pos`) instead of via
  `eds_out.tellp()`, because batched raw copies defer physical writes past the
  point where each symbol's metadata is recorded.

Validation: byte-identical output vs the pre-change binary across cartesian +
linear, compact + full-bracket input, `--full` + compact output, l∈{2,3,4,5},
variability∈{0.01…0.10} (differential harness). A/B on 20 MB inputs:
**cartesian 1.9–2.2×**, **linear 1.16–1.63×** faster. Unit tests
`test_copy_symbol_range_to_stream` (Test A6) + e2e
`test_raw_copy_passthrough_roundtrip`.

### ~~[PERF] `read_symbol_from_stream()` scans char-by-char despite knowing exact lengths~~ FIXED (2026-07-03)

`EDS::read_symbol_from_stream()` now bulk-reads the symbol's exact byte span with
a single `stream_.read()` (span `[base_positions[pos], base_positions[pos+1])`, or
`[base_positions[pos], file_size)` for the last symbol) and scans it in memory:
the full-bracket body is split on `,` with bulk substring construction, and the
compact body is one bulk substring. This removes the three-`peek()`-per-byte loop
condition and per-char string growth. Interior whitespace is still stripped
defensively (slow path) so behaviour is unchanged for the Test-5 whitespace case.
Helps every METADATA_ONLY `read_symbol` caller (the merge fallback path, both
final-serialisation loops, `generate_patterns`, `extract`). Unit test
`test_read_symbol_bulk_edge_cases` (Test A5). Behaviour verified identical via the
differential harness above.

---

## Bugs

### [TEST] e2e golden files are stale for eds2leds / msa2eds / vcf2eds (pre-existing)

- **Observed:** `tests/e2e/run_all.sh` reports 3/6 suites failing — every failure is
  `output differs from expected` against the checked-in `tests/e2e/expected/**` golden
  files (eds2leds: 6 goldens; msa2eds and vcf2eds: several each).
- **Not caused by the 2026-07-03 raw-copy/bulk-read work:** the pre-change `eds2leds`
  binary produces the **same** 12 eds2leds e2e failures, and the msa2eds/vcf2eds suites
  (untouched by that work) fail too. The raw-copy change is proven byte-identical to the
  old serialiser by a differential harness (cartesian + linear, compact + full-bracket
  I/O, l∈{2..5}, var∈{0.01..0.10}).
- **Likely cause:** the uncommitted working-tree changes to `sources.cpp`, `vcf2eds.cpp`,
  `msa2eds.cpp`, `stats.cpp`, `eds2leds.cpp` (sparse SEDS / EDZ format work) changed tool
  output without regenerating the goldens.
- **Fix:** confirm the current output of each tool is correct (valid EDS/SEDS/l-EDS),
  then regenerate `tests/e2e/expected/**` from it. Do **not** blind-overwrite — that would
  mask any real regression hiding in the in-progress format work. The C++ ctest suite
  (`test_eds … test_integration`) is fully green, so the divergence is confined to the
  golden-file e2e layer.

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

### [MEM] `read_source()` copies the PathSet even on a cache hit

- **Location:** `Sources::read_source()` in `src/cpp/lib/formats/sources.cpp` (~L1118, `return
  cache_it->second->paths;`) and `get_src` in `compute_merge_metadata()` in
  `src/cpp/lib/transforms/eds_transforms.cpp` (~L232, returns `PathSet` by value even from the
  `preloaded` map)
- **Problem:** every source access on the merge hot path and in `calculate_path_intersection()`
  heap-allocates a fresh `vector<int>` copy, even when the data is already sitting in the LRU
  cache or the preload map. `read_source_ref()` exists precisely to avoid this but is not used
  on these paths.
- **Idea:** use `read_source_ref()` / return `const PathSet&` on single-threaded paths (the
  preload map is already immutable during the parallel region, so a ref into it is safe there
  too). Small per-access win, but it is a per-string hot path.

### [PERF] `parse()` appends one char at a time from an in-RAM buffer

- **Location:** `EDS::parse()` in `src/cpp/lib/formats/eds.cpp` (`bracketed_content += ch` ~L177,
  `current_token += ch` ~L206)
- **Problem:** the scan already holds a 64 KB buffer in RAM, but symbol/token bodies are built
  one char at a time (repeated reallocation for long context blocks).
- **Idea:** find the delimiter within the buffer and bulk-append the run. Amortised O(1) today
  so low priority, but free load-time throughput.

### [PERF] `select_merge_groups()` calls `needs_merge()` twice per position

- **Location:** `select_merge_groups()` in `src/cpp/lib/transforms/eds_transforms.cpp` (~L149, L154)
- **Problem:** each position is tested by `needs_merge()` in both the outer scan and the inner
  chain-extension, and each `needs_merge()` re-fetches `eds.get_metadata()` several times. O(n)
  overall so not a hotspot, but redundant.
- **Idea:** hoist the metadata reference and memoise the previous `needs_merge()` result so each
  position is evaluated once.

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

### Source Format Conversion Tool — `edsparser-source-transform`

Currently there is no way to convert between source formats (SEDS ↔ EDZ ↔ EDZ_COMPRESSED) without
re-running a full EDS transformation from scratch. A dedicated conversion tool would allow users to:

- Compress an existing `.seds` file to `.edz` or `.edz_compressed` after the fact (reducing disk
  footprint 3–10× without re-parsing the input data)
- Decompress `.edz` back to `.seds` for inspection or compatibility with external tools
- Re-compress with a different codec or block size

**Planned interface:**
```
edsparser-source-transform -i input.seds  -o output.edz              # text → binary
edsparser-source-transform -i input.edz   -o output.edz_compressed   # binary → compressed
edsparser-source-transform -i input.edz   -o output.seds             # binary → text (decompress)
edsparser-source-transform -i input.seds  -o output.edz --verify     # round-trip check
```

**Implementation path:** thin wrapper around `Sources::save_edz()` /
`Sources::save_edz_compressed()` / `Sources::parse_seds()` — the actual codec logic lives in
`sources.cpp`; the tool only handles CLI argument parsing and streaming I/O. Requires EDZ_COMPRESSED
to be fully implemented first (see entry above).

### l-EDS (`-l`) merge output never writes sparse or EDZ sources

- **Location:** `eds_to_leds_linear()` in `src/cpp/lib/transforms/eds_transforms.cpp` (both the
  stream-based and path-based overloads) — the merged-output sources writer at ~L735-745 is a
  hardcoded `*sources_out << '{' ... '}'` loop, and reload uses `Sources::load(temp_seds_out,
  Sources::Format::SEDS)` unconditionally (~L1066).
- **Problem:** `vcf2eds -l N` (two-stage VCF→EDS→l-EDS) and `eds2leds -l N` both always write their
  merged output `.seds` as dense text SEDS, regardless of the input source format. Before
  2026-07-02 this silently mislabeled the file: `vcf2eds -l N -z` opened the output in binary mode
  and named it `.edz`, but wrote plain SEDS text into it — an unreadable, corrupted file. Fixed by
  forcing dense-text naming/mode whenever `create_leds` is true and printing a warning when `-z` is
  given together with `-l` (see `vcf2eds.cpp`); `eds2leds` already named its merge output `.seds`
  honestly, so no corruption there — just an undocumented always-downgrades-to-SEDS limitation.
- **Idea:** give `stream_merged_symbols_to_file()` / the SEDS batching path a sparse and/or EDZ
  writer (mirroring `Sources::write_seds_sparse_finalize()` / `write_edz_entry()`) so `-l` mode can
  actually honor the requested source format instead of always falling back to dense SEDS.

### `PathSet` complement encoding requires knowing total path count — SEDS text has no header for it

- **Location:** `Sources::parse_seds()` in `src/cpp/lib/formats/sources.cpp`.
- **Problem:** `PathSet` uses complement encoding (`{0,e1,e2,...}` = all paths except `e1,e2,...`;
  see `sources.hpp`), which `write_seds_entry()` in `vcf_transforms.cpp` uses automatically whenever
  a variant is present in >50% of paths — common on real data. Correctly expanding a complement set
  to its true size requires the total path universe size, which EDZ stores in its 24-byte header but
  text SEDS never encodes anywhere. Before 2026-07-02, `edsparser-stats`'s `compute_source_stats()`
  just took `PathSet::size()` directly, silently under-reporting "paths per string" for any
  complement-encoded entry (and for `{0}` pure-universal entries in *both* formats).
- **Fix landed:** `parse_seds()` now infers `num_paths_` as the largest path-ID token seen anywhere
  in the file (explicit members or complement exceptions), during the same single pass that already
  builds the entry-position index — no extra I/O. `compute_source_stats()` uses it to expand
  complement/universal sets to their true size.
- **Known residual limitation:** the inference undercounts in the degenerate case where the true
  maximum path ID is present in literally every entry and never appears explicitly anywhere in the
  file (never listed as an explicit member, never listed as a complement exception). This is rare in
  practice (some entry almost always has a small enough explicit set, or excludes that path
  somewhere) but not impossible on adversarial input. A fully robust fix would need text SEDS to
  carry an explicit path-count header (format change) or for callers to pass `num_paths` in from
  external context (e.g. VCF sample count).

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

