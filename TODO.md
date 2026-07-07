# EDSParser – known issues and planned work

---
## Planned Features


### Format Genericity — EDZ_COMPRESSED *(implemented 2026-07-07)*

**Done:** EDZ_COMPRESSED is a zstd-block-compressed variant of the dense EDZ bitset. The dense
per-entry bitset data section is split into ~256 KiB blocks; each block is compressed with
`ZSTD_compress` and preceded by a block index (compressed offset/size + uncompressed size per
block). Reads compute `block = string_id / entries_per_block`, decompress that block once via
`ZSTD_decompress`, cache it (single-block cache guarded by `io_mutex_`, so sequential reads only
decompress each block once), and slice out the entry.

- **Layout** (`sources.cpp`): 40-byte header `magic(4) | flags(2,=0x0003) | reserved(2) |
  cardinality(8) | num_paths(8) | entries_per_block(8) | num_blocks(8)`, then `num_blocks × 16`
  index entries, then the concatenated zstd frames. `detect_format()` already routed `flags & 0x0001`
  to EDZ_COMPRESSED; `load()` now parses instead of throwing.
- **Entry points filled in:** `parse_edz_compressed()`, `read_from_edz_compressed()`,
  `save_edz_compressed()` (were stubs). `save_as()` / `save()` dispatch to them; the
  `edsparser-source-transform` gate is removed so `--to edz_compressed` works.
- **Optional dependency:** gated on `EDSPARSER_HAVE_ZSTD` (CMake `find_path`/`find_library` for
  zstd, searching `$CONDA_PREFIX`/`$HOME`/system). Without zstd the three functions throw a clear
  "built without zstd" error and nothing else changes. `Sources::edz_compressed_available()` exposes
  build support to tools/tests.
- **All EDZ variants keep the `.edz` extension** (self-describing via flags); force compression with
  `--to edz_compressed`, auto-detect resolves it back on load.
- **Tests:** unit `test_edz_compressed_multiblock` (large num_paths → multi-block, out-of-order reads)
  + updated `test_save_as_conversions`; e2e `SEDS↔EDZ_COMPRESSED round-trip (zstd)` in
  `test_source_transform.sh` (skips cleanly when built without zstd).

The `copy_range_to_stream()` slow fallback (re-serialise via `read_source`) remains the correct path
for EDZ_COMPRESSED — it is only called from `eds2leds --linear` SEDS output, so no format-specific
fast path is needed until an EDZ output mode is added to the merge pipeline.

### Source Format Conversion Tool — `edsparser-source-transform` *(implemented)*

**Done:** `src/cpp/tools/source_transform.cpp` converts a source file between all implemented
formats (SEDS, SEDS_SPARSE, EDZ, EDZ_SPARSE, and EDZ_COMPRESSED on a zstd-enabled build) without
re-running a full EDS transformation. It is a
thin wrapper around the new `Sources::save_as(path, Format)` method (`sources.cpp`), which reads each
entry via format-agnostic `read_source()` and re-encodes into the requested format. `save_seds` was
refactored to share `append_seds_set()`; `save_edz` shares `effective_num_paths()` with the new
`save_seds_sparse` / `save_edz_sparse` writers. Unit test: `test_save_as_conversions` in
`tests/unit/test_sources.cpp`.

Interface:
```
edsparser-source-transform -i input.seds -o output.edz               # text → binary EDZ
edsparser-source-transform -i input.edz  -o output.seds              # binary → text
edsparser-source-transform -i input.seds -o output.edz --sparse      # → EDZ_SPARSE
edsparser-source-transform -i input.seds -o output.edz --verify      # semantic round-trip check
edsparser-source-transform -i in -o out --from seds --to edz_sparse  # explicit formats
```
Input format auto-detected (override `--from`); output inferred from extension (override `--to`;
`--sparse` picks the sparse variant). `--verify` collapses the two universal spellings (`{0}` and the
explicit full universe `{1..num_paths}`, which EDZ canonicalizes to `{0}`) before comparing.

EDZ_COMPRESSED is now available as a conversion target/source through `--to edz_compressed` /
auto-detect (see the EDZ_COMPRESSED entry above); on a build without zstd the tool surfaces the
library's "built without zstd" error instead of writing a corrupt file.

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

### `PathSet` complement encoding requires knowing total path count — SEDS text has no header for it *(solved 2026-07-06)*

- **Location:** `Sources::parse_seds()` / writers in `src/cpp/lib/formats/sources.cpp`.
- **Problem:** `PathSet` uses complement encoding (`{0,e1,e2,...}` = all paths except `e1,e2,...`;
  see `sources.hpp`), which `write_seds_entry()` uses automatically whenever a variant is present in
  >50% of paths — common on real data. Correctly expanding a complement set to its true size requires
  the total path universe size, which EDZ stores in its 24-byte header but text SEDS never encoded.
  Inference (largest path-ID token seen) was the interim fix but undercounts in the degenerate case
  where the true maximum path ID is present in every entry yet never appears explicitly anywhere.
- **Fix landed (format change, backward compatible):** SEDS now persists `num_paths` in a
  self-describing trailer, so the loader reads it exactly instead of inferring:
  - sparse: `bitvec | "SED2"(4) | cardinality(8) | m_degen(8) | num_paths(8)` (was 20-byte `"SEDS"`)
  - dense:  `text | "SEDN"(4) | cardinality(8) | num_paths(8)` (dense previously had no trailer)
  `parse_seds()` detects `SED2`/`SEDS`/`SEDN`/none and only falls back to max-path-ID inference for
  legacy (trailerless / `"SEDS"`) files, so every pre-existing `.seds` still loads. All producers emit
  the trailer with the true universe: `vcf2eds -s/-z/-l` (`n_samples`), `msa2eds` (`n_sequences`),
  `eds2leds`/l-EDS merge (`MergeStreamWriter::finish()` carries it through iterations),
  `Sources::save_seds`/`save_seds_sparse`. `genrandomeds` intentionally has no trailer — its
  round-robin output writes the explicit full universe `{1..n}`, so inference is always exact.
- **`--verify` also fixed:** `edsparser-source-transform --verify` now expands complement sets with
  the (now-reliable) `num_paths` before comparing (`canonicalize_expanded()`), instead of collapsing
  only the universal spelling — it previously reported false "source set mismatch" on any
  complement-encoded entry (i.e. on essentially all real VCF sources).
- **Tests:** unit `test_num_paths_trailer` (adversarial case: universe 10, max explicit token 5) in
  `tests/unit/test_sources.cpp`; e2e `--verify passes on complement data` + `SEDS trailer records
  num_paths` in `tests/e2e/test_source_transform.sh`; trailer-magic assertions updated in
  `test_vcf2eds.sh`; SEDS goldens regenerated.

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

