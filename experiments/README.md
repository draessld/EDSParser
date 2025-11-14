# EDSParser Experiments

This directory contains universal experiment scripts for transforming MSA, VCF, and EDS files into length-constrained EDS (l-EDS) formats.

## Available Scripts

- **[transform_to_eds.sh](transform_to_eds.sh)** - Transform MSA/VCF/EDS to EDS/l-EDS formats
- **[generate_patterns.sh](generate_patterns.sh)** - Generate random patterns from EDS files for benchmarking
- **[generate_statistics.sh](generate_statistics.sh)** - Generate detailed statistics from EDS/l-EDS files
- **[clean_experiments.sh](clean_experiments.sh)** - Universal cleanup script

## Dataset Structure

```
experiments/
├── transform_to_eds.sh         # Transform MSA/VCF/EDS to EDS/l-EDS
├── generate_patterns.sh        # Pattern generation script
├── generate_statistics.sh      # Statistics generation script
├── clean_experiments.sh        # Cleanup script
├── README.md                   # This file
└── datasets/
    └── <DATASET_NAME>/
        ├── <input_format>/     # Input: msa/, vcf/, eds/, etc.
        ├── eds/                # Output: EDS files + sources + logs
        │   └── patterns_*/     # Optional: patterns generated from EDS
        ├── 3_leds/             # Output: l-EDS (l=3) + sources + logs
        │   └── patterns_*/     # Optional: patterns generated from l-EDS
        ├── 5_leds/             # Output: l-EDS (l=5) + sources + logs
        ├── 10_leds/            # Output: l-EDS (l=10) + sources + logs
        ├── 15_leds/            # Output: l-EDS (l=15) + sources + logs
        ├── 20_leds/            # Output: l-EDS (l=20) + sources + logs
        └── statistics.csv      # Aggregated metrics
```

## Quick Start

### Transform to EDS/l-EDS

The **transform_to_eds.sh** script supports MSA, VCF, and EDS input formats:

```bash
# MSA experiment - all 32 SARS-CoV-2 variants
./transform_to_eds.sh --dataset SARS_cov2 --format msa

# MSA experiment - specific files only
./transform_to_eds.sh --dataset SARS_cov2 --format msa --pattern "20*"      # 20-series
./transform_to_eds.sh --dataset SARS_cov2 --format msa --pattern "*Delta*"  # Delta variants
./transform_to_eds.sh --dataset SARS_cov2 --format msa --pattern "21F_Iota" # Single file

# Custom length values
./transform_to_eds.sh --dataset SARS_cov2 --format msa --lengths 2,4,8,16

# VCF experiment (auto-detects reference if chr21.fasta exists)
./transform_to_eds.sh --dataset human_chr1 --format vcf

# VCF with explicit reference
./transform_to_eds.sh --dataset human_data --format vcf --reference genome.fasta

# VCF with custom lengths and pattern
./transform_to_eds.sh --dataset vcf_data --format vcf --pattern "chr*.vcf" --lengths 5,10,15

# EDS to l-EDS transformation
./transform_to_eds.sh --dataset precomputed --format eds
```

### Generate Patterns

Generate random patterns from EDS files for benchmarking:

```bash
# Generate 100 patterns of length 10 from all EDS files
./generate_patterns.sh --dataset SARS_cov2

# Generate 1000 patterns of length 20
./generate_patterns.sh --dataset SARS_cov2 --count 1000 --length 20

# Generate patterns from l-EDS files
./generate_patterns.sh --dataset SARS_cov2 --input-dir 3_leds --count 500 --length 15

# Generate from specific files
./generate_patterns.sh --dataset SARS_cov2 --pattern "20*"
```

**Output:** Patterns are organized in folders inside the input directory:
- `eds/patterns_100_10/` - 100 patterns of length 10 from EDS files
- `3_leds/patterns_100_10/` - 100 patterns of length 10 from l-EDS (l=3)
- `3_leds/patterns_1000_20/` - 1000 patterns of length 20 from l-EDS (l=3)
- Each pattern folder contains `.patterns` files (one per input file)

### Generate Statistics

Generate detailed statistics from EDS/l-EDS files:

```bash
# Display statistics for all EDS files (table format)
./generate_statistics.sh --dataset SARS_cov2

# Generate statistics from multiple directories
./generate_statistics.sh --dataset SARS_cov2 --input-dirs "eds,3_leds,5_leds,10_leds"

# Save statistics as JSON
./generate_statistics.sh --dataset SARS_cov2 --format json --output stats.json

# Generate CSV statistics (requires jq)
./generate_statistics.sh --dataset SARS_cov2 --format csv --output stats.csv

# Use FULL mode for detailed statistics
./generate_statistics.sh --dataset SARS_cov2 --full

# Generate statistics from specific files
./generate_statistics.sh --dataset SARS_cov2 --pattern "20*"
```

**Output Formats:**
- `table` - Human-readable format (default, displays to stdout)
- `json` - JSON format (one object per file, ideal for programmatic processing)
- `csv` - CSV format (one row per file, requires `jq` for full parsing)

**Key Statistics:**
- File size and storage mode
- Number of symbols, characters, and strings
- Context length statistics (min, max, average)
- Variation metrics (change size, common characters)
- Memory usage (current vs. estimated full mode)
- Source tracking information (number of paths/genomes)

### Clean Up Experiments

Remove all generated outputs while preserving input data:

```bash
# Clean all datasets (dry run - preview only)
./clean_experiments.sh --dry-run

# Clean all datasets (with confirmation)
./clean_experiments.sh --interactive

# Clean specific dataset
./clean_experiments.sh SARS_cov2

# Clean all datasets (no confirmation)
./clean_experiments.sh
```

## Command-Line Options

### transform_to_eds.sh

```bash
./transform_to_eds.sh --dataset DATASET --format FORMAT [OPTIONS]

Required:
  --dataset DATASET   Dataset name (e.g., "SARS_cov2")
  --format FORMAT     Input format: "msa", "vcf", or "eds"

Options:
  --input-dir DIR     Input directory name (default: same as format)
  --pattern PATTERN   File pattern to process (default: "*")
  --lengths L1,L2,... Length values for l-EDS (default: "3,5,10,15,20")
  --reference FILE    Reference FASTA for VCF (auto-detected if <vcf_name>.{fasta,fa,fna} exists)
  --force             Overwrite existing output files
  --no-stats          Don't generate statistics.csv
  -h, --help          Show help message
```

### generate_patterns.sh

```bash
./generate_patterns.sh --dataset DATASET [OPTIONS]

Required:
  --dataset DATASET   Dataset name (e.g., "SARS_cov2")

Options:
  --input-dir DIR     Input directory name (default: "eds")
  --pattern PATTERN   File pattern to process (default: "*")
  --count N           Number of patterns per file (default: 100)
  --length L          Pattern length (default: 10)
  --force             Overwrite existing pattern files
  -h, --help          Show help message
```

### generate_statistics.sh

```bash
./generate_statistics.sh --dataset DATASET [OPTIONS]

Required:
  --dataset DATASET   Dataset name (e.g., "SARS_cov2")

Options:
  --input-dirs DIRS   Comma-separated list of input directories (default: "eds")
                      Examples: "eds", "3_leds,5_leds", "eds,3_leds,5_leds,10_leds"
  --pattern PATTERN   File pattern to process (default: "*")
  --format FORMAT     Output format: "table", "json", or "csv" (default: "table")
  --output FILE       Save output to file (default: stdout)
  --full              Use FULL mode (loads all strings, more detailed)
  --force             Overwrite existing output file
  -h, --help          Show help message
```

### clean_experiments.sh

```bash
./clean_experiments.sh [OPTIONS] [DATASET]

Arguments:
  DATASET             Optional: specific dataset to clean (e.g., "SARS_cov2")
                      If omitted, cleans all datasets

Options:
  -d, --dry-run       Show what would be deleted without deleting
  -i, --interactive   Ask for confirmation before deleting
  -h, --help          Show help message
```

## Configuration

All configuration is done via command-line arguments to `transform_to_eds.sh`. Common options:

- **Length values**: Use `--lengths 3,5,10,15,20` (default: 3,5,10,15,20)
- **File pattern**: Use `--pattern "*.msa"` to filter files (default: "*")
- **Input directory**: Use `--input-dir custom_name` if not using standard names (msa/vcf/eds)
- **Force overwrite**: Use `--force` to regenerate existing outputs
- **Statistics**: Use `--no-stats` to skip CSV generation

## Output Files

For each input MSA file (e.g., `21F_Iota.msa`), the following outputs are generated:

### EDS Directory
- `21F_Iota.eds` - Elastic-Degenerate String
- `21F_Iota.seds` - Source tracking file (maps strings to sequences)
- `21F_Iota.eds.log` - Transformation log (runtime, memory, etc.)

### l-EDS Directories (one per length value)
- `21F_Iota.leds` - Length-constrained EDS
- `21F_Iota.seds` - Source tracking file
- `21F_Iota.leds.log` - Transformation log

### Statistics CSV

Contains metrics for all transformations:

| Column | Description |
|--------|-------------|
| variant | Variant name (e.g., 21F_Iota) |
| input_size_bytes | Original MSA file size |
| eds_size_bytes | EDS file size |
| seds_size_bytes | EDS sources file size |
| eds_time_sec | EDS transformation time |
| l3_size_bytes | l-EDS (l=3) file size |
| l3_seds_size_bytes | l-EDS (l=3) sources size |
| l3_time_sec | l-EDS (l=3) transformation time |
| ... | (repeated for l=5, 10, 15, 20) |

## Examples

### Example 1: Full Experiment Run

```bash
# Process all 32 SARS-CoV-2 variants with default l-values (3,5,10,15,20)
./transform_to_eds.sh --dataset SARS_cov2 --format msa

# Output:
# - 32 EDS files + sources + logs
# - 32 × 5 = 160 l-EDS files + sources + logs
# - 1 statistics.csv with 33 rows (header + 32 data rows)
```

### Example 2: Experiment with Different Length Values

```bash
# Clean previous outputs
./clean_experiments.sh SARS_cov2

# Rerun with custom length values
./transform_to_eds.sh --dataset SARS_cov2 --format msa --lengths 2,4,8,16
```

### Example 3: Analyze Specific Lineage Evolution

Process chronologically related variants:
```bash
# Alpha variant
./transform_to_eds.sh --dataset SARS_cov2 --format msa --pattern "20I_Alpha"

# Delta variants
./transform_to_eds.sh --dataset SARS_cov2 --format msa --pattern "*Delta*"

# Compare file sizes in statistics.csv
head -1 datasets/SARS_cov2/statistics.csv
grep -E "(20I_Alpha|Delta)" datasets/SARS_cov2/statistics.csv
```

### Example 4: Force Regeneration

Regenerate all outputs (useful after tool updates):
```bash
./transform_to_eds.sh --dataset SARS_cov2 --format msa --force
```

## Understanding the Logs

Each `.log` file contains transformation metadata:

```
MSA → l-EDS transformation (l=3)
  Input: "datasets/SARS_cov2/msa/21F_Iota.msa"
Transformation complete!
  Output: "datasets/SARS_cov2/3_leds/21F_Iota.leds"
  Sources: "datasets/SARS_cov2/3_leds/21F_Iota.seds"
[Performance] Runtime: 0.00s | Peak Memory: 4.2 MB
```

## Dataset Information

The SARS-CoV-2 dataset contains 32 MSA files representing major lineages:

- **19 Series** (2 files): Early pandemic lineages (19A, 19B)
- **20 Series** (11 files): First-wave variants including Alpha, Beta, Gamma
- **21 Series** (10 files): Delta variants and predecessors
- **22 Series** (6 files): Early Omicron variants (BA.1 through BQ.1)
- **23 Series** (3 files): Recent Omicron subvariants (XBB.1.5, XBB.1.16)

File sizes range from 59 KB (21F_Iota) to 308 KB (22C_BA.2.12.1).

## Troubleshooting

### "Tool not found" Error

Ensure EDSParser tools are installed:
```bash
cd ..  # Go to project root
./INSTALL.sh
```

### Empty Statistics

Check if transformations succeeded:
```bash
# View logs for errors
cat datasets/SARS_cov2/eds/*.log
cat datasets/SARS_cov2/3_leds/*.log
```

### Memory Issues (Large MSA Files)

The `msa2eds` tool uses streaming architecture and should handle large files efficiently. Check logs for peak memory usage.

## Technical Details

### MSA Transformation

- **Algorithm**: Streaming MSA parser with source tracking
- **Memory**: Only reference sequence kept in RAM
- **Merging**: LINEAR (phasing-aware) by default - preserves valid haplotype combinations
- **Source Tracking**: Maps each string to originating sequence ID

### File Format Details

- **EDS Format**: `{str1,str2}{str3}{str4,str5,str6}...`
- **SEDS Format**: Text file mapping string IDs to sequence IDs
- **l-EDS**: EDS with minimum context length l (adjacent symbols merged until constraint met)

## Cleanup Script Details

The **clean_experiments.sh** script intelligently removes only generated outputs:

### What Gets Deleted
- EDS output directories (`eds/`, `leds/`) - including any pattern folders inside
- All l-EDS directories (`3_leds/`, `5_leds/`, `10_leds/`, etc.) - including any pattern folders inside
- Statistics files (`statistics.csv`, `*.csv`)
- Log files within output directories (`*.log`)

### What Is Preserved
- Input data directories (`msa/`, `vcf/`, `raw/`, `input/`, etc.)
- All scripts (`*.sh`, `*.py`)
- Documentation (`*.md`, `*.txt`)
- Source code (`*.cpp`, `*.hpp`)

### Usage Examples

```bash
# Preview what would be deleted (safe, no changes)
./clean_experiments.sh --dry-run

# Clean with confirmation prompt
./clean_experiments.sh --interactive

# Clean specific dataset
./clean_experiments.sh SARS_cov2

# Clean all datasets silently
./clean_experiments.sh
```

The script uses pattern matching to identify directories, so it works universally with any dataset structure (`msa/`, `vcf/`, `eds/`, etc.) and any l-EDS naming convention (`3_leds/`, `10_leds/`, `custom_leds/`, etc.).

## Workflow Example

Complete workflow for running experiments and cleaning up:

```bash
# 1. Run full experiment
./transform_to_eds.sh --dataset SARS_cov2 --format msa

# 2. Analyze results
cat datasets/SARS_cov2/statistics.csv
ls -lh datasets/SARS_cov2/eds/

# 3. Try different length values
./clean_experiments.sh SARS_cov2
./transform_to_eds.sh --dataset SARS_cov2 --format msa --lengths 2,4,8

# 4. Final cleanup (preview first)
./clean_experiments.sh --dry-run SARS_cov2
./clean_experiments.sh SARS_cov2
```
