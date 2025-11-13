# EDSParser

A high-performance C++ library for parsing and transforming **Elastic-Degenerate Strings (EDS)**, a data structure for representing sequence variation in bioinformatics.

## Features

- **Multiple Input Formats**: MSA (Multiple Sequence Alignment), VCF (Variant Call Format), and native EDS
- **Format Transformations**: Convert between formats and produce length-constrained EDS (l-EDS)
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
│   ├── tools/                  # Command-line tools
│   │   ├── msa2eds             # MSA → EDS/l-EDS
│   │   ├── vcf2eds             # VCF → EDS/l-EDS
│   │   ├── eds2leds            # EDS → l-EDS
│   │   ├── edsparser-stats     # Statistics tool
│   │   └── edsparser-genpatterns  # Pattern generation tool
│   └── test/                   # Unit tests
├── experiments/                # Experiment scripts
│   ├── transform_to_eds.sh     # Transform MSA/VCF/EDS → EDS/l-EDS
│   ├── generate_patterns.sh   # Pattern generation wrapper
│   ├── generate_statistics.sh  # Statistics generation wrapper
│   ├── clean_experiments.sh    # Cleanup utility
│   └── datasets/               # Example datasets
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
```

**Features:**
- Handles SNPs, indels, insertions, deletions
- Multi-allelic site support
- Sample-level source tracking
- Streaming reference processing

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

# Full mode (load all strings)
edsparser-stats -i data.eds --full
```

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
edsparser-genpatterns -i data.eds -o patterns.txt -c 100 -l 10

# Generate 1000 patterns of length 20
edsparser-genpatterns -i data.eds -o patterns.txt -c 1000 -l 20

# From l-EDS files
edsparser-genpatterns -i data.leds -o patterns.txt -c 500 -l 15
```

**Options:**
- `-i, --input` - Input EDS/l-EDS file
- `-o, --output` - Output pattern file
- `-c, --count` - Number of patterns to generate (default: 100)
- `-l, --length` - Pattern length (default: 10)

**Output:**
Plain text file with one pattern per line (ACGT alphabet).

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
- Both formats are supported for reading; output format controlled by `--compact` flag

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

The `experiments/` directory provides automated scripts for batch processing:

```bash
cd experiments

# Run full experiment on SARS-CoV-2 dataset (32 MSA files)
./run_experiment.sh --dataset SARS_cov2 --format msa

# Custom length values
./run_experiment.sh --dataset SARS_cov2 --format msa --lengths 3,5,10,15,20

# Process specific files
./run_experiment.sh --dataset SARS_cov2 --format msa --pattern "20*"

# Clean up generated outputs
./clean_experiments.sh SARS_cov2
```

See [experiments/README.md](experiments/README.md) for detailed documentation.

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

```bash
cd build/src/cpp

# Run all tests
ctest --output-on-failure

# Run specific test
./test_eds
./test_msa
./test_vcf
```

Available tests:
- `test_eds` - EDS parsing and operations
- `test_sources` - Source tracking
- `test_stats` - Statistics computation
- `test_merge` - Symbol merging algorithms
- `test_transform` - EDS transformations
- `test_msa` - MSA parsing
- `test_vcf` - VCF parsing

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
- **VCF Transforms**: VCF → EDS/l-EDS with sample-level sources
- **EDS Transforms**: EDS → l-EDS with LINEAR or CARTESIAN merging

### Design Patterns

**Streaming Architecture**: All transform functions use `std::istream&` and `std::ostream&` for memory-efficient processing of large files.

**Two-Phase Loading**: EDS files are parsed for metadata first, then optionally loaded fully or streamed on-demand.

**Separate Source Tracking**: Source information is stored separately (`.seds` files), allowing EDS usage without provenance overhead.

## Performance Characteristics

- **MSA Parsing**: O(n×m) where n=number of sequences, m=alignment length
- **Memory Usage**: Linear in output size (streaming mode uses constant memory)
- **VCF Processing**: Streaming reference genome (only current position in RAM)
- **l-EDS Generation**: Linear in number of symbols

## Documentation

- **[experiments/README.md](experiments/README.md)** - Experiment framework documentation
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
