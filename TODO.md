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

## Critical issues (code review 2026-06-12)

Found during a correctness/robustness review of the streaming + concurrency paths.
The first three are fixed; the fourth is a pre-existing correctness bug uncovered
while verifying the fixes.

### [DONE 2026-06-12] [CONCURRENCY] Deadlock in the VCF→l-EDS pipe pipeline
- **Location:** `parse_vcf_to_leds_streaming_direct()` in
  `src/cpp/lib/transforms/vcf_transforms.cpp` (production path: `vcf2eds -l`).
- **Problem:** a single producer thread wrote EDS and SEDS to two bounded 64 MB
  in-memory pipes (`make_pipe()`) *interleaved*, while the consumer
  (`eds_to_leds_linear`) drained the **entire EDS pipe to EOF before reading the
  SEDS pipe**. Once the intermediate SEDS exceeded 64 MB, the producer blocked
  writing SEDS while the consumer blocked waiting for EDS the producer could no
  longer emit → hang. Triggered precisely on the large population VCFs this path
  targets; passed on small test inputs.
- **Fix applied:** removed the pipe + producer thread; route the two stages
  through temp files (`stage1.eds` / `stage1.seds`) under a per-process temp dir,
  then feed those to `eds_to_leds_linear`. No ordering dependency remains.
  Verified byte-identical output vs the old binary on the e2e fixture.

### [DONE 2026-06-12] [ARCH] The VCF→l-EDS pipe delivered none of its claimed benefit
- **Location:** same function.
- **Problem:** the pipe's stated purpose was "no intermediate temp files", but
  `eds_to_leds_linear` copies its input streams to its own temp dir up front
  regardless — so temp files were always written. The pipe only added a thread,
  128 MB of buffers, and the deadlock above.
- **Fix applied:** subsumed by the rewrite above. Same disk cost as before, minus
  the thread/buffers/deadlock. Dropped now-unused includes (`<thread>`,
  `<atomic>`, `<exception>`, `pipe_stream.hpp`). NOTE: `pipe_buffer.hpp` /
  `pipe_stream.hpp` now have no remaining callers — candidates for deletion.

### [DONE 2026-06-12] [RESOURCE] Temp dir leaked on any mid-transform exception
- **Location:** `eds_to_leds_linear()` / `eds_to_leds_cartesian()` in
  `src/cpp/lib/transforms/eds_transforms.cpp`.
- **Problem:** `remove_all(temp_dir)` ran only on the success path. A throw in the
  merge loop (e.g. the empty-source-intersection `runtime_error`, or any I/O
  failure) propagated to `main`, stranding the iteration temp files — up to
  hundreds of GB for 100 GB+ inputs.
- **Fix applied:** added a `TempDirGuard` RAII helper (anonymous namespace) and
  installed it right after `create_directories` in both functions; removed the
  success-only cleanup. Destructor uses the `std::error_code` overload (never
  throws) and is declared before the `EDS` object so streams close before removal.

### [DONE 2026-06-12] [CORRECTNESS] vcf2eds -l produced mismatched EDS/SEDS cardinality
- **Location:** the l-EDS merge selection in `eds_to_leds_linear()`
  (`src/cpp/lib/transforms/eds_transforms.cpp`), NOT the plumbing above.
- **Problem:** `vcf2eds -i small.vcf -r small.fa -l 3` emitted an l-EDS with 28
  strings but a `.seds` with 29 source sets, so the result failed
  `EDS::load()`'s cardinality check (`edsparser-stats` reported
  "Sources cardinality (29) does not match EDS cardinality (28)").
- **Root cause (not what the first guess suggested):** the merge can leave two
  *adjacent common (non-degenerate) symbols* — e.g. `{TAAGCTTACGAT}{CGATCG}`.
  In the default **compact** output both lose their brackets and serialise as
  one bare run `TAAGCTTACGATCGATCG`; re-parsing yields a single symbol, dropping
  the EDS cardinality by one while the SEDS still has both entries. `--full`
  output was unaffected (29/29), which localised it to compact serialisation of
  adjacent commons. Confirmed pre-existing (original binary produced identical
  mismatched output — not a regression from the plumbing fixes).
- **Fix applied:** added an `ADJACENT_COMMON` merge reason and made
  `select_independent_merge_pairs()` coalesce any two adjacent non-degenerate
  symbols (regardless of length). An EDS in canonical form never has adjacent
  commons, so this keeps EDS/SEDS counts consistent and makes compact output
  lossless. Both modes now report 28/28 and load cleanly.
- **Regression guard:** added `test_vcf2leds_cardinality_consistency` to
  `tests/unit/test_integration.cpp` — runs `vcf2eds -l 2` (linear, with sources,
  compact) and asserts the `.leds`+`.seds` pair loads via `edsparser-stats -s`
  with no "does not match" error. Full suite green (7/7 ctest, 15/15 integration).

---

## Optimizations

### Code-review bottlenecks (2026-06-12)

Found during a performance review of the core library. Ordered by impact.

`[MISSES TARGET]` marks **optimizations that miss their target**: an optimized
fast path exists in the code, but it does not fire on the large population-scale
workloads the streaming design was built for, so the hot case still hits the slow
path. These are the highest-value fixes — the speedup is already half-written.

#### [DONE 2026-06-12] [I/O] EDS::parse() built the index one byte at a time
- **Location:** `EDS::parse()` in `src/cpp/lib/formats/eds.cpp`
- **Problem:** the EDS index builder used `is.peek()`/`is.get()` per byte plus a
  locale `std::isspace()` per byte — a virtual streambuf round-trip on every
  character. This is the load path hit on every file open AND once per iteration
  of the l-EDS merge loop (each iteration re-loads a temp file as METADATA_ONLY).
  The identical anti-pattern was already documented and fixed in
  `Sources::parse_seds()` (64 KB bulk reads + in-memory brace scan) but never
  ported to `EDS::parse()`.
- **Fix applied:** ported the bulk-read scanner to `parse()` as a
  BETWEEN/IN_BARE/IN_BRACKET state machine over 64 KB chunks. Semantics are
  bit-for-bit identical (verified by `test_mode_equivalence`).
- **Measured:** 387 MB EDS, end-to-end `edsparser-stats` 5.1 s → 3.9 s (~24%
  wall-clock; parse-stage share is larger). Peak memory unchanged.

#### [ARCH] eds2leds rewrites the entire file + re-indexes the entire SEDS every merge iteration
- **Location:** `eds_to_leds_linear()` / `eds_to_leds_cartesian()` in
  `src/cpp/lib/transforms/eds_transforms.cpp`
- **Problem:** `select_independent_merge_pairs()` only picks non-overlapping
  adjacent pairs, so a run of L symbols that must collapse needs multiple
  iterations. Each iteration streams ALL positions to a fresh temp file (even the
  ~99% unmodified passthroughs), and linear mode re-parses the WHOLE new SEDS via
  `Sources::load()` to rebuild `base_positions_` from scratch. Total cost is
  `O(num_iterations × total_file_size)`, not `O(actual_merge_work)`. For genomic
  data where variation is sparse but a few regions need several rounds, the whole
  genome is rewritten and re-scanned once per round. This is the dominant
  algorithmic ceiling at scale and the reason throughput sits at single-digit MB/s.
- **Idea:** resolve transitive merge chains in one pass (union-find over adjacent
  merge candidates) so the number of full-file passes is ~constant rather than
  proportional to chain length. Constrained by the constant-memory design goal —
  see the [ARCH] note below on not holding multiple blocks in RAM.

#### [MEM] [MISSES TARGET] std::set<int> is the universal source representation
- **Location:** `Sources::read_source()` and callers in
  `src/cpp/lib/formats/sources.cpp` / `sources.hpp`
- **Why it misses its target:** the `uint64_t` bitset fast path in
  `compute_merge_metadata()` / `merge_adjacent_sources()` disables itself the
  moment any path ID exceeds 63 — exactly the population-scale case (thousands of
  samples) the streaming design targets. So on real population VCFs the fast path
  *never runs* and every merge falls back to the slow `std::set` path.
- **Problem:** every `read_source()` returns a heap-allocating red-black tree BY
  VALUE (a full copy even on a cache hit); the LRU cache stores `std::set`;
  intersections allocate new sets. In the >63-path regime everything falls back to
  per-element node allocation.
- **Idea:** back the set payload with a sorted `std::vector<int>` (or
  `boost::container::flat_set`); extend the bitset path to a small fixed-width
  bitset for larger ID ranges; return cache hits by const-ref where the caller is
  single-threaded.

#### [I/O] read_fasta_region() reads the reference one base at a time
- **Location:** `read_fasta_region()` in `src/cpp/lib/transforms/vcf_transforms.cpp`
  (the `while (... fasta_stream.get(c))` loop)
- **Problem:** same char-at-a-time anti-pattern as the EDS parse fix above, on the
  path that reads the reference genome during VCF conversion. The destination
  string is already `reserve()`d, so a single sized `read()` would drop the
  per-byte overhead for large references.

#### [CLEANUP] dead/duplicated code in eds.cpp
- `EDS::calculate_statistics()` is now dead weight — `parse()` computes the same
  stats inline. ~100 lines that can silently diverge from `parse()`; remove or
  repurpose.
- `read_symbol_from_stream()` parses bracket bodies char-by-char with `isspace`
  per char; secondary to the items above but rides the same fix.

#### [CONCURRENCY] [MISSES TARGET] LINEAR-mode `--threads` is effectively serial
- **Location:** `compute_merge_metadata()` in
  `src/cpp/lib/transforms/eds_transforms.cpp` (the `#pragma omp parallel for`).
- **Why it misses its target:** the parallel region exists to speed up LINEAR
  merges, but every worker calls `eds.read_source()` under the single
  `Sources::io_mutex_`, so adding threads buys almost nothing — the `--threads`
  knob the optimization was added for does not actually scale LINEAR mode.
- **Problem:** the mutex is taken on *every* call (hit and miss). With sources
  enabled (the only mode that uses LINEAR), all threads serialize on that one
  mutex. This is why CLAUDE.md observes linear ≈ cartesian throughput despite
  "parallel" processing. (Related: benchmark item #5 below.)
- **Idea:** preload each pair's source sets outside the parallel region, or shard
  the LRU cache / use a reader-friendly structure so workers don't contend on one
  lock. Pairs with the bitset fast path then run lock-free.

#### [MEM] `all_metadata` accumulates a whole iteration's merge results in RAM
- **Location:** `eds_to_leds_linear()` / `eds_to_leds_cartesian()` merge loop in
  `src/cpp/lib/transforms/eds_transforms.cpp`.
- **Problem:** the `BATCH_SIZE` batching comment claims it "controls parallel
  memory", but every pair's `MergeMetadata` is appended to `all_metadata` and
  retained for the entire iteration before streaming. For CARTESIAN merges each
  entry stores `merged_string_lengths` + `valid_indices` sized `set1×set2`; in a
  dense/exponential region this is the dominant allocation and is **not** bounded
  by `BATCH_SIZE` — batching only caps the transient compute working set.
- **Idea:** stream each batch's results to the output as they are computed instead
  of accumulating all of them, or cap retained metadata and spill.

#### [DISK] ~2× temp footprint plus a full up-front input copy
- **Location:** `eds_to_leds_linear()` / `eds_to_leds_cartesian()`.
- **Problem:** each iteration writes `iter_N.eds` while `iter_(N-1).eds` still
  exists (deleted only afterward) → peak temp ≈ 2× current file size, on top of
  the initial full copy of the input to `input.eds`. For 100 GB inputs that is a
  real provisioning constraint not reflected in the memory-only figures in
  `performance.md`.
- **Action:** at minimum document the temp-disk requirement; ideally delete the
  predecessor before/while writing the successor where the iteration chain allows.

#### [MEM] `read_symbol()` copies the whole symbol by value
- **Location:** `EDS::read_symbol()` in `src/cpp/lib/formats/eds.cpp`.
- **Problem:** returns `StringSet` (a `vector<string>`) by value; in FULL mode
  `return sets_[pos]` is a full copy. Hot loops (final serialization, `print()`,
  `stream_merged_symbols_to_file()`) copy every symbol's strings on each access.
- **Idea:** add a `const StringSet&` overload for in-memory mode; keep the
  by-value path only for METADATA_ONLY where the set is freshly constructed.

---

### Correctness / robustness (code review 2026-06-12)

Lower severity than the Critical section above; worth fixing.

#### [VERIFY] VCF block-boundary variants may duplicate/overlap reference
- **Location:** `parse_vcf_to_eds_streaming()` /
  `generate_eds_from_variants()` in `src/cpp/lib/transforms/vcf_transforms.cpp`.
- **Concern:** carryover only defers variants whose *start* is ≥ `block_end`. A
  variant that starts inside a block but whose REF span extends past `block_end`
  is processed in the current block and advances `current_pos` past the boundary,
  while the next block re-emits reference starting at `block_end`. For a large
  deletion/SV straddling a 10 MB block edge this looks like it could duplicate or
  overlap the reference region.
- **Next step:** add a targeted test with an SV spanning a block boundary
  (small `--block-size` to force it) and confirm the EDS is well-formed.

#### [UB] Data race on `first_exception` in the OpenMP merge loop
- **Location:** `compute_merge_metadata()` parallel region.
- **Problem:** `if (first_exception) continue;` reads the `std::exception_ptr`
  outside any critical section while another thread writes it inside one →
  technically a data race / UB.
- **Fix:** use a `std::atomic<bool>` flag for the early-out check, guarding the
  `exception_ptr` write/read with the existing `#pragma omp critical`.

#### [UB] `std::isspace(ch)` on a raw `char` in bracket parsing
- **Location:** `read_symbol_from_stream()` full-bracket path in
  `src/cpp/lib/formats/eds.cpp`.
- **Problem:** passes a raw `char` (possibly negative) to `std::isspace(int)` →
  UB for bytes ≥ 0x80. The compact path already casts to `unsigned char`; the
  bracket path does not. Harmless for ACGT input, but inconsistent. (Folds into
  the `read_symbol_from_stream` cleanup noted above.)

---

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