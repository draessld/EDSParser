# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

EDSParser is a C++ library for parsing and transforming Elastic-Degenerate Strings (EDS), a data structure for representing sequence variation in bioinformatics. The library supports:
- **Multiple input formats**: MSA (Multiple Sequence Alignment), VCF (Variant Call Format), and native EDS
- **Format transformations**: Convert between formats and produce length-constrained EDS (l-EDS)
- **Memory-efficient streaming**: File-based loading keeps only metadata in RAM; strings read on demand via `read_symbol()`
- **Source tracking**: Maintains provenance information through transformations (via .seds files)

## Build and Test Commands

### Initial Setup
```bash
# Install and build everything
./INSTALL.sh

# This will:
# - Check dependencies (cmake, g++, make, Boost, SDSL)
# - Build C++ library and tools in build/ directory
# - Run tests automatically
# - Install to ~/.local/ (library, headers, CMake config, tools)
```

### Development Workflow
```bash
# Clean build from scratch
rm -rf build && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local ..
make -j$(nproc)

# Run all tests
cd build/src/cpp && ctest --output-on-failure

# Run specific test
cd build/tools && ./test_eds

# Available tests (auto-run by ctest): test_eds, test_sources, test_stats, test_merge, test_msa, test_vcf, test_integration

# Manual memory stress tests (too slow for CI, run manually):
cd build/tools && ./test_memory_stress

# Install after build
cd build && make install

# Uninstall
./UNINSTALL.sh
```

### Development Build Types
```bash
# Debug build (with symbols, no optimization)
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Release build (optimized, default)
cmake -DCMAKE_BUILD_TYPE=Release ..
```

## Architecture

### Core Components

**EDS Class** ([src/cpp/lib/formats/eds.hpp](src/cpp/lib/formats/eds.hpp), [src/cpp/lib/formats/eds.cpp](src/cpp/lib/formats/eds.cpp))
- Central data structure representing an elastic-degenerate string
- Format: Sequence of sets where each position contains alternative strings: `{ACGT}{A,ACA}{CGT}{T,TG}`
- Two construction paths (mode is implicit, not a user-facing enum):
  - **Stream/string constructors** (`EDS(istream&)`, `EDS(string&)`): all strings loaded into `sets_` in RAM
  - **File loader** (`EDS::load(path)`): only metadata in RAM; strings streamed on demand from the file via `read_symbol(pos)` — used throughout the transform pipeline for memory efficiency
- Supports source tracking via separate .seds files (managed by `Sources` class)
- Key operations:
  - Loading/parsing from streams or files
  - Metadata access via `get_metadata()` (`Metadata` struct: `cum_common_positions`, `cum_degenerate_counts`, `is_degenerate`, `string_lengths`, etc.)
  - Position checking (verify if pattern occurs at position)
  - Merging adjacent symbols (CARTESIAN without sources, LINEAR with sources)
  - Pattern generation for benchmarking
- **Removed API** (deleted — do not use): `get_statistics()`, `print_statistics()`, `get_sets()`, `get_is_degenerate()`, `set_source_cache_capacity()`, `clear_source_cache()`
  - Use `get_metadata()` for all statistics/structural fields
  - Use `get_metadata().is_degenerate` instead of `get_is_degenerate()`
  - Use `get_sources_object()->set_cache_capacity()` for cache control
- Source API: `set_sources_object()`, `get_sources_object()`, `read_source(idx)`, `has_sources()`
- **Note**: `get_sources()` throws when EDS was constructed from a stream; use `read_source(idx)` instead

**Transform Modules**
- **MSA → EDS/l-EDS** ([src/cpp/lib/transforms/msa_transforms.hpp](src/cpp/lib/transforms/msa_transforms.hpp), [src/cpp/lib/transforms/msa_transforms.cpp](src/cpp/lib/transforms/msa_transforms.cpp)): Parse FASTA alignments with gaps (`-`) into EDS with source tracking. Uses streaming approach with per-symbol processing for memory efficiency:
  - Reference sequence and bit vectors in memory (O(alignment length))
  - Sequence data read on-demand via file seeking
  - Per-symbol variant collection (cleared after each symbol)
  - Direct streaming output to files (no output accumulation in memory)
  - Memory footprint: O(reference + bit vectors), independent of output size
- **VCF → EDS/l-EDS** ([src/cpp/lib/transforms/vcf_transforms.hpp](src/cpp/lib/transforms/vcf_transforms.hpp), [src/cpp/lib/transforms/vcf_transforms.cpp](src/cpp/lib/transforms/vcf_transforms.cpp)): Parse VCF + FASTA reference into EDS. Handles:
  - SNPs and small indels
  - Simple deletions (`<DEL>`) and insertions (`<INS>`)
  - **Inversions (`<INV>`)** - reverse complement of reference
  - **Copy Number Variations (`<CN0>`, `<CN1>`, `<CN2>`, etc.)** - deletions and duplications
  - Multi-allelic sites (multiple ALT alleles)
  - Sample-level source tracking (one path per sample)
  - **Block-based processing** for memory-efficient handling of large VCF files (default: 10M bases per block)
  - Reference FASTA uses random-access streaming (minimal memory footprint)
  - Returns `VCFStats` with counts: `total_variants`, `processed_variants`, `skipped_malformed`, `skipped_unsupported_sv`, `variant_groups`
- **EDS → l-EDS** ([src/cpp/lib/transforms/eds_transforms.hpp](src/cpp/lib/transforms/eds_transforms.hpp), [src/cpp/lib/transforms/eds_transforms.cpp](src/cpp/lib/transforms/eds_transforms.cpp)): Length-constrained merging to ensure minimum context length. Two strategies:
  - LINEAR: Phasing-aware merging using source information (preserves valid combinations)
  - CARTESIAN: All combinations (no source info required)
  - **Chain-Merging Selection** (2026-06-28): `select_merge_groups()` replaces the old pairwise selection. Detects maximal contiguous chains where every adjacent pair needs merging and emits each chain as one `MergeGroup(start, count, reason)`. A chain of L positions is resolved in one full-file pass instead of L−1 passes. Typical convergence: **1 iteration** for real genomic data (was 3-4 with old pairwise approach; 2.1-2.4× throughput improvement measured).
  - **Contiguous-context measurement** (2026-07-10): `needs_merge()` judges "short context" against the length of the whole contiguous run of common symbols a position belongs to (measured by `CtxRunCursor` during `select_merge_groups()`'s left-to-right walk; it was an n-entry `ctx_run_len` vector until 2026-07-30), not a single common symbol's length. VCF/MSA-derived EDS fragment one deterministic context into several adjacent common symbols; measuring a single short fragment made the greedy chain bridge across an otherwise long-enough context and over-merge. This also restores the **idempotence/monotonicity invariant** `T_B(EDS) == T_B(T_A(EDS))` for `A < B` (building l=B from an existing l=A l-EDS is byte-identical — `.leds` and `.seds` — to building l=B from the raw EDS), which the incremental experiment runner (`experiments/run_leds_serial.sh`, `INCREMENTAL=1`) relies on. Guarded by `tests/e2e/test_leds_incremental.sh`.
  - **Memory-Stable Architecture** for 100GB+ files:
    - Uses METADATA_ONLY mode (only metadata in RAM, ~100MB for 100GB file)
    - Iterative temp file chaining (iteration N → temp file → iteration N+1)
    - **Per-process unique temp directory** (`/tmp/edsparser_leds_<pid>/`) — prevents conflicts when multiple `eds2leds` instances run in parallel (e.g. from experiment scripts)
    - Incremental batch streaming (2026-07-05): a stateful `MergeStreamWriter` consumes each `BATCH_SIZE` window's `MergeMetadata` via `add_batch()` and writes it out immediately, then `finish()` drains the trailing unmodified tail. Retained `MergeMetadata` is capped to one batch (no per-iteration `all_metadata` accumulator), while still writing each unmodified symbol only once. Byte-identical output vs the old accumulate-then-stream path; ~23% lower peak RSS on cartesian runs with tens of thousands of moderate-size groups.
    - Streaming output with immediate flushing (no ostringstream accumulation)
    - Memory footprint: O(metadata + batch) instead of O(file_size × iterations × threads)
    - Typical reduction: 2TB → 500MB peak memory for 100GB EDS with 1000 groups and 16 threads
    - **Index slimming** (2026-07-30): peak RSS is *linear in input symbols*, and was ~139 B/symbol. Three changes brought it to ~74 B/symbol (byte-identical output; measured 322→162 MB at 1.98M symbols, 4404→2380 MB at 31.7M symbols): (1) `base_positions` in both `EDS::Metadata` and `Sources` is `uint64_t`, not `std::streampos` — streampos carries an `mbstate_t` and is 16 bytes, doubling the largest per-symbol and per-string arrays; (2) `cum_common_positions`/`cum_degenerate_counts` are **lazy** — pure prefix sums used only by the position-lookup helpers, materialised by `EDS::ensure_position_index()` on first lookup, so the merge (which holds an input *and* an output metadata at once) no longer pays 12 B/symbol twice; (3) the merge writer `reserve()`s its per-symbol arrays to the exact output symbol count (known up front: each group of `count` positions collapses to one symbol) and `EDS` parsing `shrink_to_fit()`s its index arrays, whose push_back slack would otherwise stay resident for the object's whole life.
    - **`--block-size` block mode** (2026-07-30) — the actual memory *ceiling*: `eds_to_leds_blocked()` cuts the input at **barriers** and merges one block at a time with the unchanged merge core. A barrier is a maximal run of common symbols whose total length ≥ `context_length`: `needs_merge()` measures short context over the whole run (see contiguous-context measurement above), so such a run is never short, no neighbour can absorb it, and the only merging inside it is common-common concatenation which keeps it ≥ l. It therefore splits the merge into independent halves *in every iteration*. Cuts are taken at the **last** symbol of such a run (the next symbol is degenerate by construction), that one symbol is duplicated into the following block, and the duplicate is dropped when concatenating — so the output is **byte-identical to a whole-file run** (`.leds` and `.seds`), which is what `tests/e2e/test_eds2leds.sh` asserts for linear and cartesian at several l values. Measured on a 949 MB / 31.7M-symbol input, `-l 10`: whole-file 2380 MB / 54 s; `--block-size 200M` 1071 MB / 140 s; `50M` 644 MB / 145 s; `10M` **540 MB linear, 80 MB cartesian** / 145 s. Falls back to whole-file with a warning when no barrier exists at a block boundary (variation too dense, or l larger than every context). Any source format works — block slices are materialised through format-agnostic `read_source()`.
    - **The linear-mode floor is the input sources index, not the blocks** (2026-07-30): cartesian block mode is a true ceiling (80 MB on a 949 MB input, independent of file size), but linear mode still loads one `Sources` index for the whole file — 8 bytes per string, i.e. 509 MB for the 63.7M strings of that input, which is exactly the 540 MB observed. Blocks smaller than ~50 MB stop helping because of it. Fixing it means slicing sources without a whole-file index (streaming slice, or a sampled index at 0.5 B/string). See TODO.md.
    - **Remaining O(n) structures** without `--block-size` (why whole-file mode is not constant memory): input metadata (~23 B/symbol), output metadata built inline (~19 B/symbol), and the `Sources` byte-offset index (8 B/string). All are per-symbol/per-string arrays required by the random-access `read_symbol(pos)` design. True O(1) needs a sequential symbol reader — every phase (`select_merge_groups`, `compute_merge_metadata`, the writer) already walks positions strictly left-to-right, so the index exists only because the API is random-access. See TODO.md.

**Command-Line Tools** ([src/cpp/tools/](src/cpp/tools/))

Transformation tools (each focused on a specific conversion):
- `eds2leds`: Transform EDS to l-EDS with linear or cartesian merging
  - **`--full` flag**: Force full bracket format output (default: compact)
  - **Source flags**: `-s/--seds <path>` (SEDS or EDZ, auto-detected by extension/content) or `-z/--edz <path>` (forces EDZ interpretation regardless of extension — mutually exclusive with `-s`). Either enables LINEAR merging; omitting both uses CARTESIAN.
  - Runs complexity estimation before transformation and warns on exponential-growth risk
  - **`--max-memory <size>` pre-flight guard** (2026-07-11; threshold corrected 2026-07-30): before any merging, predicts peak RAM from metadata only and refuses the transform with **exit code 3** if it would exceed `<size>` (accepts `450G`, `512M`, `2T`, or bare bytes; binary units). The **merge term** is `Σ_group merged_size × (group_count·8 + ~48 B)` over the largest `BATCH_SIZE`(=1000)-group window (matching how the transform batches metadata), but the guard compares the **full predicted peak**: merge term + index arrays (2× per-symbol metadata + the 8 B/string sources index) + 15% headroom + 64 MB. Until 2026-07-30 it compared the merge term alone, which is ~295 KiB for a linear run that actually peaks at 2.3 GB — so the guard was effectively unfirable in linear mode and only ever caught cartesian blow-ups. `merged_size` is the CARTESIAN product of the group's symbol cardinalities, capped at `num_paths` when sources are given. **That cap is WRONG on VCF-derived data and the estimate is unsafe for admission control until it is fixed** (measured 2026-08-01, chr7 l=5: predicted 0.64 GiB, actual peak 28.3 GiB — 45× under). The cap assumed a linear merge cannot outnumber the paths, but `vcf2eds` sources are *sample-level, not haplotype-level* (see TODO 9c): a heterozygous sample is marked present in both the reference and the alt string at a site, so source intersections rarely go empty and a chain of k adjacent degenerate sites can survive up to 2^k combinations — unbounded by `num_paths`. chr7 iteration 0 has 218,016 adjacent-degenerate groups and expands 20M input strings to 55.5M. Without the cap the same input estimates ~TB (the raw cartesian bound), so neither bound is usable as-is; see TODO.md for the fix. Exit 3 is distinct from ordinary failures so orchestrators can mark the input "too intensive" and skip it (`experiments/run_leds_serial.sh`, and since 2026-07-30 the parallel workers `run_chrom_leds.sh` / `run_chrom_leds_incremental.sh` via `MAX_MEMORY`). Covered by `tests/e2e/test_eds2leds.sh`.
  - **Complement source sets were read as universal in the merge** (fixed 2026-08-04, 20d8ff1): a `PathSet` starting with 0 means either universal (`{0}`) or *complement* (`{0,e1,e2}` = all paths except e1,e2), and the bitset fast path in `compute_merge_metadata()` collapsed both to `~0ULL`. Complement sets then intersected with everything, the iterative fold never pruned, and it kept combinations no path carries — an l-EDS containing strings no genome carries, produced at combinatorial cost. `vcf2eds` and `msa2eds` both encode the reference allele as a complement, and the fast path engages only when all path IDs fit in [1,63], so **every dataset of at most 63 samples/sequences was affected and every larger one was not** (above 63 the guard already fell back to the correct `PathSet` code). Measured on a 50-path haploid VCF at l=5, where ≤ 50 strings per symbol is provable: 6,557,335 strings / 3159 MB / 21.2 s → 2,491 strings / max 50 per symbol / 5.9 MB / 0.01 s; a 40-sequence 3 kb MSA at l=20 went from killed at a 6 GB cap to 9.5 MB. `to_bits()` now expands complements against a universe masked to `num_paths`, and `use_bits` requires `num_paths ≤ 63`. **Consequence for existing results:** anything produced at ≤ 63 paths before this commit measures the bug — see TODO.md.
  - **Path count and heterozygosity drive linear-merge memory** — magnitude not yet measured. The 2026-08-02 yeast 1011-vs-50 comparison is void: the two runs took different code paths across the 63-path threshold (see the complement fix above), so the "20x fewer paths bought 500x memory" figure compared a correct run against a buggy one. What survives: 1000G chr7 (2504 samples, correct path) genuinely peaks at 28.3 GiB at l=5, and on synthetic 50-path clustered data a heterozygous diploid VCF is still killed at 8 GB at every l while its haploidised twin runs l=5..30 under 19 MB. Cause is the sample-level path model (TODO 9c). `experiments/scripts/make_sample_subset.sh` + `run_subset_dataset.sh` build seeded, nested sample subsets and run them under a kernel-enforced `MemoryMax` scope; see TODO.md for the path-count experiment, which must be re-run on the fixed binary.
  - **`--estimate-memory`** (2026-07-30): prints the same prediction as machine-readable `KEY=BYTES` lines and exits 0 without doing any work — `MERGE_PEAK_BYTES`, `EDS_INDEX_BYTES`, `SOURCES_INDEX_BYTES`, `MERGE_GROUPS_BYTES`, `RECOMMENDED_BUDGET_BYTES` (the one to schedule on), `MERGE_GROUPS`, `PATH_CAP`, `SATURATED`, `BLOCK_SIZE_BYTES`. Honours `--block-size` by scaling the index terms to one block's share of the input. Calibrated to over-predict, since schedulers admit work on it: predicts 2.31 GiB for a run that peaked at 2.27 GiB, 778 MiB for one that peaked at 629 MiB, 216 MiB for one that peaked at 158 MiB. Used by `experiments/run_leds.sh` for memory-aware admission control (see below).
  - **Linear vs cartesian throughput** (`--size standard`, 1-10 MB, post raw-copy fix 2026-07-03): on real violation data (`--min-context 0`, 1% var, l=5) cartesian runs at ~25 MB/s vs linear ~21 MB/s and both converge in 1 iteration after the chain-merging fix (2026-06-28). At 5% variability: cartesian ~8.7 MB/s, linear ~5.7 MB/s. Remaining gap is inherent: linear writes an extra SEDS temp file per iteration; cartesian writes no SEDS data. The **EDS raw-copy pass-through** (2026-07-03) sped both up further on larger inputs — measured **cartesian 1.9–2.2×, linear 1.16–1.63×** on 20 MB inputs vs the pre-change serialiser (cartesian gains more because it has no SEDS I/O, so the EDS pass-through dominates its runtime).
  - **`--source-format <fmt>` for the output sources** (2026-07-30): `seds` (default, dense text), `seds-sparse`, `edz`, `edz-sparse`, `edz-compressed`. The merge pipeline itself still only *writes* dense text SEDS (it streams entries as it merges and has no binary writer); any other format is produced by re-encoding that file once at the end via `Sources::save_as()`, then deleting the text file. EDZ variants are written as `<output>.edz` (all EDZ variants share that extension and are self-describing via header flags). Measured on VCF-derived sources: at 500 paths SEDS 9.4 MiB → edz 8.7 MiB → edz-sparse 4.5 MiB → **edz-compressed 2.0 MiB (4.7×)**, which also beats `gzip -6` of the text SEDS (2.9 MiB); at 2504 paths (1000G-like) SEDS 16.3 MiB → edz 13.2 MiB → edz-sparse 7.8 MiB → **edz-compressed 2.7 MiB (5.9×)**. **The deciding factor is the allele-frequency spectrum, not the path count** (corrected 2026-08-06): a bitset costs ⌈paths/8⌉ bytes per entry regardless of contents, so it wins only when entries are dense. On the rare-variant M. tuberculosis panels the ranking inverts and worsens with scale — 100 paths SEDS 464 KB vs edz-sparse 529 KB; 500 paths 2.98 MB vs 8.36 MB; 1141 paths 7.83 MB vs **34.07 MB (4.4× larger than the text it replaces)**, because a variant carried by 3 of 1141 isolates is 143 bytes as a bitset and ~12 as `{5,88,900}`. `gzip -6` of the text SEDS beats every built-in format there by ~8× (1.02 MB at 1141 paths). `edz-compressed` has not been measured on rare-variant data. `edz-compressed` requires a zstd-enabled build and is validated up front, before the merge runs. Because the re-encode happens at the end, peak *disk* still includes the dense text SEDS — only the final artifact shrinks.
- `msa2eds`: Transform MSA to EDS/l-EDS with source tracking
  - `-s/--seds <path>`: output source file (always dense text SEDS — no `-z`/EDZ support in this tool yet)
- `vcf2eds`: Transform VCF to EDS/l-EDS with sample-level source tracking (requires reference FASTA)
  - **`--block-size` parameter**: Control memory usage for large VCF files (default: 10M bases)
  - Smaller block sizes (e.g., 1M) reduce memory at the cost of more I/O
  - Larger block sizes (e.g., 100M) increase performance but use more memory
  - Set to 0 for legacy mode (loads all variants, high memory)
  - **Source format**: `-s/--seds <path>` (default) writes sparse text SEDS (universal `{0}` entries omitted); `-z/--edz` writes sparse binary EDZ instead. **Exception**: in l-EDS mode (`-l`), sources are always dense text SEDS regardless of `-z` — the two-stage EDS→l-EDS pipeline doesn't support sparse/EDZ output, and `-z` combined with `-l` prints a warning and falls back rather than corrupting output.
  - **`--keep-eds` flag**: with `-l`, also writes the intermediate stage-1 VCF→EDS output (which is otherwise discarded in a temp dir) to `<input_base>.eds` / `<input_base>.seds` next to the l-EDS output. The kept EDS is byte-identical to a plain no-`-l` run; the kept SEDS is always dense text (same limitation as the l-EDS output SEDS). No-op (with a warning) without `-l`.

Utility tools:
- `edsparser-source-transform`: Convert an existing source file between formats (SEDS ↔ EDZ) without re-running the EDS transform
  - `-i <in>` / `-o <out>`; input format auto-detected (override with `--from`), output format inferred from the output extension (`.edz`→EDZ, else SEDS; override with `--to`)
  - `--sparse` selects the sparse variant of the output format (omits universal `{0}` entries); `--verify` reloads input and output and confirms every source set matches
  - Thin wrapper around `Sources::save_as(path, format)` — reads each entry via format-agnostic `read_source()` and re-encodes. `--verify` expands complement/universal encodings to their explicit member list (`canonicalize_expanded()`) using the now-reliable `num_paths` before comparing, so SEDS complement form (`{0,4}`) and EDZ explicit form (`{1,2,3,5}`) of the same set compare equal. (Before 2026-07-06 it only collapsed the universal spelling and reported false mismatches on any complement entry.)
  - `--compress`/`-c`: select the zstd-compressed EDZ variant (equivalent to `--to edz_compressed`); mutually exclusive with `--sparse`, and only valid for EDZ output. Requires a zstd-enabled build. All EDZ variants share the `.edz` extension (distinguished by header flags), so compression is requested via the flag rather than a distinct extension; auto-detect resolves it back from the flags on load. Without zstd the conversion throws a clear "built without zstd" error rather than writing a corrupt file (`Sources::edz_compressed_available()` reports build support).
- `edsparser-stats`: Display EDS statistics, memory estimates, l-EDS compliance
  - `-s/--seds <path>` or `-z/--edz <path>` (mutually exclusive) for source-aware stats, same semantics as `eds2leds`
  - Path-count stats (`num_paths`, "paths per string") correctly expand `PathSet` complement encoding (`{0,e1,e2,...}` = all paths except e1,e2,...) against the true path universe — fixed 2026-07-02; previously undercounted for complement-heavy SEDS files
- `edsparser-genpatterns`: Generate random patterns for benchmarking
  - **`-s/--seds` / `-z/--edz` source-aware generation** (2026-08-13): with sources attached, each pattern walks **one randomly chosen path** — at every symbol it takes an alternative whose source set contains that path — so the pattern is a substring of a genome the panel actually contains. Without sources, alternatives are picked independently per symbol, which samples the *cartesian* language. Those patterns are absent from any LINEAR-merged l-EDS, because the merge prunes exactly the combinations no path carries, so a `locate()` benchmark built on them measures how invalid the pattern set is rather than the index. Measured on the 294-sequence SARS-CoV-2 panel at `|P|=120`: the cartesian set matches **158/200** patterns, the source-aware set **200/200** at every l ∈ {9,19,59}. The tool warns when run without sources; `--ignore-sources` restores the old behaviour explicitly.
  - **`--seed`** (2026-08-13): makes the pattern set reproducible. Previously the generator seeded from `std::random_device` under a comment claiming reproducibility, so every invocation produced a different set and no two benchmark runs compared the same queries.
  - **Deduplication** (2026-08-25): patterns are distinct by default — a repeat is the same query timed twice, not a second measurement, and on a small or low-diversity panel repeats are common (the 294-genome SARS-CoV-2 set produced 198 distinct patterns from a request for 200). A duplicate is discarded and retried exactly like a walk that ran off the end, so a request for more distinct patterns than the EDS holds reports a shortfall rather than padding the set. `--allow-duplicates` restores the old behaviour, which is what regenerating a pre-2026-08-25 pattern set requires: rejecting a duplicate changes how much randomness is consumed, so **the same seed produces a different set** under the default.
  - `generate_patterns()` returns `PatternGenStats` (`requested`, `generated`, `source_aware`, `num_paths`, `duplicates_discarded`, `unique`). It no longer pads a short walk by wrapping around to symbol 0 — that spliced together sequence which is not contiguous in any genome. It retries from a fresh path and start instead (64 attempts), and reports any shortfall.
- `genrandomeds`: Generate synthetic EDS files with controlled variability for testing/benchmarking
  - Requires `--ref-size-mb` (size) and `-o` (output); key options: `-v` (variability), `--min-context`, `--seed`
  - Automatically generates paired `.seds` source file using **round-robin haplotype assignment**: each path is assigned to exactly one alternative per degenerate symbol, matching real phased data. This prevents Cartesian product explosion during linear merge.
  - **O(1) memory**: streams EDS and SEDS directly to disk symbol-by-symbol; peak RSS ~4 MB constant regardless of output size. Uses two independent PRNGs (`ref_gen(seed)` / `var_gen(seed^0x9e3779b9)`) — same `--seed` is reproducible but produces different output than pre-2026-05 builds.
  - `min_context==0`: per-position Bernoulli trials (expected variant count = `total_bp × variability`); `min_context>0`: one variant per segment, processed iteratively.
  - Installed to `~/.local/bin/` by `./INSTALL.sh`; also available at `build/tools/genrandomeds`

**Performance Output**: All tools write runtime and peak memory to stderr on completion:
```
[Performance] Runtime: X.XXs | Peak Memory: XXX.X MB
```

### File Formats

- `.msa`: Multiple Sequence Alignment in FASTA format (with gaps as `-`)
- `.vcf`: Variant Call Format
- `.eds`: Elastic-Degenerate String: `{str1,str2,...}{str3}{...}`
- `.seds`: Sources file (text entries mapping string IDs to path IDs; range+complement encoding, sparse by default from `vcf2eds`). Ends with a self-describing binary trailer that records the path universe size (`num_paths`) so complement entries (`{0,e1,...}` = all paths except e1,...) expand exactly on load: sparse = `bitvec | "SED2"(4) | cardinality(8) | m_degen(8) | num_paths(8)`; dense = `text | "SEDN"(4) | cardinality(8) | num_paths(8)`. Legacy trailerless files (and the old 20-byte `"SEDS"` sparse trailer) still load — `parse_seds()` falls back to inferring `num_paths` from the largest path-ID token, which is exact except in the rare degenerate case where the true max path appears in every entry but never explicitly.
- `.edz`: Sources file (binary bitset format; self-describing via magic bytes `"EDZ\0"` + flags; auto-detected by `.edz` extension or forced via `-z`/`--edz`). Flags select the variant: dense (`0x0002`), sparse (`0x0006`), or zstd-compressed (`0x0003`). EDZ_COMPRESSED splits the dense bitset data into ~256 KiB blocks, zstd-compresses each, and stores a per-block index (compressed offset/size + uncompressed size) for O(1) block lookup; reads decompress one block on demand and cache it. Requires a zstd-enabled build.
- `.leds`: Length-constrained EDS (minimum context length guaranteed)
- `.peds`: Phased EDS (combined .eds + .seds, planned but not implemented)

### Key Design Patterns

**Streaming Architecture**: Transform functions accept `std::istream&` and `std::ostream&` rather than file paths. This enables:
- Memory-efficient processing of large files
- Easy unit testing with `std::stringstream`
- Flexible I/O (files, pipes, memory buffers)

**Two-Phase Loading**: EDS class uses metadata-first approach:
1. Parse file/stream to build index and statistics
2. Stream constructors also populate `sets_` (in-memory); file loader (`EDS::load`) keeps the file handle open for on-demand symbol reading via `read_symbol()`

**Sources as Separate Class** ([src/cpp/lib/formats/sources.hpp](src/cpp/lib/formats/sources.hpp), [src/cpp/lib/formats/sources.cpp](src/cpp/lib/formats/sources.cpp)): Source tracking (provenance) is managed by a dedicated `Sources` class, separate from EDS. This allows:
- Using EDS without sources (simpler, faster)
- Loading sources on-demand with LRU cache (default: 10K entries)
- Different merging strategies (LINEAR requires sources, CARTESIAN doesn't)
- Format extensibility: `SEDS`/`SEDS_SPARSE` (text, implemented), `EDZ`/`EDZ_SPARSE` (binary bitset, implemented), `EDZ_COMPRESSED` (zstd block compression, implemented — requires a zstd-enabled build)

Key methods:
- `Sources::load()` — factory method, returns `shared_ptr<Sources>`
- `Sources::read_source(idx)` — access with automatic LRU caching; **returns by value** (thread-safe copy)
- `Sources::read_source_ref(idx)` — returns a const reference into the LRU cache for single-threaded use only; **do not use in parallel contexts** — another thread can evict the cache entry, dangling the reference
- `Sources::merge_adjacent_sources()` / `intersect_sources()` — set operations for l-EDS; uses `read_source()` (not `read_source_ref()`) to avoid dangling references across OpenMP threads
- `EDS::set_sources_object()` / `get_sources_object()` — attach/retrieve Sources on EDS
- `EDS::read_source(idx)` — delegates to Sources (works regardless of how the EDS was constructed)

**Thread safety**: `Sources` has a `mutable std::mutex io_mutex_` protecting the shared `stream_` and LRU cache from concurrent access. Both `read_source()` and `read_source_ref()` acquire this mutex. However, `read_source_ref()` releases the mutex before returning — the reference is only safe while no other thread calls any `Sources` method. In practice: always use `read_source()` in parallel code paths.

**Note**: `EDS::get_sources()` throws when the EDS was constructed from a stream (sources not loaded). Use `read_source(idx)` instead.

**Cardinality validation**: `EDS::load(eds_path, seds_path)` validates that `Sources::cardinality()` matches `EDS::m_` at load time; throws `std::invalid_argument` on mismatch. This catches stale or mismatched `.seds` files early.

**Pipe Streaming** ([src/cpp/lib/pipe_buffer.hpp](src/cpp/lib/pipe_buffer.hpp), [src/cpp/lib/pipe_stream.hpp](src/cpp/lib/pipe_stream.hpp)): Thread-safe circular buffer implementing `std::streambuf` to connect producer/consumer threads without intermediate temp files:
- `PipeOutputStream` / `PipeInputStream` — stream wrappers for producer and consumer threads
- `make_pipe()` — factory to create connected stream pairs
- Default buffer: 64MB; uses mutex + condition variables for synchronization
- Used in the VCF→EDS→l-EDS direct pipeline to eliminate temp files

**Complexity Estimation** ([src/cpp/lib/transforms/eds_complexity.cpp](src/cpp/lib/transforms/eds_complexity.cpp)): `estimate_leds_complexity()` analyzes EDS structure before transformation:
- Detects: adjacent degenerate pairs, short context blocks, dense degenerate clusters
- Returns `TransformComplexity` with `warn_slow`, `warn_exponential` flags and `recommendation` string
- Risk categories: FAST, SLOW, EXPONENTIAL_GROWTH_RISK
- Automatically called by `eds2leds` — warns users before committing to expensive transforms

**Memory Monitoring** ([src/cpp/lib/memory_monitor.hpp](src/cpp/lib/memory_monitor.hpp), [src/cpp/lib/memory_monitor.cpp](src/cpp/lib/memory_monitor.cpp)): `MemoryMonitor` class for diagnostics and test validation:
- Periodic sampling thread; methods: `start()`, `stop()`, `add_label()`, `get_peak_memory_mb()`, `detect_memory_leak()`
- Linear regression-based leak detection (threshold: 1.0 MB/sec default)
- Helpers: `assert_memory_below()`, `assert_no_memory_growth()` for test assertions
- Used by `test_memory_smoke` and `test_memory_stress`

**Block-Based VCF Processing with Streaming Output**: For handling very large VCF files (e.g., 65GB+), the VCF parser uses genomic windowing with incremental file writing:
- Divides reference genome into blocks (default: 10M bases)
- Processes variants in each block sequentially
- **Writes output directly to disk per block** (prevents output accumulation in memory)
- Maintains carryover queue for variants read from VCF stream but belonging to next block
- Memory footprint: O(variants per block), not O(total variants)
- Typical reduction: 120GB → 6-8GB for large population VCF files
- Reference genome uses random-access reading (not loaded into memory)
- Output files flushed after each block to prevent memory growth
- For l-EDS: Uses temporary files for two-stage pipeline (VCF→EDS→l-EDS) to avoid accumulating full EDS in memory

## Memory Optimization Guidelines

### MSA Processing for Large Files

**Memory Architecture**: MSA transformation is already memory-efficient with streaming I/O:

**Input Processing**:
- Reference sequence: O(N) where N = alignment length (kept in RAM)
- Bit vectors: O(N) - two compact bit vectors for variant tracking and symbol boundaries
- File position index: O(K) where K = number of sequences (8 bytes per sequence)
- **Per-symbol processing**: Variants collected only for current symbol, then immediately flushed

**Output Processing**:
- Writes directly to file streams (no accumulation in memory)
- Each symbol flushed immediately after generation
- Memory footprint: O(1) relative to output size

**Memory Estimate**:
```
Total Memory = ref_seq_size + (2 × bit_vectors) + file_positions + per_symbol_variants
             ≈ N + (N/4) + (K × 8) + variants_per_symbol

Example: 1000 sequences × 100MB alignment
  = 100MB + 25MB + 8KB + <symbol variants>
  ≈ 125MB baseline (excellent!)
```

**When MSA Works Well**:
- Any realistic MSA size (up to 10K+ sequences, 1GB+ alignment)
- High-diversity regions (each symbol processed independently)
- Limited RAM environments (output doesn't accumulate)

**Implementation Details**:
- **Three-pass algorithm** ([msa_transforms.cpp:36-374](src/cpp/lib/transforms/msa_transforms.cpp#L36-L374)):
  1. **Pass 1** ([msa_transforms.cpp:36-90](src/cpp/lib/transforms/msa_transforms.cpp#L36-L90)): Parse MSA, store reference + file positions, build variant bit vector
  2. **Pass 2** ([msa_transforms.cpp:101-190](src/cpp/lib/transforms/msa_transforms.cpp#L101-L190)): Determine symbol boundaries (EDS: every transition; l-EDS: merge based on context length)
  3. **Pass 3** ([msa_transforms.cpp:201-327](src/cpp/lib/transforms/msa_transforms.cpp#L201-L327)): Generate output symbol-by-symbol with streaming writes
- **Streaming output** ([msa_transforms.cpp:323-325](src/cpp/lib/transforms/msa_transforms.cpp#L323-L325)): Output flushed after each symbol to prevent memory growth
- **File seeking** ([msa_transforms.cpp:276-286](src/cpp/lib/transforms/msa_transforms.cpp#L276-L286)): Reads sequence data on-demand for each symbol, avoiding loading all sequences into memory

### VCF Processing for Large Files

**Problem**: Large population VCF files (e.g., 1000 Genomes, UK Biobank) can contain hundreds of millions of variants, requiring 100GB+ RAM with naive loading.

**Solution**: Adjust `--block-size` based on available memory:

```bash
# Server with 16GB RAM: Use smaller blocks
vcf2eds -i large.vcf -r reference.fa --block-size 1000000  # 1M bases

# Server with 64GB RAM: Default is optimal
vcf2eds -i large.vcf -r reference.fa  # 10M bases (default)

# Server with 256GB RAM: Use larger blocks for speed
vcf2eds -i large.vcf -r reference.fa --block-size 100000000  # 100M bases
```

**Trade-offs**:
- Smaller blocks: Lower peak memory, more I/O operations, slightly more EDS symbols at block boundaries
- Larger blocks: Higher peak memory, fewer I/O operations, optimal EDS structure

**Implementation Details**:
- **Core streaming function** ([vcf_transforms.cpp:735-855](src/cpp/lib/transforms/vcf_transforms.cpp#L735-L855)): `parse_vcf_to_eds_streaming()`
  - VCF stream is read line-by-line (cannot seek backward)
  - Variants assigned to blocks based on start position
  - Carryover mechanism handles variants that span block boundaries
  - Each block processes independently: read → sort → generate EDS → flush → clear → next block
  - Automatic block size adjustment: if reference < block_size, processes as single block
- **l-EDS streaming function** ([vcf_transforms.cpp:894-951](src/cpp/lib/transforms/vcf_transforms.cpp#L894-L951)): `parse_vcf_to_leds_streaming_direct()`
  - Uses temporary files for two-stage pipeline: VCF→EDS (temp) → l-EDS (output)
  - Prevents accumulating full EDS string in memory
  - Temp files automatically cleaned up on completion or error
  - Location: `std::filesystem::temp_directory_path()`
- **Memory optimizations**:
  - `read_fasta_region()` ([vcf_transforms.cpp:98-131](src/cpp/lib/transforms/vcf_transforms.cpp#L98-L131)): Pre-allocated strings with `reserve()` instead of `ostringstream` (eliminates repeated reallocations)
  - `generate_eds_from_variants()` ([vcf_transforms.cpp:621-726](src/cpp/lib/transforms/vcf_transforms.cpp#L621-L726)): Returns group count to avoid duplicate `group_overlapping_variants()` calls
  - Output flushed after each block to prevent buffering
  - All unused variables and constants removed (warning-free compilation)

### EDS→l-EDS Processing for Large Files

**Memory Architecture**: EDS→l-EDS transformation uses a memory-stable streaming architecture for 100GB+ files:

**Problem with Previous Implementation**:
- Full EDS loaded into RAM (100GB file → 100GB+ RAM)
- Each parallel merge created full EDS copy (16 threads × 100GB = 1.6TB peak)
- ostringstream accumulation during reconstruction (4× overhead: buffers + copy + parse)
- Total: ~2TB RAM for 100GB input with 1000 pairs and 16 threads

**New Streaming Architecture**:
1. **METADATA_ONLY Loading** ([eds_transforms.cpp:666-668](src/cpp/lib/transforms/eds_transforms.cpp#L666-L668)):
   - Only metadata loaded into RAM (~100MB for 100GB file)
   - String data read on-demand via `read_symbol()` from disk
   - Memory independent of file size

2. **Metadata-Only Merge Calculation** ([eds_transforms.cpp:133-249](src/cpp/lib/transforms/eds_transforms.cpp#L133-L249)): `compute_merge_metadata()`
   - Accepts `vector<MergeGroup>` (each group spans `count` consecutive positions)
   - For each group: iterative fold from position p₀ through p_{k-1} computing all valid output strings
   - String lengths computed via addition only (`len0 + len1 + ...`, no actual concatenation)
   - Source intersections via bitset fast-path (path IDs ≤ 63) or PathSet fallback
   - Returns `MergeMetadata` with `valid_indices_flat[m * group_count + k]` = which alternative from position `group_start+k` contributes to output string `m`
   - Memory: O(groups × metadata_per_group)

3. **Streaming Output** ([eds_transforms.cpp:452-603](src/cpp/lib/transforms/eds_transforms.cpp#L452-L603)): `stream_merged_symbols_to_file()`
   - Reads all `group_count` symbols for each group on-demand via `read_symbol()`
   - Builds each output string by concatenating `syms[k][valid_indices_flat[m*gc+k]]` for k=0..gc-1
   - Positions inside a group (non-start) are marked as `skip[pos]=true` and emitted as nothing
   - Writes directly to file stream (no ostringstream accumulation); flushes after each symbol
   - Memory: O(group_count × single_symbol) ≈ 1-10KB per group

4. **Temp File Iteration Chain** ([eds_transforms.cpp:620-779](src/cpp/lib/transforms/eds_transforms.cpp#L620-L779)):
   ```
   Input → temp0.eds (METADATA_ONLY)
   Iteration 1: temp0.eds → stream merge → temp1.eds
   Iteration 2: temp1.eds → stream merge → temp2.eds
   ...
   Final: tempN.eds → output
   ```
   - Each iteration loads temp file as METADATA_ONLY (constant memory)
   - Output streamed directly to next temp file
   - Old temp files cleaned up automatically

5. **Batch Processing** ([eds_transforms.cpp:712-731](src/cpp/lib/transforms/eds_transforms.cpp#L712-L731)):
   - Default: 1000 groups per batch
   - Controls parallel memory usage
   - Each batch: compute metadata → stream output → free memory → next batch
   - Prevents memory spikes from processing all groups at once

**Memory Footprint Comparison**:

| Scenario | Previous | New | Reduction |
|----------|----------|-----|-----------|
| 1GB EDS, 100 pairs, 4 threads | ~20GB | ~200MB | 100× |
| 10GB EDS, 500 pairs, 8 threads | ~200GB | ~400MB | 500× |
| 100GB EDS, 1000 pairs, 16 threads | ~2TB | ~600MB | 3000× |

**Memory Estimate**:
```
Total Memory = metadata + (batch_size × metadata_per_group) + streaming_buffer
             ≈ 100MB + (1000 × 10KB) + 100MB
             ≈ 210MB baseline for 100GB file
             + parallel overhead (threads × single_symbol_size)
             ≈ 210MB + (16 × 10KB) = 210.16MB
```

**Performance Trade-off**:
- 2-3× slower runtime (due to file I/O for on-demand symbol reading)
- 100-10000× memory reduction
- Acceptable trade-off for files that wouldn't fit in RAM otherwise

**Implementation Details**:
- **Chain selection** (`select_merge_groups()`): walks positions left-to-right; when `needs_merge(i, context_length)` is true, extends the chain while `needs_merge(i+1, ...)` is also true; emits one `MergeGroup`. Result: adjacent degenerate runs and short-context clusters collapse to a single group per chain.
- **Metadata-only merge** (`compute_merge_metadata()`): per-group iterative fold — initialize from p₀, fold in p₁..p_{k-1}; `valid_indices_flat` grows as outer-product of valid index tuples filtered by source intersection.
- **Streaming reconstruction** (`stream_merged_symbols_to_file()`): reads all group symbols, assembles each output string via `valid_indices_flat`, writes directly. **SEDS batching**: consecutive unmodified symbols accumulate into one `copy_range_to_stream()` call per run. **EDS raw-copy batching** (2026-07-03): unmodified full-bracket symbols are byte-copied verbatim via `EDS::copy_symbol_range_to_stream()` (the symmetric counterpart of the SEDS batch) instead of parsed into a `StringSet` and re-serialised — consecutive raw-copyable symbols flush in one call. A symbol is raw-copyable when its input span equals its full-bracket byte length (`Σ string_lengths + sym_size + 1`); compact input, inter-symbol whitespace, or the final symbol fall back to parse-and-reserialise. Output byte offsets are tracked via a manual `out_pos` counter (not `tellp()`) because raw copies defer physical writes. Measured: cartesian 1.9–2.2×, linear 1.16–1.63× faster on 20 MB inputs; output byte-identical to the pre-change serialiser.
- **Sequential seek elimination + bulk read** (`read_symbol_from_stream()` in `eds.cpp`): skips `seekg()` when the stream is already at the target position (symbols processed in order → almost all seeks eliminated), and reads each symbol's exact byte span in a single `stream_.read()` (2026-07-03) rather than one `get()`/`peek()` per byte, splitting the in-memory buffer on `,`. The last symbol's span runs to `stream_file_size()` (derived from the open stream, so it works on unlinked temp files).
- **Temp directory**: `std::filesystem::temp_directory_path() / "edsparser_leds_<pid>"`
- **Automatic cleanup**: Temp files removed on completion or error

**When to Use**:
- EDS files > 1GB: Significant memory savings
- EDS files > 10GB: Essential for machines with limited RAM
- Any file size: Works seamlessly (no mode selection needed)

**Example**:
```bash
# Memory-stable transformation (automatic with new implementation)
eds2leds -i large_100GB.eds -s large_100GB.seds -l 10 --threads 16

# Peak memory: ~600MB (instead of ~2TB with old implementation)
# Runtime: ~2-3× slower, but completes successfully
```

### Source Streaming for Large .seds Files

**Problem**: Large source files (multi-GB .seds files) consume excessive memory when loaded.

**Example Case**:
- EDS file: 84MB
- SEDS file: 13GB (155× larger!)
- Memory usage: 10.6GB resident (sources alone)

**Solution**: `Sources` class with streaming + LRU cache (implemented in [sources.hpp](src/cpp/lib/formats/sources.hpp) / [sources.cpp](src/cpp/lib/formats/sources.cpp)):

**Architecture**:
1. **Index building** (`parse_seds()`): Bulk reads (64KB chunks) + in-memory brace scanning to record position of each `{`. ~32 `read()` syscalls for a 2MB file. Avoids the original per-char `get()`+`tellg()` approach that caused ~2M function calls per index build.
2. **On-demand reading**: `read_source(idx)` seeks to indexed position, parses one `{path_id1,...}` set
3. **LRU cache**: Default 10K sets (~400KB), 98% hit rate due to locality in merge operations
4. **Bulk copy** (`copy_range_to_stream()`): Uses `base_positions_[start+count]` to compute exact byte range — reads precisely those bytes with no over-read, leaves stream at the start of the next entry. Sequential calls skip `seekg()` entirely (position-cache check). Falls back to brace-counting for the last batch only.

**Memory Footprint**:
```
Stream constructor:  13GB .seds file → ~10.6GB RAM (all sources in RAM)
EDS::load (default): index + cache = ~25MB for 13GB file  (420× reduction)
```

**Usage** (automatic with `EDS::load`):
```cpp
// Load with source streaming (no code changes needed!)
EDS eds = EDS::load("file.eds", "file_13GB.seds");
eds.get_sources_object()->set_cache_capacity(100000);  // optional: increase cache
std::set<int> paths = eds.read_source(string_id);
```

**Cache Size Guidelines**:
- Small files (<100K strings): stream/string constructor or cache all
- Medium files (100K-10M strings): 10K-100K cache (400KB-4MB)
- Large files (>10M strings): 100K-1M cache (4MB-40MB)

**Example**:
```bash
eds2leds -i file_84MB.eds -s file_13GB.seds -l 3 -o output.leds
# Peak memory: <500MB (instead of 10.6GB), ~1.5-2× slower
```

## Dependencies

- **CMake** 3.10+: Build system
- **C++17**: Required language standard
- **Boost** (program_options): Command-line argument parsing for tools
- **SDSL** (optional): Required for MSA transformations (suffix array construction). Install from https://github.com/simongog/sdsl-lite
- **divsufsort/divsufsort64** (optional): Required by SDSL
- **OpenMP** (optional): Parallel processing support
- **zstd** (optional): Enables the EDZ_COMPRESSED source format. CMake searches `$CONDA_PREFIX`, `$HOME`, and system paths for `zstd.h` + `libzstd`; when found it defines `EDSPARSER_HAVE_ZSTD` and links the library. Without it, EDZ_COMPRESSED load/save/read paths throw a clear error and every other format works unchanged.

## Using as a Library

The library can be integrated into other C++ projects via CMake:

```cmake
find_package(EDSParser REQUIRED)
target_link_libraries(your_target EDSParser::EDSParser)
```

After installation, CMake config files are located in `~/.local/lib/cmake/EDSParser/`.

## Experiments and Analysis

**Nothing about experiments lives in this repository.** The whole tree — specs,
the driver, dataset builders, run directories and collected result bundles —
sits at `~/Data/experiments/edsparser/`, and `experiments/` is gitignored so it
cannot creep back:

```
~/Data/experiments/edsparser/run.sh        driver: ./run.sh <spec>
~/Data/experiments/edsparser/specs/        xbench specs
~/Data/experiments/edsparser/scripts/      dataset builders
~/Data/experiments/edsparser/runs/         run directories
~/Data/experiments/edsparser/results/      collected bundles, one per dataset
~/Data/covid  ~/Data/synthetic  ~/Data/tb  inputs
```

`run.sh` still needs this checkout, because a spec resolves its binaries from
repo-relative paths (`resolve.prefer: build/tools`); it defaults to
`~/Documents/uni_projects/edsparser` and takes `EDSPARSER_REPO` as an override.
See `~/Data/experiments/edsparser/README.md` for the pipeline.

**Measuring is declarative, via xbench** (2026-08-16). Specs live in `~/Data/experiments/edsparser/specs/`, the engine lives in the sibling `xbench/` project (BIO-FMI uses the same one), and `run.sh` connects them: `~/Data/experiments/edsparser/run.sh merge_mode --dry-run`. Three specs: `merge_mode` (LINEAR vs CARTESIAN across an l sweep), `vcf2eds` (transform cost + `--block-size` trade-off), `source_formats` (SEDS vs EDZ variants vs gzip). The harness resolves `build/tools` before `PATH` and **aborts if `eds2leds` predates the complement fix**, records over-cap runs as `status=oom` rows with their peak rather than dropping them, sizes every input and artifact raw + gzip in its gather phase, and can replay extractors over archived stdout/stderr (`xbench analyze <run> --reextract`) instead of re-running. Run directories land in `~/Data/experiments/edsparser/runs/` (`XBENCH_RUNS` overrides).

This supersedes the collect/parse/compare half of the shell pipeline: `collect_results.sh` sizes → gather phase, `leds_runs.tsv` → the `status` column, `compare_merge_modes.py` → `summary.csv` joined on `tool`, `parse_transformation_logs.py` → `extract:` blocks. The shell runners still matter for 1000G-scale runs on the server (`run_leds.sh`'s screen-based scheduling has no xbench equivalent), and `compare_merge_modes.py` still reads the pre-existing `results/` bundles.

**Dataset building stays shell** — xbench measures binaries, it is not a data pipeline. Builders: `fetch_tb_dataset.sh` (M. tuberculosis panels from NCBI, no root or conda needed), `make_sample_subset.sh` (fewer samples), `make_allele_subset.sh` (drop long alleles). `run_subset_dataset.sh <panel>` with no l values runs VCF→EDS only, which is how the `.eds`/`.seds` inputs the `merge_mode` spec consumes get built; with l values it also runs the l-EDS sweep (`MODES`, default both merges). `run_tb_experiment.sh` drives the full panel × filter × l matrix; `collect_results.sh` bundles a run for transport.

**Result validity:** anything produced at ≤ 63 paths before 20d8ff1 measures the complement bug — see TODO.md §0a. `experiments/results/yeast1011_50` and `tb_100` are invalid; `hgp1000`, `tb_p100`, `tb_p500`, `tb_p1141` and their `_snv50` twins are not.

## Testing

### The unit suite builds and passes (repaired 2026-08-20)

`ctest` had drifted badly: `test_eds` did not compile (and so had not run since
086e842), and `test_sources`, `test_stats`, `test_merge` and `test_vcf` each
aborted on the first assertion in their area. All seven now pass, alongside the
nine e2e suites.

Repairing them turned up **four real defects**, since fixed, which is the reason
a green e2e run is not evidence that library internals are correct:

- **`find_symbol_at_common_position()` read out of bounds** for any common
  position at or past the total common-character count: `upper_bound` returns
  `end()`, `symbol_idx` becomes `n`, and every per-symbol array is indexed one
  past the end. Segfaulted ~2 runs in 3. Reachable from the public
  `check_position()`, which is written to catch `out_of_range` from it — the
  read happened first. (Valgrind catches it; ASan does not, because the first
  over-read lands inside a `std::vector<bool>` word.)
- **An out-of-range degenerate string id** was reported as `"Internal error: ...
  maps to non-degenerate symbol"`. Now `out_of_range`, naming the universe size.
- **A stray `}` was absorbed as a sequence character** — `"ACGT}"` parsed as one
  5-character symbol — so a truncated file parsed "successfully" with braces
  inside its strings. Unmatched `{` was already rejected; this is the symmetric
  check. Compact-format bare symbols are unaffected.
- **`vcf2eds` counted variants past the end of the reference as processed**, then
  dropped them, reporting a 100% success rate while discarding the tail. Now a
  `skipped_out_of_range` count plus a stderr warning, since the usual cause is a
  VCF/FASTA mismatch.

Two things worth knowing about the tests themselves:

- **`check_position()` has no caller anywhere in the repo**, so its tests are its
  entire specification — and they used to contradict themselves about the
  coordinate convention. It is now pinned in `test_check_position_basic`:
  `common_pos` indexes **common characters only**; `degenerate_strings` are
  **global** degenerate-string ids that must belong to a symbol the match
  actually traverses (otherwise `invalid_argument`), while ids for symbols
  outside the traversed range are warned about and ignored. A match cannot begin
  inside a degenerate symbol.
- **Half of `test_merge` merged nothing.** `{G,C}{T}`, `{T}{A,C,G}`, `{,A}{T}`,
  `{ACGT}{G,C}{T}` and `{A}{B,C}{D}` all place their short common symbol at a
  boundary, where `needs_merge()`'s `i > 0 && i < n - 1` exempts it, so they
  asserted results the transform correctly never produces. They now use interior
  placements, and the exemption is asserted deliberately in two tests instead of
  accidentally in five. `test_merge` also covers the **path-count invariant**
  (TODO 0b): a linear merge can never produce more strings in one symbol than
  there are paths — the assertion the complement-source bug (20d8ff1) would have
  failed.

### Test Categories

| Test | Auto (ctest) | Purpose |
|------|-------------|---------|
| `test_eds`, `test_sources`, `test_stats`, `test_merge`, `test_msa`, `test_vcf` | Yes | Unit tests for core library |
| `test_integration` | Yes | End-to-end CLI tool workflows (all tools, all formats) |
| `test_memory_smoke` | **No** | Quick memory validation with 10-50MB files, 2GB limit (~1-2 min) |
| `test_memory_stress` | **No** | Full stress testing with 100-500MB files, leak detection (~30+ min) |

**`test_eds` coverage note**: Both construction modes are tested.
- METADATA_ONLY (file loader via `EDS::load`): covered by most existing tests via `create_temp_eds()`
- FULL (in-memory via stream/string ctors): covered by `test_stream_constructor`, `test_from_string_factory`, `test_mode_equivalence`, `test_full_mode_edge_cases` (Tests A1–A4)
- Mode equivalence: `test_mode_equivalence` constructs the same EDS via string ctor and file loader and asserts all observable outputs match (`length`, `cardinality`, `size`, all `read_symbol(i)`, all metadata fields)

### End-to-End Suites ([tests/e2e/](tests/e2e/))

Not run by ctest — shell suites driving the installed CLI tools. `bash tests/e2e/run_all.sh` runs all nine: `test_msa2eds`, `test_vcf2eds`, `test_eds2leds`, `test_leds_incremental`, `test_source_transform`, `test_seds_edz`, `test_stats`, `test_genpatterns`, `test_genrandomeds`. `test_eds2leds.sh` is the gate for `--max-memory` (exit 3), `--source-format`, and `--block-size` byte-identity vs a whole-file run; `test_leds_incremental.sh` guards the idempotence/monotonicity invariant.

**All e2e tests are expected to pass.** An older note claimed some `test_eds2leds.sh` tests fail intentionally to document a compact-output bug — that bug is fixed and those tests are gone.

**The suites resolve tools via `PATH` first** (`find_tool()` in `tests/e2e/helpers.sh`), falling back to `build/tools/` only when the name isn't on `PATH`. A stale `~/.local/bin` copy silently fails every test for a flag it predates — run `make install`, or `PATH="$PWD/build/tools:$PATH" bash tests/e2e/run_all.sh`, before believing a failure.

### Benchmark Scenarios ([tests/bench/](tests/bench/))

`bench.sh --size standard --scenario <name>` runs named scenarios; `all` runs everything.

Key scenarios relevant to recent changes:
- **`chain_merging`**: tests `eds2leds` on inputs generated with `--min-context 0` (Bernoulli mode), which produces adjacent degenerates and short contexts that actually trigger merging. Sweeps variability (0.01, 0.05). Logs iteration count alongside throughput — confirms 1 iteration with the chain-merging fix. **Important**: all other eds2leds scenarios use `--min-context 5` with l=5, so 0 merge work is done; only `chain_merging` and `context_length_sweep` exercise the actual merge path.
- **`context_length_sweep`**: varies l (3, 5, 10, 20) on input generated without `--min-context`, revealing how merge cost scales with the required context length.
- **`variability_sweep`**: varies variant density (0.01–0.40); uses `--min-context 5`, so runtime reflects I/O and metadata overhead rather than merge work.

### Running Memory Tests Manually
```bash
cd build/tools
./test_memory_smoke    # Quick: validates streaming + LRU cache
./test_memory_stress   # Full: memory growth detection via linear regression
```

**Binary locations**: `ctest` must run from `build/src/cpp` (where `CTestTestfile.cmake` lives), but every test *executable* is written to `build/tools/`. Docs said `build/src/cpp` for both until 2026-08-11.

## Test Data

Test fixtures live in [tests/e2e/data/](tests/e2e/data/) (`.eds`, `.seds`, `.edz`, `.msa`, `.fa`) with expected outputs in [tests/e2e/expected/](tests/e2e/expected/), plus [tests/stress/data/](tests/stress/data/) for the memory tests. An older `data/test/` directory no longer exists. Scenarios covered:
- Simple and complex degenerate symbols
- Edge cases (empty strings, adjacent degenerates, boundaries)
- Files with sources (`.seds` files)
- Pre-transformed l-EDS examples for validation
