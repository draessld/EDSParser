# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

EDSParser is a C++ library for parsing and transforming Elastic-Degenerate Strings (EDS), a data structure for representing sequence variation in bioinformatics. The library supports:
- **Multiple input formats**: MSA (Multiple Sequence Alignment), VCF (Variant Call Format), and native EDS
- **Format transformations**: Convert between formats and produce length-constrained EDS (l-EDS)
- **Memory-efficient streaming**: Two storage modes (FULL vs METADATA_ONLY) for handling large datasets
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
cd build/src/cpp && ./test_eds

# Available tests (auto-run by ctest): test_eds, test_sources, test_stats, test_merge, test_msa, test_vcf, test_integration

# Manual memory stress tests (too slow for CI, run manually):
cd build/src/cpp && ./test_memory_stress

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
- Two storage modes:
  - `FULL`: All strings loaded into RAM (backward compatible)
  - `METADATA_ONLY`: Only metadata/index in RAM, strings streamed from disk (memory-efficient for large files)
- Supports source tracking via separate .seds files (managed by `Sources` class)
- Key operations:
  - Loading/parsing from streams or files
  - Statistics computation (metadata includes `cum_common_positions`, `cum_degenerate_counts`)
  - Position checking (verify if pattern occurs at position)
  - Merging adjacent symbols (CARTESIAN without sources, LINEAR with sources)
  - Pattern generation for benchmarking
- New source API: `set_sources_object()`, `get_sources_object()`, `read_source(idx)`, `has_sources()`
- **Note**: `get_sources()` throws in METADATA_ONLY mode; use `read_source(idx)` instead

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
  - **Memory-Stable Architecture** for 100GB+ files:
    - Uses METADATA_ONLY mode (only metadata in RAM, ~100MB for 100GB file)
    - Iterative temp file chaining (iteration N → temp file → iteration N+1)
    - **Per-process unique temp directory** (`/tmp/edsparser_leds_<pid>/`) — prevents conflicts when multiple `eds2leds` instances run in parallel (e.g. from experiment scripts)
    - Batch metadata accumulation: all `MergeMetadata` is collected across batches before `stream_merged_symbols_to_file` is called once — avoids writing every unmodified symbol once per batch
    - Streaming output with immediate flushing (no ostringstream accumulation)
    - Memory footprint: O(metadata + batch) instead of O(file_size × iterations × threads)
    - Typical reduction: 2TB → 500MB peak memory for 100GB EDS with 1000 pairs and 16 threads

**Command-Line Tools** ([src/cpp/tools/](src/cpp/tools/))

Transformation tools (each focused on a specific conversion):
- `eds2leds`: Transform EDS to l-EDS with linear or cartesian merging
  - **`--full` flag**: Force full bracket format output (default: compact)
  - Runs complexity estimation before transformation and warns on exponential-growth risk
  - **Linear vs cartesian throughput**: after I/O optimizations (2026-05-26) the gap is 1.26× (linear ~4.3 MB/s vs cartesian ~5.4 MB/s on 10% variability, 4-path, --min-context 5 data). Remaining difference is inherent: linear writes an extra SEDS temp file per iteration; cartesian writes no SEDS data.
- `msa2eds`: Transform MSA to EDS/l-EDS with source tracking
- `vcf2eds`: Transform VCF to EDS/l-EDS with sample-level source tracking (requires reference FASTA)
  - **`--block-size` parameter**: Control memory usage for large VCF files (default: 10M bases)
  - Smaller block sizes (e.g., 1M) reduce memory at the cost of more I/O
  - Larger block sizes (e.g., 100M) increase performance but use more memory
  - Set to 0 for legacy mode (loads all variants, high memory)

Utility tools:
- `edsparser-stats`: Display EDS statistics, memory estimates, l-EDS compliance
- `edsparser-genpatterns`: Generate random patterns for benchmarking
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
- `.seds`: Sources file (text format mapping string IDs to path IDs)
- `.edz`: Sources file (binary format, not yet fully implemented)
- `.leds`: Length-constrained EDS (minimum context length guaranteed)
- `.peds`: Phased EDS (combined .eds + .seds, planned but not implemented)

### Key Design Patterns

**Streaming Architecture**: Transform functions accept `std::istream&` and `std::ostream&` rather than file paths. This enables:
- Memory-efficient processing of large files
- Easy unit testing with `std::stringstream`
- Flexible I/O (files, pipes, memory buffers)

**Two-Phase Loading**: EDS class uses metadata-first approach:
1. Parse file to build index and statistics
2. Optionally load all strings (FULL mode) or keep file handle open for streaming (METADATA_ONLY mode)

**Sources as Separate Class** ([src/cpp/lib/formats/sources.hpp](src/cpp/lib/formats/sources.hpp), [src/cpp/lib/formats/sources.cpp](src/cpp/lib/formats/sources.cpp)): Source tracking (provenance) is managed by a dedicated `Sources` class, separate from EDS. This allows:
- Using EDS without sources (simpler, faster)
- Loading sources on-demand with LRU cache (default: 10K entries)
- Different merging strategies (LINEAR requires sources, CARTESIAN doesn't)
- Format extensibility: SEDS (text), EDZ (binary, planned), EDZ_COMPRESSED (planned)

Key methods:
- `Sources::load()` — factory method, returns `shared_ptr<Sources>`
- `Sources::read_source(idx)` — access with automatic LRU caching; **returns by value** (thread-safe copy)
- `Sources::read_source_ref(idx)` — returns a const reference into the LRU cache for single-threaded use only; **do not use in parallel contexts** — another thread can evict the cache entry, dangling the reference
- `Sources::merge_adjacent_sources()` / `intersect_sources()` — set operations for l-EDS; uses `read_source()` (not `read_source_ref()`) to avoid dangling references across OpenMP threads
- `EDS::set_sources_object()` / `get_sources_object()` — attach/retrieve Sources on EDS
- `EDS::read_source(idx)` — delegates to Sources (works in both FULL and METADATA_ONLY modes)

**Thread safety**: `Sources` has a `mutable std::mutex io_mutex_` protecting the shared `stream_` and LRU cache from concurrent access. Both `read_source()` and `read_source_ref()` acquire this mutex. However, `read_source_ref()` releases the mutex before returning — the reference is only safe while no other thread calls any `Sources` method. In practice: always use `read_source()` in parallel code paths.

**Note**: `EDS::get_sources()` throws a helpful error in METADATA_ONLY mode. Use `read_source(idx)` instead.

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
   - Calculates merge results using ONLY metadata (no string data)
   - Computes string lengths via addition: `len1 + len2` (no actual concatenation)
   - Computes source intersections via set operations
   - Returns `MergeMetadata` structs with sizes, lengths, sources (NO strings)
   - Memory: O(pairs × metadata_per_pair) ≈ 10MB for 1000 pairs

3. **Streaming Output** ([eds_transforms.cpp:452-603](src/cpp/lib/transforms/eds_transforms.cpp#L452-L603)): `stream_merged_symbols_to_file()`
   - Reads symbols on-demand via `read_symbol()` (METADATA_ONLY compatible)
   - Merges strings on-the-fly: `set1[i] + set2[j]` (immediate concatenation, no storage)
   - Writes directly to file stream (no ostringstream accumulation)
   - Flushes after each symbol to prevent buffering
   - Memory: O(single_symbol) ≈ 1-10KB per symbol

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
   - Default: 1000 pairs per batch
   - Controls parallel memory usage
   - Each batch: compute metadata → stream output → free memory → next batch
   - Prevents memory spikes from processing all pairs at once

**Memory Footprint Comparison**:

| Scenario | Previous | New | Reduction |
|----------|----------|-----|-----------|
| 1GB EDS, 100 pairs, 4 threads | ~20GB | ~200MB | 100× |
| 10GB EDS, 500 pairs, 8 threads | ~200GB | ~400MB | 500× |
| 100GB EDS, 1000 pairs, 16 threads | ~2TB | ~600MB | 3000× |

**Memory Estimate**:
```
Total Memory = metadata + (batch_size × metadata_per_pair) + streaming_buffer
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
- **Metadata-only merge** ([eds_transforms.cpp:140-225](src/cpp/lib/transforms/eds_transforms.cpp#L140-L225)): Computes all merge logic without touching string data
- **Streaming reconstruction** (`stream_merged_symbols_to_file()`): Reads merged positions on-the-fly, concatenates, writes immediately. **SEDS batching**: consecutive unmodified symbols accumulate into one `copy_range_to_stream()` call per run (one call per merge boundary instead of one per symbol — ~2727 calls vs ~200K for 10% variability).
- **Sequential seek elimination** (`read_symbol_from_stream()` in `eds.cpp`): skips `seekg()` when stream is already at the target position. Since symbols are processed in order, almost all seeks are eliminated (down to ~5K per iteration from ~400K).
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
FULL mode:   13GB .seds file → ~10.6GB RAM
METADATA_ONLY: index + cache = ~25MB for 13GB file  (420× reduction)
```

**Usage** (automatic in METADATA_ONLY mode):
```cpp
// Load with source streaming (no code changes needed!)
EDS eds = EDS::load("file.eds", "file_13GB.seds", EDS::StoringMode::METADATA_ONLY);
eds.set_source_cache_capacity(100000);  // optional: increase cache
std::set<int> paths = eds.read_source(string_id);
```

**Cache Size Guidelines**:
- Small files (<100K strings): FULL mode or cache all
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

## Using as a Library

The library can be integrated into other C++ projects via CMake:

```cmake
find_package(EDSParser REQUIRED)
target_link_libraries(your_target EDSParser::EDSParser)
```

After installation, CMake config files are located in `~/.local/lib/cmake/EDSParser/`.

## Experiments and Analysis

### Performance Analysis Tools

**Transformation Log Parser** ([experiments/parse_transformation_logs.py](experiments/parse_transformation_logs.py))

Script to extract performance metrics from transformation log files:

```bash
# View performance metrics in console
python3 experiments/parse_transformation_logs.py experiments/datasets/SARS_cov2

# Export to CSV for analysis
python3 experiments/parse_transformation_logs.py experiments/datasets/SARS_cov2 \
    --output sars_cov2_performance.csv
```

**Features**:
- Automatically discovers all `.eds.log` and `.leds.log` files in dataset directory
- Extracts: variant name, transformation type, context length, runtime, peak memory, threads
- Outputs CSV compatible with pandas, Excel, plotting libraries
- Supports all transformation types: MSA→EDS, EDS→l-EDS, VCF→EDS, etc.

**Expected directory structure**:
```
datasets/<dataset_name>/
├── eds/
│   ├── variant1.eds.log
│   └── variant2.eds.log
├── 3_leds/
│   ├── variant1.leds.log
│   └── variant2.leds.log
├── 5_leds/
│   └── ...
└── 10_leds/
    └── ...
```

**Output CSV columns**:
- `variant_name`: Name of the variant/sequence
- `transformation_type`: MSA→EDS, EDS→l-EDS, VCF→EDS, etc.
- `context_length`: l-EDS context length (empty for regular EDS)
- `runtime_seconds`: Transformation runtime
- `memory_mb`: Peak memory usage
- `threads`: Number of threads used
- `log_file`: Relative path to source log file

### Analysis Notebooks

The experiments directory contains Jupyter notebooks for analyzing transformation results. Each dataset has a modular notebook structure:

**Notebook Structure** (located in `experiments/datasets/<dataset_name>/`):

1. **[00_load_data.ipynb](experiments/datasets/SARS_cov2/00_load_data.ipynb)** - Data Loading and Processing
   - Loads all CSV files (statistics.csv, transformation_performance.csv, detailed statistics)
   - Merges data into comprehensive datasets
   - Saves processed data to `processed_data/` directory for use by analysis notebooks
   - **Run this first** before any analysis notebooks

2. **[01_time_memory_analysis.ipynb](experiments/datasets/SARS_cov2/01_time_memory_analysis.ipynb)** - Transformation Performance Analysis
   - Time consumption per variant (all transformations)
   - Memory consumption per variant (all transformations)
   - Context length impact on EDS→l-EDS transformation time and memory
   - Detailed analysis with box plots, progression charts, and relative increases
   - Time vs Memory scatter plots
   - Statistical summaries by transformation type

3. **[02_file_size_analysis.ipynb](experiments/datasets/SARS_cov2/02_file_size_analysis.ipynb)** - File Size and Compression Analysis
   - Input vs EDS+SEDS size comparison (stacked bars showing EDS and SEDS components)
   - l-EDS sizes with dual y-axis visualization for SEDS overhead
   - Compression ratio statistics and SEDS overhead analysis
   - Context length impact on compression ratios

4. **[03_eds_statistics.ipynb](experiments/datasets/SARS_cov2/03_eds_statistics.ipynb)** - EDS Structure Analysis
   - Symbol statistics across transformations (total symbols, degenerate symbols, context length)
   - Memory reduction factors comparison
   - Context length impact on reduction/compression factors (box plots and progression)
   - Correlation matrix for EDS metrics
   - Key insights and summary tables

**Workflow**:
```bash
# 1. First, run data loading notebook
jupyter notebook experiments/datasets/SARS_cov2/00_load_data.ipynb

# 2. Then run any analysis notebook independently
jupyter notebook experiments/datasets/SARS_cov2/01_time_memory_analysis.ipynb
jupyter notebook experiments/datasets/SARS_cov2/02_file_size_analysis.ipynb
jupyter notebook experiments/datasets/SARS_cov2/03_eds_statistics.ipynb
```

**Key Features**:
- **Modular design**: Each notebook focuses on one analysis area
- **Independent execution**: Analysis notebooks can run in any order after data loading
- **Comprehensive visualizations**: 20+ graphs covering all aspects of transformation performance
- **Statistical summaries**: Detailed tables with mean, median, std dev, min, max
- **Relative analysis**: Shows percentage changes and improvements across context lengths

## Testing

### Test Categories

| Test | Auto (ctest) | Purpose |
|------|-------------|---------|
| `test_eds`, `test_sources`, `test_stats`, `test_merge`, `test_msa`, `test_vcf` | Yes | Unit tests for core library |
| `test_integration` | Yes | End-to-end CLI tool workflows (all tools, all formats) |
| `test_memory_smoke` | **No** | Quick memory validation with 10-50MB files, 2GB limit (~1-2 min) |
| `test_memory_stress` | **No** | Full stress testing with 100-500MB files, leak detection (~30+ min) |

### Running Memory Tests Manually
```bash
cd build/src/cpp
./test_memory_smoke    # Quick: validates streaming + LRU cache
./test_memory_stress   # Full: memory growth detection via linear regression
```

## Test Data

Test files are located in [data/test/](data/test/) with various EDS scenarios:
- Simple and complex degenerate symbols
- Edge cases (empty strings, adjacent degenerates, boundaries)
- Files with sources (`.seds` files)
- Pre-transformed l-EDS examples for validation
