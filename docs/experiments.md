# Experiments Framework

The `experiments/` directory provides a complete batch-processing framework
for running MSA, VCF, and EDS transformations at scale, collecting
statistics, and benchmarking performance.

---

## Directory Layout

```
experiments/
├── transform_to_eds.sh         # Main transformation driver
├── generate_random_dataset.sh  # Synthetic dataset generator
├── generate_patterns.sh        # Pattern generation for benchmarking
├── generate_statistics.sh      # Aggregate statistics collection
├── run_synthetic_test.sh       # End-to-end synthetic test workflow
├── clean_experiments.sh        # Remove generated outputs
├── common.sh                   # Shared shell helpers
├── README.md                   # Full experiments documentation
└── datasets/
    └── <DATASET_NAME>/
        ├── msa/  (or vcf/ or eds/)   # Input data
        ├── eds/                       # Generated EDS + SEDS + logs
        │   └── patterns_N_L/         # Patterns subfolders
        ├── 3_leds/                    # l-EDS (l=3) + SEDS + logs
        ├── 5_leds/
        ├── 10_leds/
        ├── 15_leds/
        ├── 20_leds/
        └── statistics.csv            # Aggregated metrics
```

---

## Dataset Path Conventions

All scripts accept either:
- A **dataset name**: `--dataset SARS_cov2` → `experiments/datasets/SARS_cov2/`
- An **absolute or relative path**: `--dataset /data/experiments/my_dataset`

---

## transform_to_eds.sh

The main transformation driver. Processes all input files in a dataset and
produces EDS + l-EDS at multiple context lengths.

### Usage

```bash
./transform_to_eds.sh --dataset DATASET --format FORMAT [OPTIONS]
```

### Required Arguments

| Argument | Values | Description |
|----------|--------|-------------|
| `--dataset` | name or path | Target dataset |
| `--format` | `msa`, `vcf`, or `eds` | Input format |

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `--input-dir DIR` | same as format | Override the input subdirectory name |
| `--pattern GLOB` | `*` | File glob to select inputs |
| `--lengths L1,L2,...` | `3,5,10,15,20` | l-EDS context lengths to generate |
| `--reference FILE` | auto-detected | Reference FASTA for VCF (see below) |
| `--force` | off | Overwrite existing outputs |
| `--no-stats` | off | Skip `statistics.csv` generation |

### VCF Reference Auto-Detection

If `--reference` is not provided, the script searches for:
1. `datasets/DATASET/vcf/<basename>.{fasta,fa,fna}` (same directory as VCF)
2. `datasets/DATASET/ref/<basename>.{fasta,fa,fna}` (dataset ref/ directory)

### Examples

```bash
# MSA dataset — all files, default lengths 3,5,10,15,20
./transform_to_eds.sh --dataset SARS_cov2 --format msa

# MSA — specific variants only
./transform_to_eds.sh --dataset SARS_cov2 --format msa --pattern "*Delta*"

# Custom lengths
./transform_to_eds.sh --dataset SARS_cov2 --format msa --lengths 2,4,8,16

# VCF with auto-detected reference
./transform_to_eds.sh --dataset human_chr1 --format vcf

# VCF with explicit reference
./transform_to_eds.sh --dataset my_data --format vcf --reference /data/hg38.fasta

# EDS → l-EDS (input already in EDS format)
./transform_to_eds.sh --dataset precomputed --format eds

# Force regeneration after tool update
./transform_to_eds.sh --dataset SARS_cov2 --format msa --force
```

### Output Files

For each input file `<name>.<ext>`:

| Output | Location | Description |
|--------|----------|-------------|
| `<name>.eds` | `eds/` | EDS in compact format |
| `<name>.seds` | `eds/` | Source tracking |
| `<name>.eds.log` | `eds/` | Transformation log with runtime + memory |
| `<name>.leds` | `<l>_leds/` | l-EDS (for each l value) |
| `<name>.seds` | `<l>_leds/` | Updated source tracking |
| `<name>.leds.log` | `<l>_leds/` | Transformation log |
| `statistics.csv` | dataset root | Aggregated size and timing metrics |

### statistics.csv Schema

| Column | Description |
|--------|-------------|
| `variant` | Input file stem (e.g., `21F_Iota`) |
| `input_size_bytes` | Original input file size |
| `eds_size_bytes` | EDS file size |
| `seds_size_bytes` | EDS sources file size |
| `eds_time_sec` | EDS transformation time |
| `l3_size_bytes` | l-EDS (l=3) file size |
| `l3_seds_size_bytes` | l-EDS (l=3) sources size |
| `l3_time_sec` | l-EDS (l=3) transformation time |
| … | Repeated for each l value |

### Log Format

Each `.log` file contains:
```
MSA → l-EDS transformation (l=3)
  Input: "datasets/SARS_cov2/msa/21F_Iota.msa"
Transformation complete!
  Output: "datasets/SARS_cov2/3_leds/21F_Iota.leds"
  Sources: "datasets/SARS_cov2/3_leds/21F_Iota.seds"
[Performance] Runtime: 0.12s | Peak Memory: 45.3 MB
```

---

## generate_random_dataset.sh

Generate synthetic EDS datasets for benchmarking and testing.

### Usage

```bash
./generate_random_dataset.sh --dataset DATASET [OPTIONS]
```

### Dataset Parameters

| Option | Default | Description |
|--------|---------|-------------|
| `--num-files N` | `10` | Number of EDS files to generate |
| `--ref-size-mb N` | `1` | Reference size per file in Mbp |
| `--variability FRAC` | `0.10` | Fraction of positions with variants |

### Variant Parameters

| Option | Default | Description |
|--------|---------|-------------|
| `--min-alternatives N` | `2` | Min alternatives per degenerate symbol |
| `--max-alternatives N` | `4` | Max alternatives per degenerate symbol |
| `--variant-length-max N` | `10` | Max indel length in bp |
| `--snp-ratio FRAC` | `0.70` | Fraction of SNPs vs indels |
| `--alphabet STR` | `"ACGT"` | Sequence alphabet |

### l-EDS Parameters

| Option | Default | Description |
|--------|---------|-------------|
| `--min-context N` | `0` | Min context between variants (0 = disabled) |
| `--lengths L1,L2,...` | `3,5,10,15,20` | l-EDS lengths to generate |
| `--no-leds` | off | Skip l-EDS generation |

### Other Options

| Option | Description |
|--------|-------------|
| `--seed N` | First file seed (incremented per file for reproducibility) |
| `--force` | Overwrite existing files |

### Examples

```bash
# Small test: 5 files × 2 MB
./generate_random_dataset.sh --dataset test_small --num-files 5 --ref-size-mb 2

# Medium benchmark: 20 files × 5 MB, 20% variability
./generate_random_dataset.sh --dataset bench_medium \
  --num-files 20 --ref-size-mb 5 --variability 0.20

# l-EDS compliant from generation (min-context=10)
./generate_random_dataset.sh --dataset leds_native \
  --min-context 10 --lengths 10,20,30

# Reproducible dataset, custom variant structure
./generate_random_dataset.sh --dataset repro42 \
  --seed 42 --num-files 10 --min-alternatives 3 --max-alternatives 6

# Raw EDS only (no l-EDS transformation)
./generate_random_dataset.sh --dataset raw_only --no-leds
```

---

## generate_patterns.sh

Generate random patterns from EDS or l-EDS files for downstream benchmarks.

### Usage

```bash
./generate_patterns.sh --dataset DATASET [OPTIONS]
```

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `--input-dir DIR` | `eds` | Input directory |
| `--input-dirs D1,D2,...` | — | Multiple input directories |
| `--pattern GLOB` | `*` | File glob |
| `--count N` | `100` | Patterns per file |
| `--length L` | `10` | Pattern length |
| `--counts N1,N2,...` | — | Multiple counts (batch) |
| `--lengths L1,L2,...` | — | Multiple lengths (batch) |
| `--force` | off | Overwrite existing patterns |

### Batch Generation

Specifying multiple counts/lengths generates all combinations:

```bash
# 4 output folders: 100×10, 100×20, 500×10, 500×20
./generate_patterns.sh --dataset SARS_cov2 \
  --input-dirs eds,3_leds --counts 100,500 --lengths 10,20
```

### Output Structure

```
datasets/SARS_cov2/
  eds/
    patterns_100_10/
      21F_Iota.patterns
      21J_Delta.patterns
      ...
    patterns_500_20/
      ...
  3_leds/
    patterns_100_10/
      ...
```

---

## generate_statistics.sh

Compute detailed EDS statistics for all files in a dataset and save to CSV
or JSON.

### Usage

```bash
./generate_statistics.sh --dataset DATASET [OPTIONS]
```

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `--input-dirs DIRS` | `eds` | Comma-separated directories |
| `--pattern GLOB` | `*` | File glob |
| `--format FORMAT` | `table` | `table`, `json`, or `csv` |
| `--output FILE` | auto | Custom output filename |
| `--full` | off | Load all strings (more detailed output) |
| `--force` | off | Overwrite existing files |

### Output Naming

Without `--output`: one file per directory saved in the dataset root:
```
datasets/SARS_cov2/eds_statistics.csv
datasets/SARS_cov2/3_leds_statistics.csv
```

With `--output`: all directories merged into one file.

### Example: Full Statistics Sweep

```bash
./generate_statistics.sh --dataset SARS_cov2 \
  --input-dirs "eds,3_leds,5_leds,10_leds,15_leds,20_leds" \
  --format csv
```

---

## clean_experiments.sh

Remove generated outputs while preserving input data.

### Usage

```bash
./clean_experiments.sh [OPTIONS] [DATASET]
```

### What Is Deleted

- EDS output directories (`eds/`, `leds/`) including pattern subfolders
- All l-EDS directories (`<N>_leds/`) including pattern subfolders
- Statistics files (`statistics.csv`, `*_statistics.csv`)
- Log files in output directories

### What Is Preserved

- Input data (`msa/`, `vcf/`, `raw/`, `input/`, etc.)
- Scripts (`*.sh`, `*.py`)
- Documentation (`*.md`, `*.txt`)

### Options

| Option | Description |
|--------|-------------|
| `--dry-run` | Show what would be deleted without deleting |
| `--interactive` | Prompt for confirmation before each deletion |

---

## run_synthetic_test.sh

End-to-end smoke test: generate a synthetic dataset, transform to EDS and
l-EDS, generate patterns, and validate that patterns are found. Useful for
CI and quick sanity checks.

```bash
./run_synthetic_test.sh
```

---

## Benchmarks

### bench.sh

```bash
bash tests/bench/bench.sh [--size quick|standard|large]
```

Scenarios:
- **genrandomeds**: EDS generation across multiple sizes
- **eds2leds cartesian**: Size sweep for cartesian merging
- **eds2leds linear**: Size sweep for linear merging
- **edsparser-stats**: Statistics computation performance
- **edsparser-genpatterns**: Pattern generation performance
- **Sweep: variability** (standard/large only): Vary variant density
- **Sweep: context length** (standard/large only): Vary l value
- **Sweep: path count** (standard/large only): Vary number of haplotypes

### Output

Results: `tests/bench/results/YYYY-MM-DD_HH-MM-SS.csv`

| Column | Description |
|--------|-------------|
| `timestamp` | ISO timestamp |
| `preset` | `quick`, `standard`, or `large` |
| `scenario` | Scenario name |
| `tool` | Tool name |
| `input_size_mb` | Input EDS size in MB |
| `runtime_s` | Wall-clock runtime in seconds |
| `peak_memory_mb` | Peak RSS in MB |
| `throughput_mb_s` | Input MB / runtime_s |

### bench_compare.sh

```bash
bash tests/bench/bench_compare.sh
```

Compares the latest results against `baseline.csv`. Reports regressions
where throughput dropped > 20% or memory grew > 20%.

**Promoting a new baseline** after an intentional performance change:
```bash
cp tests/bench/results/$(ls -t tests/bench/results/*.csv | head -1) \
   tests/bench/baseline.csv
```

### bench_plot.py

```bash
python3 tests/bench/bench_plot.py tests/bench/results/LATEST.csv
```

Requires `matplotlib` and `pandas`. Generates:
- `size_sweep.png` — throughput vs input size
- `summary.png` — side-by-side memory and throughput
- (for standard/large) `variability_sweep.png`, `context_length_sweep.png`,
  `path_count_sweep.png`

Plots saved to `tests/bench/results/plots/<timestamp>/`.

---

## Typical Full Workflow

```bash
# 1. Generate or place input data
./generate_random_dataset.sh --dataset my_test --num-files 5 --ref-size-mb 10

# — or bring real MSA data —
mkdir -p experiments/datasets/real_data/msa
cp *.msa experiments/datasets/real_data/msa/

# 2. Transform
./transform_to_eds.sh --dataset my_test --format eds   # (synthetic)
# or
./transform_to_eds.sh --dataset real_data --format msa

# 3. Generate patterns for downstream FM-index benchmarks
./generate_patterns.sh --dataset my_test --input-dirs "eds,5_leds" \
  --counts 100,1000 --lengths 10,20

# 4. Inspect statistics
./generate_statistics.sh --dataset my_test \
  --input-dirs "eds,3_leds,5_leds,10_leds" --format csv

# 5. Run performance benchmarks
bash tests/bench/bench.sh --size standard
bash tests/bench/bench_compare.sh

# 6. Clean up when done
./clean_experiments.sh --dry-run my_test   # preview
./clean_experiments.sh my_test             # execute
```
