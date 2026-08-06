# CLI Tool Reference

All tools are installed to `~/.local/bin/` by `./INSTALL.sh`. Every tool:

- Accepts `--help` / `-h` for usage information.
- Writes runtime and peak memory to **stderr** on exit:
  ```
  [Performance] Runtime: 1.23s | Peak Memory: 45.6 MB
  ```
- Exits with code `0` on success, non-zero on error.

---

## msa2eds

Transform a Multiple Sequence Alignment (FASTA with gaps) to EDS or l-EDS.

### Synopsis

```
msa2eds -i <alignment.msa> [OPTIONS]
```

### Options

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `-i, --input` | path | required | Input MSA file in FASTA format (gaps as `-`) |
| `-o, --output` | path | `<stem>.eds` | Output EDS / l-EDS file |
| `-s, --seds` | path | `<stem>.seds` | Output source-tracking file |
| `-l, --context-length` | uint | — | If set, produces l-EDS instead of EDS |

### Behaviour

- The **first sequence** in the FASTA alignment is used as the reference.
- Each input sequence becomes **one source path** in the output `.seds`.
- Common columns → non-degenerate EDS symbol.
- Columns with variation → degenerate EDS symbol.
- When `-l` is provided, adjacent symbols are merged until every internal
  common segment is ≥ l characters (see [Algorithms](algorithms.md#msa-eds-three-pass-streaming-algorithm)).
- Merging strategy is always **LINEAR** (phasing-aware).

### Examples

```bash
# Produce EDS + sources
msa2eds -i data.msa

# Produce l-EDS (l=10) directly, custom output paths
msa2eds -i data.msa -l 10 -o data.l10.leds -s data.l10.seds

# Pipe from stdin (redirect)
msa2eds -i /dev/stdin < data.msa
```

### Output Files

| File | Description |
|------|-------------|
| `<stem>.eds` or `<stem>.leds` | EDS / l-EDS in compact format |
| `<stem>.seds` | Source tracking (one set of path IDs per string) |

---

## vcf2eds

Transform a VCF file (with reference FASTA) to EDS or l-EDS.

### Synopsis

```
vcf2eds -i <variants.vcf> --reference <genome.fasta> [OPTIONS]
```

### Options

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `-i, --input` | path | required | Input VCF file |
| `-r, --reference` | path | required | Reference FASTA (random-access streaming) |
| `-o, --output` | path | `<stem>.eds` | Output EDS / l-EDS file |
| `-s, --seds` | path | `<stem>.seds` | Output source-tracking file (text SEDS, sparse) |
| `-z, --edz` | flag | off | Write the source file in binary EDZ format instead of text SEDS (both sparse) |
| `-l, --context-length` | uint | — | If set, produces l-EDS |
| `--block-size` | uint | `10000000` | Genomic window size in bases; `0` = load all (legacy, high memory) |
| `--keep-eds` | flag | off | With `-l`, also write the intermediate stage-1 EDS/SEDS instead of discarding them (no-op without `-l`) |

> **`-z` is ignored in l-EDS mode.** The two-stage VCF→EDS→l-EDS pipeline writes dense text
> SEDS, so `-z` combined with `-l` prints a warning and falls back rather than producing a
> mislabelled file. The kept `--keep-eds` output has the same limitation for its SEDS; its
> `.eds` is byte-identical to a plain no-`-l` run.

> **Sources are sample-level, not haplotype-level.** Each diploid sample becomes one path,
> and phase is ignored (`0/1` is treated like `0|1`), so a heterozygous sample is recorded in
> *both* the reference and the alt string at a site. Traversing one path therefore does not
> reconstruct a single physical chromosome, and `num_paths` counts samples rather than
> haplotypes. This also drives l-EDS merge cost — see [performance.md](performance.md).

### Variant Support

| VCF ALT | Handling |
|---------|---------|
| SNP / small indel | Direct |
| `<DEL>` | Empty alternative |
| `<INS>` | Uses INFO/SEQ |
| `<INV>` | Reverse complement of REF |
| `<CN0>` | Empty (deletion) |
| `<CN1>` | REF string |
| `<CN2..N>` | Duplicated REF (N copies) |
| Multi-allelic | Each ALT → one alternative in same degenerate symbol |

Unsupported SVs and malformed lines are **skipped with warnings**; a
summary is printed to stderr on exit.

### Memory Tuning via `--block-size`

The default 10 Mbp block keeps peak memory to a few gigabytes on typical
population VCFs. Adjust based on available RAM:

```bash
# 16 GB server — smaller blocks, lower memory
vcf2eds -i 1000g.vcf -r hg38.fasta --block-size 1000000

# 256 GB server — larger blocks, faster I/O
vcf2eds -i 1000g.vcf -r hg38.fasta --block-size 100000000
```

Setting `--block-size 0` disables blocking and loads all variants into RAM
(suitable only for small VCF files).

### Examples

```bash
# Basic: VCF → EDS
vcf2eds -i variants.vcf -r reference.fasta

# VCF → l-EDS (l=5) with low memory
vcf2eds -i large.vcf -r hg38.fasta -l 5 --block-size 2000000

# Explicit output paths
vcf2eds -i variants.vcf -r ref.fasta -o out.eds -s out.seds

# Binary EDZ sources instead of text SEDS
vcf2eds -i variants.vcf -r ref.fasta -z
```

---

## eds2leds

Transform an EDS to l-EDS. Automatically selects merging strategy based on
whether a source file is provided.

### Synopsis

```
eds2leds -i <data.eds> -l <N> [OPTIONS]
```

### Options

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `-i, --input` | path | required | Input EDS file (must have `.eds` extension) |
| `-l, --context-length` | uint | required | Target minimum context length |
| `-o, --output` | path | `<stem>_l<N>.leds` | Output l-EDS file |
| `-s, --seds` | path | — | Input `.seds`/`.edz` source file (enables LINEAR merging); format auto-detected from extension/content |
| `-z, --edz` | path | — | Input source file explicitly treated as binary EDZ regardless of its extension (mutually exclusive with `-s`) |
| `--full` | flag | off | Force full bracket format (default: compact) |
| `-t, --threads` | int | `1` | Parallel worker threads |
| `--source-format` | enum | `seds` | Output sources format: `seds`, `seds-sparse`, `edz`, `edz-sparse`, `edz-compressed` |
| `--block-size` | size | — | Merge in blocks of ~N EDS bytes (`200M`, `1G`) so peak RAM is bounded by the block, not the file |
| `--max-memory` | size | — | Refuse the transform with **exit code 3** if the predicted peak exceeds this budget |
| `--estimate-memory` | flag | off | Print a machine-readable estimate and exit 0 without doing any work |

#### `--block-size` — bounding peak memory

Cuts land only where no merge can cross — a run of common symbols totalling ≥ `-l` — so the
output is **byte-identical** to a whole-file run, both `.leds` and `.seds`. Measured on a
949 MB / 31.7 M-symbol input at `-l 10`: whole file 2 380 MB, `200M` 1 071 MB, `50M` 644 MB,
`10M` 540 MB linear and **80 MB cartesian**. Cost is roughly 2.7× wall clock.

Cartesian block mode is a true ceiling, independent of file size. Linear mode floors at the
whole-file sources index (8 bytes per string), so blocks below ~50 MB stop helping. When no
barrier exists at a block boundary — variation too dense, or `-l` larger than every context —
it warns and falls back to a whole-file run.

#### `--source-format` — shrinking the output sources

Non-SEDS formats are produced by re-encoding the merged sources once at the end, so peak
*disk* still includes the dense text SEDS; only the final artifact shrinks. All EDZ variants
are written as `<output>.edz` and are self-describing via header flags. `edz-compressed`
requires a zstd-enabled build and is validated before the merge starts.

Pick by path count — the ranking inverts:

| Paths | SEDS | `gzip -6` of SEDS | `edz-sparse` | `edz-compressed` |
|---:|---:|---:|---:|---:|
| 100 | 440 KB | **58 KB** | 510 KB | — |
| 500 | 9.4 MiB | 2.9 MiB | 4.5 MiB | **2.0 MiB** |
| 2 504 | 16.3 MiB | — | 7.8 MiB | **2.7 MiB** |

At 100 paths sparse EDZ is *larger* than the text it replaces. EDZ only pays off from a few
hundred paths up, and the ratio improves as path count grows.

#### `--max-memory` / `--estimate-memory` — admission control

`--estimate-memory` emits `KEY=BYTES` lines — `MERGE_PEAK_BYTES`, `EDS_INDEX_BYTES`,
`SOURCES_INDEX_BYTES`, `RECOMMENDED_BUDGET_BYTES` (the one to schedule on), `MERGE_GROUPS`,
`PATH_CAP`, `SATURATED`, `BLOCK_SIZE_BYTES` — and honours `--block-size`.

> **Do not use these for admission control on heterozygous diploid VCF data.** The estimator
> caps each group at `num_paths`, which is invalid when sources are sample-level: on 1000G
> chr7 at l=5 it predicted 0.64 GiB against an actual peak of 28.3 GiB. It is calibrated and
> reliable on haploid or low-heterozygosity input.

### Auto-Detection of Merging Strategy

| Sources provided? | Strategy |
|:-----------------:|----------|
| Yes (`-s` or `-z`) | **LINEAR** — phasing-aware; only valid haplotype combinations kept |
| No | **CARTESIAN** — all combinations (cross-product of alternatives) |

**Prefer LINEAR** for real genomic data (MSA- or VCF-derived). CARTESIAN is
suitable when phasing is unknown or all combinations are needed.

### Output Files

| File | Description |
|------|-------------|
| `<stem>_l<N>.leds` | l-EDS in compact format (or full if `--full`) |
| `<stem>_l<N>.seds` | Updated source tracking (only when `-s` was given) |

### Complexity Warning

Before running the transformation, `eds2leds` estimates complexity via
`estimate_leds_complexity()`. If the EDS contains many adjacent degenerate
pairs, a warning is printed:

```
[WARNING] High transformation complexity detected.
  Adjacent degenerate pairs: 1204
  Recommendation: Consider reducing variability or increasing context length.
```

### Examples

```bash
# LINEAR merging (with sources, compact output)
eds2leds -i data.eds -s data.seds -l 5

# CARTESIAN merging (no sources)
eds2leds -i data.eds -l 10

# Full bracket format, 4 threads
eds2leds -i data.eds -s data.seds -l 10 --full --threads 4

# Explicit output path
eds2leds -i data.eds -l 20 -o custom_output.leds

# LINEAR merging with an EDZ source file that lacks a .edz extension
eds2leds -i data.eds -z data.sources -l 5
```

### Self-Overwrite Protection

If the output `.seds` path is the same file as the input `-s` path,
`eds2leds` writes to a temporary file first and renames atomically on
completion to avoid truncating the input.

---

## edsparser-stats

Display statistics about an EDS or l-EDS file, including l-EDS compliance
checking.

### Synopsis

```
edsparser-stats -i <data.eds> [OPTIONS]
```

### Options

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `-i, --input` | path | required | Input EDS / l-EDS file |
| `-s, --seds` | path | — | Companion `.seds`/`.edz` source file; format auto-detected from extension/content |
| `-z, --edz` | path | — | Companion source file explicitly treated as binary EDZ regardless of its extension (mutually exclusive with `-s`) |
| `-j, --json` | flag | off | Output in JSON format |
| `-c, --csv` | flag | off | Output in CSV format |
| `-v, --verbose` | flag | off | Show detailed statistics |

### Output

**Default (table) format:**
```
EDS Statistics:
  Symbols (n):        12543
  Strings (m):        38291
  Total chars (N):    1048576
  Degenerate symbols: 2714
  Common chars:       891200
  Min context:        5
  Max context:        127
  Avg context:        8.4
  Source paths:       4   (when -s provided)
  l-EDS compliant:    YES (l=5)
```

**JSON format** (`--json`):
```json
{
  "n": 12543,
  "m": 38291,
  "N": 1048576,
  "num_degenerate_symbols": 2714,
  "num_common_chars": 891200,
  "min_context_length": 5,
  "max_context_length": 127,
  "avg_context_length": 8.4
}
```

**CSV format** (`--csv`): One header row + one data row — suitable for
aggregating across multiple files with shell loops.

### Examples

```bash
# Table output
edsparser-stats -i data.leds

# JSON with sources
edsparser-stats -i data.eds -s data.seds --json

# CSV batch collection
for f in results/*.leds; do
  edsparser-stats -i "$f" --csv
done > all_stats.csv

# Verbose: adds memory estimates and compliance details
edsparser-stats -i data.leds --verbose
```

---

## edsparser-genpatterns

Extract random patterns from an EDS or l-EDS file for benchmarking.

Each pattern follows an actual path through the EDS, so it is guaranteed
to be present in any downstream tool that searches the same file.

### Synopsis

```
edsparser-genpatterns -i <data.eds> -o <patterns.txt> [OPTIONS]
```

### Options

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `-i, --input` | path | required | Input EDS / l-EDS file |
| `-o, --output` | path | required | Output patterns file (one per line) |
| `-n, --count` | uint | `100` | Number of patterns to generate |
| `-l, --length` | uint | `10` | Pattern length in characters |

### Output Format

Plain text, one pattern per line, DNA alphabet (ACGT):

```
ACGTACGTAC
TGCATGCATG
```

### Examples

```bash
# 100 patterns of length 10
edsparser-genpatterns -i data.leds -o p100_l10.txt -n 100 -l 10

# 1000 patterns of length 30
edsparser-genpatterns -i data.leds -o p1000_l30.txt -n 1000 -l 30
```

---

## genrandomeds

Generate a synthetic EDS file with controlled variability. Produces both
a `.eds` and a companion `.seds` file.

### Synopsis

```
genrandomeds --ref-size-mb <N> -o <output.eds> [OPTIONS]
```

### Options

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--ref-size-mb` | uint | required | Reference length in Mbp (1 MB = 1 000 000 bp) |
| `-o, --output` | path | required | Output EDS file (`.eds` or `.leds`) |
| `-v, --variability` | double | `0.10` | Fraction of positions with a variant (0–1) |
| `--min-alternatives` | uint | `2` | Min strings per degenerate symbol |
| `--max-alternatives` | uint | `4` | Max strings per degenerate symbol |
| `--variant-length-max` | uint | `10` | Maximum indel length in bp |
| `--snp-ratio` | double | `0.70` | Fraction of variants that are SNPs (rest are indels) |
| `--alphabet` | string | `"ACGT"` | Characters used for sequence generation |
| `--min-context` | uint | `0` | Minimum spacing between variants (0 = disabled; >0 = l-EDS compliant) |
| `--seed` | uint | random | Random seed for reproducibility |

### Behaviour

- Reference sequence is generated base-by-base using a random PRNG seeded
  from `--seed`.
- Variant structure (positions, types, lengths) uses a second independent
  PRNG seeded from `seed ^ 0x9e3779b9` to ensure the reference and variant
  streams do not correlate.
- **Round-robin haplotype assignment**: path `p` is assigned alternative
  `(p − 1) mod num_alts` at each degenerate symbol. This ensures every path
  passes through exactly one alternative per symbol, matching real phased
  data and preventing Cartesian explosion during linear l-EDS merging.
- Number of source paths = `max(max_alternatives, 3)`.
- Memory footprint is **O(1)** — symbols are streamed directly to disk.

### Variant Modes

| `--min-context` | Mode | Variant placement |
|:-:|---|---|
| `0` | Bernoulli | Per-position independent trial with probability `variability` |
| `>0` | Segment | One variant per segment of size `total_bp / expected_variants`; minimum spacing enforced |

### Examples

```bash
# Basic 100 MB dataset, 10% variability
genrandomeds --ref-size-mb 100 --variability 0.10 -o data.eds

# l-EDS compliant (min context = 5), reproducible
genrandomeds --ref-size-mb 50 --variability 0.05 --min-context 5 --seed 42 -o data.leds

# High variability, many alternatives
genrandomeds --ref-size-mb 10 --variability 0.20 \
  --min-alternatives 3 --max-alternatives 8 -o high_var.eds

# SNP-heavy dataset (90% SNPs)
genrandomeds --ref-size-mb 20 --snp-ratio 0.90 -o snp_heavy.eds
```

### Output Files

| File | Description |
|------|-------------|
| `output.eds` | The generated EDS |
| `output.seds` | Source tracking file with round-robin path assignments |

### Progress Output

Progress is written to stderr as a percentage:

```
Generating random EDS (streaming, O(1) memory):
  Reference size: 100 MB (100000000 bp)
  Variability: 10.0%
  Expected variant sites: 10000000
  Progress: 47.3%
```
