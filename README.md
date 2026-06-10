# EDSParser

A high-performance C++ library for parsing and transforming **Elastic-Degenerate Strings (EDS)**, a data structure for representing sequence variation in bioinformatics.

## Features

- **Multiple Input Formats**: MSA (Multiple Sequence Alignment), VCF (Variant Call Format), and native EDS
- **Format Transformations**: Convert between formats and produce length-constrained EDS (l-EDS)
- **Random EDS Generation**: Create synthetic datasets with controlled variability for testing and benchmarking
- **Memory-Efficient Streaming**: Handle large datasets with minimal memory footprint
- **Source Tracking**: Maintain provenance information through transformations
- **High Performance**: C++17 implementation with optional OpenMP parallelization

## Quick Start

### Installation

```bash
# Clone the repository
git clone <repository-url>
cd edsparser

# Install dependencies and build
./INSTALL.sh
```

This will:
- Check for required dependencies (CMake, g++, Boost, SDSL)
- Build the C++ library and command-line tools
- Run tests automatically
- Install to `~/.local/` (library, headers, tools)

### Basic Usage

```bash
# Transform MSA to EDS
msa2eds -i alignment.msa -o output.eds

# Transform MSA directly to l-EDS (length-constrained)
msa2eds -i alignment.msa -l 10 -o output.leds

# Transform VCF to EDS
vcf2eds -i variants.vcf --reference genome.fasta -o output.eds

# Transform EDS to l-EDS
eds2leds -i data.eds -s data.seds -l 10

# Generate random EDS for testing
genrandomeds --ref-size-mb 100 --variability 0.10 -o random.eds

# View EDS statistics
edsparser-stats -i data.eds
```

## Project Structure

```
edsparser/
├── src/cpp/
│   ├── lib/                    # Core library
│   │   ├── formats/            # EDS, MSA, VCF parsers
│   │   └── transforms/         # Transformation algorithms
│   └── tools/                  # Command-line tools
│       ├── msa2eds             # MSA → EDS/l-EDS
│       ├── vcf2eds             # VCF → EDS/l-EDS
│       ├── eds2leds            # EDS → l-EDS
│       ├── edsparser-stats     # Statistics tool
│       ├── edsparser-genpatterns  # Pattern generation tool
│       └── genrandomeds        # Random EDS generation (internal)
├── tests/
│   ├── unit/                   # C++ unit tests
│   └── e2e/                    # Shell-based end-to-end tests
├── data/test/                  # Test data
├── INSTALL.sh                  # Installation script
├── UNINSTALL.sh                # Uninstallation script
└── README.md                   # This file
```

## Command-Line Tools

### msa2eds - MSA to EDS/l-EDS Transformation

Transform Multiple Sequence Alignments (FASTA format with gaps) to EDS:

```bash
# Basic transformation
msa2eds -i alignment.msa

# Direct to l-EDS with minimum context length
msa2eds -i alignment.msa -l 10

# Custom output paths
msa2eds -i alignment.msa -o custom.eds -s custom.seds
```

**Features:**
- Automatic source tracking (one path per input sequence)
- LINEAR merging (phasing-aware, preserves valid combinations)
- Streaming architecture (memory-efficient)

### vcf2eds - VCF to EDS/l-EDS Transformation

Transform VCF files (with reference genome) to EDS:

```bash
# Basic transformation
vcf2eds -i variants.vcf --reference genome.fasta

# Direct to l-EDS
vcf2eds -i variants.vcf --reference genome.fasta -l 10

# Sample-level source tracking
vcf2eds -i variants.vcf --reference genome.fasta -o output.eds

# Large VCF files: adjust block size for memory optimization
vcf2eds -i large.vcf --reference genome.fasta --block-size 1000000  # 1M bases (lower memory)
vcf2eds -i large.vcf --reference genome.fasta --block-size 100000000  # 100M bases (higher memory, faster)
```

**Features:**
- Handles SNPs and small indels
- Simple deletions (`<DEL>`) and insertions (`<INS>`)
- **Inversions (`<INV>`)** - converted to reverse complement
- **Copy Number Variations (`<CN0>`, `<CN1>`, `<CN2>`, etc.)** - handles deletions and duplications
- Multi-allelic site support
- Sample-level source tracking
- Streaming reference processing
- **Block-based processing** for memory-efficient handling of large VCF files (default 10M bases per block)

### eds2leds - EDS to l-EDS Transformation

Transform EDS to length-constrained EDS with auto-detected merging method:

```bash
# Linear merging (auto-detected with sources, compact output by default)
eds2leds -i data.eds -s data.seds -l 10

# Cartesian merging (auto-detected without sources)
eds2leds -i data.eds -l 10

# Full output format (brackets on all symbols)
eds2leds -i data.eds -s data.seds -l 10 --full

# Parallel processing
eds2leds -i data.eds -l 10 --threads 4
```

**Merging Methods (auto-detected):**
- **LINEAR**: Phasing-aware merging using source information (preserves valid haplotypes) - automatically used when sources provided
- **CARTESIAN**: All-combinations merging (cross-product of alternatives) - automatically used when no sources provided

**Output Format:**
- Default: Compact format (brackets only on degenerate symbols)
- Use `--full` flag for full format (brackets on all symbols)

### edsparser-stats - Statistics and Analysis

Display EDS statistics and metadata:

```bash
# Basic statistics
edsparser-stats -i data.eds

# With source tracking
edsparser-stats -i data.eds -s data.seds

# JSON output
edsparser-stats -i data.eds --json

# CSV output
edsparser-stats -i data.eds --csv

# Verbose output (detailed statistics)
edsparser-stats -i data.eds --verbose
```

**Options:**
- `-j, --json` - Output in JSON format
- `-c, --csv` - Output in CSV format
- `-v, --verbose` - Show detailed statistics

**Output:**
- Number of symbols, characters, and strings
- Context length statistics (min, max, average)
- File size and memory estimates (METADATA_ONLY vs FULL mode)
- Source tracking information (number of paths/genomes)
- l-EDS compliance verification

### edsparser-genpatterns - Pattern Generation

Generate random patterns from EDS files for benchmarking:

```bash
# Generate 100 patterns of length 10
edsparser-genpatterns -i data.eds -o patterns.txt -n 100 -l 10

# Generate 1000 patterns of length 20
edsparser-genpatterns -i data.eds -o patterns.txt -n 1000 -l 20

# From l-EDS files
edsparser-genpatterns -i data.leds -o patterns.txt -n 500 -l 15
```

**Options:**
- `-i, --input` - Input EDS/l-EDS file
- `-o, --output` - Output pattern file
- `-n, --count` - Number of patterns to generate (default: 100)
- `-l, --length` - Pattern length (default: 10)

**Output:**
Plain text file with one pattern per line (ACGT alphabet).

### genrandomeds - Random EDS Generation

Installed to `~/.local/bin/genrandomeds` by `./INSTALL.sh`.

Generate synthetic EDS files with controlled variability for testing and benchmarking:

```bash
# Generate 100 MB EDS with 10% variability
genrandomeds --ref-size-mb 100 --variability 0.10 -o random.eds

# Generate l-EDS with minimum context length
genrandomeds --ref-size-mb 50 --variability 0.05 --min-context 50 -o random.leds

# High variability with more alternatives per variant
genrandomeds --ref-size-mb 10 --variability 0.20 \
  --min-alternatives 3 --max-alternatives 6 -o high_var.eds

# Reproducible generation with seed
genrandomeds --ref-size-mb 10 --variability 0.10 --seed 42 -o test.eds
```

**Options:**
- `--ref-size-mb` - Reference size in megabytes (required, 1 MB = 1,000,000 bp)
- `-v, --variability` - Fraction of positions with variants (default: 0.10 = 10%)
- `--min-alternatives` - Minimum strings per degenerate symbol (default: 2)
- `--max-alternatives` - Maximum strings per degenerate symbol (default: 4)
- `--variant-length-max` - Maximum indel length in bp (default: 10)
- `--snp-ratio` - Fraction of variants that are SNPs vs indels (default: 0.70)
- `--alphabet` - Character alphabet for sequence generation (default: "ACGT")
- `--min-context` - Minimum context between variants for l-EDS compliance (default: 0 = disabled)
- `--seed` - Random seed for reproducibility (optional)
- `-o, --output` - Output EDS file (required)

**Features:**
- Automatically generates `.seds` (source) file alongside `.eds` file
- **Round-robin haplotype assignment**: each path is assigned to exactly one alternative per degenerate symbol, mirroring real phased genomic data. This prevents Cartesian product explosion during linear l-EDS merging.
- Number of paths (samples) automatically calculated as `max(max_alternatives, 3)`
- Generates realistic variation: SNPs, insertions, and deletions
- Progress reporting and performance metrics

**Output:**
- `output.eds` - Random EDS file with controlled variability
- `output.seds` - Source tracking file with path assignments

**Example Use Cases:**
- **Benchmarking**: Generate large synthetic datasets for performance testing
- **Testing**: Create controlled test cases with specific variation patterns
- **Simulation**: Model sequence variation with adjustable mutation rates
- **Validation**: Verify tools work correctly with various EDS characteristics

## File Formats

### EDS Format (`.eds`)

Elastic-Degenerate String with curly braces notation:

**Full format** (brackets on all symbols):
```
{ACGT}{A,ACA}{CGT}{T,TG}
```

**Compact format** (brackets only on degenerate symbols):
```
ACGT{A,ACA}CGT{T,TG}
```

- Each `{...}` represents a symbol (set of alternative strings)
- Single strings: non-degenerate symbols
- Multiple strings: degenerate symbols (variants)
- Both formats are supported for reading; output is compact by default — use `--full` to force full format

### SEDS Format (`.seds`)

Source tracking file mapping each string to its originating sequences/samples:

```
{0}{1,2,3,5}{2,3,4}{0}{1,2,3,5}...
```

**Format**: Flattened sequence of sets, one per string (not per symbol):
- Each `{...}` contains source IDs (path/sample IDs) for one string
- Order follows the global string indexing: all strings from symbol 0, then symbol 1, etc.
- Example: For EDS `{A}{B,C}{D}` the .seds has 4 sets: one for A, two for B and C, one for D

**Special marker**: `{0}` represents "all paths" (universal marker)

### MSA Format (`.msa`)

FASTA format with aligned sequences:

```
>seq1
ACGT-TAG
>seq2
ACGTATAG
>seq3
ACGT--AG
```

- Gap character: `-`
- All sequences must be same length (aligned)

### VCF Format (`.vcf`)

Standard Variant Call Format (requires reference FASTA)

## Running Experiments

Experiment scripts are located in the parent `biofmi` repository at `experiments/`.
They cover the full pipeline: data generation, EDS transformation, FM-index building, and pattern querying.

```bash
cd biofmi/experiments
./run_full_experiment.sh          # 6-step end-to-end smoke test
```

See [biofmi/experiments/README.md](../../experiments/README.md) for full documentation.

## Development

### Building from Source

```bash
# Clean build
rm -rf build && mkdir build && cd build

# Configure (Release build)
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local ..

# Build
make -j$(nproc)

# Run tests
ctest --output-on-failure

# Install
make install
```

### Debug Build

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

### Running Tests

EDSParser has two test layers: **unit tests** (C++, fast) and **end-to-end tests** (shell, full CLI).

#### Unit Tests

```bash
cd build/src/cpp

# Run all unit tests
ctest --output-on-failure

# Run a specific test executable
./test_eds
./test_msa
./test_vcf
```

Available unit tests:

| Executable | Coverage | Auto (ctest) |
|---|---|---|
| `test_eds` | EDS parsing and operations | Yes |
| `test_sources` | Source tracking | Yes |
| `test_stats` | Statistics computation | Yes |
| `test_merge` | Symbol merging algorithms | Yes |
| `test_msa` | MSA parsing | Yes |
| `test_vcf` | VCF parsing | Yes |
| `test_integration` | End-to-end CLI tool workflows | Yes |
| `test_memory_smoke` | Quick memory validation (~1-2 min) | **No — run manually** |
| `test_memory_stress` | Full memory stress + leak detection (~30+ min) | **No — run manually** |

#### End-to-End Tests

Shell-based tests that invoke the installed CLI tools against reference data. Requires the tools to be installed (run `./INSTALL.sh` first, or ensure `~/.local/bin` is on `PATH`).

```bash
# Run all e2e suites
bash tests/e2e/run_all.sh

# Run a single suite
bash tests/e2e/test_msa2eds.sh
bash tests/e2e/test_vcf2eds.sh
bash tests/e2e/test_eds2leds.sh
bash tests/e2e/test_stats.sh
bash tests/e2e/test_genpatterns.sh
```

Each suite reports individual test results and a per-suite pass/fail summary. `run_all.sh` reports overall suite counts.

> **Note:** Some `test_eds2leds.sh` tests currently fail intentionally — they document a known compact-output bug (see `TODO`). Expected output files already contain the correct format and will pass once the bug is fixed.

#### Benchmarks

Shell-based performance measurement for all CLI tools. Requires installed tools.

```bash
# Quick smoke run (~10 s, N=1)
bash tests/bench/bench.sh --size quick

# Standard run with median of 3 reps (~2 min)
bash tests/bench/bench.sh --size standard

# Check for regressions against stored baseline
bash tests/bench/bench_compare.sh
```

Results are written to `tests/bench/results/YYYY-MM-DD_HH-MM-SS.csv` with columns:
`timestamp, preset, scenario, tool, input_size_mb, runtime_s, peak_memory_mb, throughput_mb_s`.

On first run, `bench_compare.sh` bootstraps `baseline.csv` from the latest result. To promote a new baseline after an intentional performance change: `cp tests/bench/results/LATEST.csv tests/bench/baseline.csv`.

## Using as a Library

EDSParser can be integrated into other C++ projects:

### CMake Integration

```cmake
find_package(EDSParser REQUIRED)
target_link_libraries(your_target EDSParser::EDSParser)
```

### Example Code

```cpp
#include <edsparser/formats/eds.hpp>
#include <edsparser/transforms/eds_transforms.hpp>

// Load EDS from file
EDS eds;
eds.load("data.eds", StorageMode::FULL);

// Get statistics
auto stats = eds.get_statistics();
std::cout << "Symbols: " << stats.total_symbols << std::endl;

// Transform to l-EDS
transform_eds_to_leds(
    std::ifstream("data.eds"),
    std::ifstream("data.seds"),
    std::ofstream("output.leds"),
    10,  // context length
    MergingStrategy::LINEAR
);
```

## Dependencies

### Required
- **CMake** 3.10+
- **C++17** compatible compiler (g++, clang++)
- **Boost** (program_options) - Command-line argument parsing

### Optional
- **SDSL** - Required for MSA transformations (suffix array construction)
  - Install: https://github.com/simongog/sdsl-lite
- **divsufsort/divsufsort64** - Required by SDSL
- **OpenMP** - Parallel processing support

### Installing Dependencies

**Ubuntu/Debian:**
```bash
sudo apt-get install cmake g++ libboost-program-options-dev
```

**macOS:**
```bash
brew install cmake boost
```

**SDSL (optional):**
```bash
git clone https://github.com/simongog/sdsl-lite
cd sdsl-lite
./install.sh
```

## Architecture

### Core Components

**EDS Class** ([src/cpp/lib/formats/eds.hpp](src/cpp/lib/formats/eds.hpp))
- Central data structure for elastic-degenerate strings
- Two storage modes: FULL (all in RAM) and METADATA_ONLY (streaming)
- Support for source tracking

**Transform Modules** ([src/cpp/lib/transforms/](src/cpp/lib/transforms/))
- **MSA Transforms**: MSA → EDS/l-EDS with source tracking
- **VCF Transforms**: VCF → EDS/l-EDS with sample-level sources. Supports SNPs, indels, deletions (`<DEL>`), insertions (`<INS>`), **inversions (`<INV>`)**, and **copy number variations (`<CN0>`, `<CN1>`, `<CN2>`, etc.)**
- **EDS Transforms**: EDS → l-EDS with LINEAR or CARTESIAN merging

### Design Patterns

**Streaming Architecture**: All transform functions use `std::istream&` and `std::ostream&` for memory-efficient processing of large files.

**Two-Phase Loading**: EDS files are parsed for metadata first, then optionally loaded fully or streamed on-demand.

**Separate Source Tracking**: Source information is stored separately (`.seds` files), allowing EDS usage without provenance overhead. The `Sources` class is **thread-safe for concurrent reads**: a mutex protects the shared file stream and LRU cache. `merge_adjacent_sources()` uses value-returning `read_source()` (not `read_source_ref()`) to avoid dangling references when called from multiple OpenMP threads.

**Per-Process Temp Directories**: `eds2leds` creates a PID-scoped temp directory (`/tmp/edsparser_leds_<pid>/`) for intermediate iteration files. This allows multiple `eds2leds` instances to run in parallel (e.g. from experiment scripts) without interfering with each other's temp files.

## Performance Characteristics

### Memory Efficiency

**MSA Transformations**:
- Memory: O(reference + bit vectors), independent of output size
- Streaming output: Symbols flushed incrementally (no output accumulation)
- Example: 1000 sequences × 100MB alignment ≈ 125MB peak memory
- Suitable for: Any realistic MSA size (10K+ sequences, 1GB+ alignments)

**VCF Transformations**:
- Block-based processing: Default 10M bases per block
- Memory: O(variants per block), not O(total variants)
- Typical reduction: 120GB → 6-8GB for large population VCF files
- Streaming reference: Random-access reading (not loaded into memory)
- Adjustable: Use `--block-size` parameter to control memory usage

**General**:
- **MSA Parsing**: O(n×m) where n=number of sequences, m=alignment length
- **VCF Processing**: Streaming reference genome (only current block in RAM)
- **l-EDS Generation**: Linear in number of symbols
- **Streaming Architecture**: All transforms write directly to disk (O(1) output memory)

## Documentation

- **Tool Help**: Run any tool with `--help` for detailed usage information

## Troubleshooting

### Build Errors

**Missing Boost:**
```bash
sudo apt-get install libboost-program-options-dev
```

**CMake version too old:**
```bash
# Install newer CMake from official website or snap
snap install cmake --classic
```

### Runtime Errors

**Tool not found:**
```bash
# Add installation directory to PATH
export PATH="$HOME/.local/bin:$PATH"

# Or use full path
~/.local/bin/msa2eds --help
```

**Out of memory:**
- Use streaming mode (METADATA_ONLY) for large files
- Process files in smaller chunks
- Use l-EDS transformation with smaller context lengths

## Acknowledgments

- CLAUDE CODE by Antrophic for supporting this project
