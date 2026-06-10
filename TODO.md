# EDSParser – known issues and planned work

---

## Planned Features

### RandomAccess — pointer into EDS data at a located position

**Motivation:** `BioFMI::locate()` returns `(position, changes)` pairs where `position` is
a character-level offset in T₀ and `changes` is a list of **absolute** (0-based global)
alternative indices. Currently there is no way to go from that result back into the EDS
data without re-parsing the whole file. `RandomAccess` closes that gap by returning a
lightweight pointer (file offset + symbol metadata) into the underlying data so the caller
can stream from that point without materialising the string.

**Proposed signature:**

```cpp
// Returns a pointer into the EDS data at the given character position,
// following the given absolute alternative indices when traversing degenerate sets.
// 'changes' uses the same 0-based global numbering as BioFMI::locate() output.
struct EDSPointer {
    std::streampos file_offset;   // byte offset in the .eds / .leds file
    size_t         symbol_idx;    // EDS symbol index (0-based)
    size_t         string_idx;    // alternative index within that symbol (0-based)
    Position       char_offset;   // character offset within that alternative
};

EDSPointer random_access(Position char_pos,
                         const std::vector<int>& changes) const;
```

**Design notes:**
- `char_pos` is a character-level position in the same coordinate space as
  `locate()` results: 0-based index in T₀ if the position is in reference,
  `base_position_of_set + offset_within_alternative` if inside a degenerate alternative.
- `changes` carries **absolute** global alternative indices (the same `changes` vector
  returned by `BioFMI::locate()`), not per-symbol local indices.
- The method must map from that coordinate space back to a `(symbol_idx, string_idx,
  char_offset)` triple using `metadata_.cum_common_positions` and
  `metadata_.cum_degenerate_counts`, then seek to the byte in the file.
- In FULL (in-memory) mode: `file_offset` may be unused; `string_idx` / `char_offset`
  index directly into `sets_`.
- Distinct from the existing `extract(symbol_pos, len, per_symbol_changes)` which takes a
  symbol-level position and per-symbol local alternative indices.

**Implementation sketch:**
1. Determine which symbol owns `char_pos` by binary search on
   `metadata_.cum_common_positions`.
2. Map the global `changes` entry for that symbol to a local string index within the
   symbol.
3. Seek to `metadata_.base_positions[symbol_idx]`, scan past the selected alternative's
   byte offset (using `metadata_.string_lengths`).
4. Return `EDSPointer`.

---

### Extract — materialise a substring starting at a located position

**Motivation:** Complement of `RandomAccess`. Given a `(position, changes)` pair from
`BioFMI::locate()` and a length in characters, return the actual string content. This is
needed to verify match results, compute flanking sequences, or feed downstream tools.

**Proposed signature:**

```cpp
// Extract 'len' characters starting at 'char_pos', following 'changes' across
// degenerate set boundaries as needed.
// 'changes' uses the same 0-based global numbering as BioFMI::locate() output.
String extract_at(Position char_pos,
                  const std::vector<int>& changes,
                  Length len) const;
```

**Relationship to existing `extract()`:**

The existing method (`eds.hpp:124`) takes a **symbol index** and **per-symbol** local
alternative indices:

```cpp
String extract(Position symbol_pos, Length len, const std::vector<int>& per_symbol_changes);
```

`extract_at` takes a **character position** and **absolute global** alternative indices,
matching `locate()` output directly. Internally it can delegate to `random_access()` to
find the starting point and then read forward `len` characters, crossing symbol boundaries
using `metadata_` to follow the correct alternatives.

**Design notes:**
- Must handle spans that cross multiple EDS symbols (both degenerate and non-degenerate).
- `changes` may be shorter than the number of degenerate sets traversed if the span ends
  before the last set; validate accordingly.
- In METADATA_ONLY mode uses `read_symbol()` per traversed symbol; in FULL mode uses
  `sets_` directly.
- Should be a CLI-exposed tool (`edsparser-extract`) as well as a library method, mirroring
  the pattern of other tools in `src/cpp/tools/`.

---

## Format Genericity — EDZ_COMPRESSED

EDZ (binary, uncompressed) is fully implemented: `parse_edz()`, `read_from_edz()`, `save_edz()`,
the guard in `load()` is lifted, and 6 unit tests cover basic load, auto-detect, save/load
roundtrip, multi-byte varints, LRU cache eviction, and error handling.

**Remaining:** EDZ_COMPRESSED follows the same pattern but steps 2/4/5 additionally wrap
data blobs with zstd block compression; the index stores compressed block boundaries instead of
raw per-entry offsets. The `parse_edz_compressed`, `read_from_edz_compressed`, and
`save_edz_compressed` stubs in `sources.cpp` are the entry points.

The `copy_range_to_stream()` slow fallback (re-serialise via `read_source`) is the correct
interim path for both EDZ and EDZ_COMPRESSED — it is only called from `eds2leds --linear` SEDS
output, so no format-specific fast path is needed until EDZ output mode is added.

---

## Optimizations

### [ARCH] eds2leds per-symbol throughput scales inversely with variability
- **Observed (2026-05-27, 10 MB ref, cartesian, --min-context 5):**
  1% variability → ~50 MB/s; 10% variability → ~5.6 MB/s (~9× drop)
  even when input already satisfies the l-EDS constraint (no merging needed)
- **Previous numbers (pre I/O opt, 2026-05):** 1% → 173 MB/s; 10% → 2.9 MB/s (60× drop).
  The 1% figure dropped after I/O optimisations because the old measurement used the
  installed binary (which loaded small files into FULL mode); the 10% figure slightly
  improved (2.9 → 5.6 MB/s) from the seek-guard and SEDS-batching fixes.
- **Root cause:** per-symbol string concatenation and merge-metadata overhead in
  METADATA_ONLY streaming mode; 10% variability produces ~10× more degenerate sets
  than 1%, each requiring string reads, cartesian concatenation, and output writes.
  (Raw disk I/O was a contributor pre-2026-05 but has been largely addressed.)
- **Decision:** do NOT add block-parallel processing to recover this throughput
  Rationale: parallel blocks require holding multiple blocks in RAM simultaneously,
  contradicting the core design goal of constant memory for arbitrarily large files.
  **Memory stability is higher priority than transformation speed.**
- **Acceptable tradeoff:** a one-time transformation of a 100 GB file taking extra
  minutes is fine; running out of memory on a laptop is not

---

## Experiments / Validation

All items below correspond to numbers in `docs/performance.md` that are either
theoretical estimates or measured only on synthetic/unspecified hardware.
Each experiment should produce a plot or table that replaces the placeholder claim.

### 1. Verify eds2leds convergence claim on real genomic data

`docs/algorithms.md` and `performance.md` state iteration counts for MSA-derived
(2–4) and VCF-derived (3–6) data. Never validated — only synthetic EDS tested.

Run `eds2leds` on representative real datasets (e.g. 1000 Genomes VCF→EDS,
human MSA) and record iteration count per l value. Update both files.

### 2. MSA throughput and memory vs alignment size / sequence count

`performance.md` claims "50–200 MB/s" and the scalability table is pure calculation.
No actual `msa2eds` benchmark exists.

Benchmark: vary alignment length (1 MB, 10 MB, 100 MB, 1 GB) and sequence count
(100, 1K, 10K) — measure runtime, throughput (MB/s), and peak RSS.
Confirm the memory formula matches observed values.

### 3. VCF block size trade-off: memory vs throughput

The block-size table in `performance.md` is theoretical (memory formula only).
No throughput or wall-clock measurements exist for different `--block-size` values.

Benchmark `vcf2eds` on a real or large synthetic VCF with block sizes
1M, 5M, 10M (default), 50M, 100M — measure peak RSS and runtime.
Produce a plot of memory vs block-size and time vs block-size.

### 4. eds2leds memory validation on large files

The old-vs-new memory table (100×–3000× reduction) is projected from the
architecture analysis, not from actually running 1 GB / 10 GB / 100 GB inputs.

Run `eds2leds` (cartesian and linear) on inputs of 100 MB, 1 GB, 10 GB with
`MemoryMonitor` or `/usr/bin/time -v`. Record actual peak RSS and compare to
the formula. Update the table with real numbers and document the machine used.

### 5. OpenMP thread-count scaling

No data exists on how throughput scales with `--threads` for
`compute_merge_metadata`. The mutex on `Sources::read_source()` may cause
contention that limits scaling.

Benchmark `eds2leds --linear --threads 1,2,4,8,16` on a fixed 1 GB input.
Plot throughput vs thread count. Identify the saturation point.

### 6. LRU cache hit rate measurement

`performance.md` claims "~98% hit rate" but this is asserted, not measured.
The claim also notes "> 100 GB SEDS → 99% with 100K cache" — also unmeasured.

Instrument `Sources::read_source()` with a hit/miss counter (behind a
compile-time flag). Run `eds2leds --linear` on real data with varying cache
capacities (1K, 10K, 100K). Record actual hit rate. Update the table.

### 7. Linear vs Cartesian on real genomic data

The 1.26× cartesian/linear throughput ratio was measured on synthetic data
(10% variability, 4-path round-robin, `genrandomeds`). Real VCF/MSA data
may have different alternative counts and source distributions.

Run both modes on real data. Record throughput ratio and output size ratio
(linear prunes invalid paths — quantify the compression).

### 8. Document hardware for all benchmarks

The "Benchmark Baseline (2026-05 Reference System)" table in `performance.md`
does not specify the machine. All numbers are therefore not reproducible.

Re-run `bench.sh --size standard` on a documented machine (CPU model, core
count, RAM, storage type). Add a "Hardware" section to `performance.md`.
Use `bench_plot.py` — it now embeds machine info in plot footers automatically.