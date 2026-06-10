# File Format Reference

This document describes every file format used or produced by EDSParser.

---

## EDS — Elastic-Degenerate String (`.eds`, `.leds`)

### Grammar

An EDS file consists of a sequence of *symbols*, each represented as a
brace-delimited set of alternative strings:

```
<EDS>          ::= <symbol>+
<symbol>       ::= '{' <alternatives> '}'
<alternatives> ::= <string> (',' <string>)*
<string>       ::= [ACGT]*          ; may be empty (represents deletion)
```

A **common (non-degenerate) symbol** has exactly one alternative.
A **degenerate symbol** has two or more alternatives.

### Output Format Variants

EDSParser supports two syntactic representations of the same EDS.

**Full format** — brackets on every symbol (including common ones):

```
{ACGT}{A,ACA}{CGT}{T,TG,}
```

**Compact format** (default) — brackets only on degenerate symbols:

```
ACGT{A,ACA}CGT{T,TG,}
```

Both formats are accepted on input and treated identically. Tools output
compact by default; use `--full` to force full format.

### l-EDS Constraint

An l-EDS is an EDS where every **internal** non-degenerate segment — one that
has a degenerate symbol on both its left and right — has length ≥ l.

*Boundary* segments (before the first or after the last degenerate symbol)
may legitimately be shorter and are not validated.

### Empty Strings

An empty alternative (deletion) is written as nothing between two commas, or
between an opening brace and the first comma:

```
{A,ACA,}        ; third alternative is empty (deletion)
{,ACGT}         ; first alternative is empty
```

### File Extension Conventions

| Extension | Meaning |
|-----------|---------|
| `.eds` | Regular EDS (no l constraint enforced) |
| `.leds` | l-EDS (minimum context length guaranteed) |

Both are valid EDS syntax — the extension is only a hint about the constraint.

---

## SEDS — Source EDS (`.seds`)

### Purpose

An SEDS file records *provenance* — for each string in the EDS, which
source paths (haplotypes / samples) contain that string.

### Format

An SEDS file is a flat sequence of sets, one per string (not per symbol),
in *global string order*: all strings from symbol 0 (in alternative order),
then all strings from symbol 1, and so on.

Each set is written as a brace-enclosed, comma-separated list of 1-based
integer path IDs:

```
{1,2,3}{1}{2,3}{1,2,3}
```

If a set contains `{0}` it is the **universal marker** meaning
"all paths traverse this string". This is typically used for common symbols.

### Example

Given the EDS `ACGT{A,ACA}CGT` with 3 haplotypes:

| String index | String | SEDS entry | Meaning |
|:---:|--------|------------|---------|
| 0 | `ACGT` | `{0}` | All 3 paths |
| 1 | `A` | `{1,3}` | Paths 1 and 3 carry `A` |
| 2 | `ACA` | `{2}` | Path 2 carries `ACA` |
| 3 | `CGT` | `{0}` | All 3 paths |

Resulting SEDS file: `{0}{1,3}{2}{0}`

### Cardinality Validation

When loading an EDS + SEDS pair, EDSParser validates that the number of
`{...}` entries in the SEDS file equals the EDS cardinality `m`. A mismatch
throws `std::invalid_argument` early.

### Special Marker: `{0}`

`{0}` is **not** path ID 0; it is the universal "all paths" marker.
Real path IDs are 1-indexed. When performing source intersections:

```
{0} ∩ {0}      = {0}        (all paths)
{0} ∩ {1,2,3}  = {1,2,3}   (concrete paths win)
{1,2} ∩ {2,3}  = {2}        (standard intersection)
{1,2} ∩ {3,4}  = ∅           → invalid combination, filtered out
```

### Format Variants (Planned)

| Extension | Format | Status |
|-----------|--------|--------|
| `.seds` | Text, human-readable | ✅ Implemented |
| `.edz` | Binary, varint encoding | 🔲 Planned |
| `.edz` (compressed) | Binary + zstd blocks | 🔲 Planned |

---

## MSA — Multiple Sequence Alignment (`.msa`)

Standard FASTA format where **all sequences are the same length** and gaps
are represented by the `-` character.

```
>sequence_1
ACGT-TAG
>sequence_2
ACGTATAG
>sequence_3
ACGT--AG
```

**Rules:**
- Gap character: `-` (hyphen/dash)
- All sequences must be the same length (aligned)
- First sequence is treated as the reference
- Header lines start with `>`
- Sequence data may span multiple lines

**Conversion notes:**
- Each input sequence becomes one path in the output SEDS
- Columns that are identical across all sequences → common symbol
- Columns with at least one difference → degenerate symbol
- Consecutive gap-only columns in a sequence contribute to a deletion alternative

---

## VCF — Variant Call Format (`.vcf`)

Standard VCF v4.x format. EDSParser requires a companion reference FASTA
file; the reference is accessed via random-read streaming (not loaded into
RAM).

### Supported Variant Types

| VCF ALT | Meaning | EDSParser handling |
|---------|---------|-------------------|
| Single nucleotide (SNP) | e.g. `A→G` | Direct substitution |
| Multi-nucleotide / small indel | e.g. `ACG→A` | Direct substitution |
| `<DEL>` | Deletion of REF bases | Empty alternative |
| `<INS>` | Insertion | Bases from INFO/SEQ field |
| `<INV>` | Inversion | Reverse complement of REF |
| `<CN0>` | 0 copies (deletion) | Empty alternative |
| `<CN1>` | 1 copy (reference) | REF string |
| `<CN2>`, `<CN3>`, … | Duplication | Repeated REF string |
| Multi-allelic | Multiple ALT values | Each ALT → one alternative |

### Skipped / Unsupported

The following are skipped with a warning written to stderr:

- Overlapping variants (resolved by the merge-overlap step)
- Translocations, mobile element insertions, and other complex SVs
- Malformed VCF lines (missing mandatory fields)

Statistics on skipped variants are returned in the `VCFStats` struct.

### Block-Based Processing

Large VCF files are processed in genomic windows (default 10 Mbp per block).
See [performance.md](performance.md#vcf-block-based-processing) for details.

---

## EDP — EDS Patterns (`.patterns`)

Simple plain-text format output by `edsparser-genpatterns`:

```
ACGTACGTAC
TGCATGCATG
AAACCCGGGT
```

One pattern per line, DNA alphabet only (ACGT). Patterns are drawn from
actual paths through the EDS, so they are guaranteed to occur in the index.

---

## Summary Table

| Extension | Direction | Tool(s) |
|-----------|-----------|---------|
| `.msa` | Input | `msa2eds` |
| `.vcf` | Input | `vcf2eds` |
| `.fasta` / `.fa` / `.fna` | Input | `vcf2eds` (reference) |
| `.eds` | Input + Output | `eds2leds`, `edsparser-stats`, `edsparser-genpatterns` |
| `.leds` | Input + Output | `edsparser-stats`, `edsparser-genpatterns` |
| `.seds` | Input + Output | `eds2leds`, sources companion |
| `.patterns` | Output | `edsparser-genpatterns` |
| `.csv` / `.json` | Output | `edsparser-stats` |
