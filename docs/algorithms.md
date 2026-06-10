# Algorithm Deep Dives

This document explains the key algorithms in EDSParser in detail.

---

## MSA → EDS: Three-Pass Streaming Algorithm

**File:** `src/cpp/lib/transforms/msa_transforms.cpp`

### Overview

Converting a FASTA MSA (with gap columns) to an EDS requires deciding, for
each column range, whether the sequences agree (common symbol) or disagree
(degenerate symbol). The algorithm does this in three passes, keeping
only the reference sequence in RAM at once.

### Pass 1 — Parse MSA Structure

```
Input:  msa_stream (FASTA with gaps)
Output: reference[] (in RAM)
        file_positions[] (one per sequence)
        variant_bitvector[] (one bit per MSA column)
```

Steps:
1. Read the first sequence as the **reference**, storing it as a `std::string`.
2. Record the file byte position of each sequence start in `file_positions[]`.
3. For each subsequent sequence, read it column-by-column and set
   `variant_bitvector[col] = true` if that column differs from the reference
   or contains a gap `-`.

Memory: O(alignment_length) for reference + O(alignment_length/8) for
the bit vector + O(num_sequences × 8 bytes) for file positions.

### Pass 2 — Symbol Boundary Determination

Two sub-modes depending on whether `-l` was specified:

**EDS mode** (no `-l`):
Every transition in the bit vector is a symbol boundary:
- consecutive `false` bits → one common symbol
- each `true` bit → one (possibly degenerate) symbol

**l-EDS mode** (with `-l`):
After identifying raw boundaries, iteratively merge symbols until every
internal common segment has length ≥ l:
1. Mark all column ranges as tentative symbols.
2. Walk left-to-right: when a common segment < l is found between two
   degenerate symbols, merge it with one of the neighbours.
3. Repeat until the constraint holds globally.

### Pass 3 — Streaming Output Generation

For each symbol boundary determined in Pass 2:
1. Seek each sequence's file stream to `file_positions[seq] + col_start`.
2. Read the columns for this symbol for each sequence.
3. Collect alternatives, remove duplicates, track source IDs.
4. Write the symbol to the output stream immediately:
   - One alternative → plain string (compact) or `{string}` (full)
   - Multiple alternatives → `{alt1,alt2,...}`
5. Write the corresponding source sets to the SEDS output.
6. Flush both streams to prevent buffering.

Memory for Pass 3: O(alternatives per symbol) — all output is streamed.

---

## EDS → l-EDS: Iterative Merge

**File:** `src/cpp/lib/transforms/eds_transforms.cpp`

### Goal

Transform an EDS into an l-EDS by merging adjacent symbols until every
**internal** non-degenerate (common) symbol has length ≥ l.

### Merging Two Adjacent Symbols

Let `S1 = {a1, a2, ..., ak}` and `S2 = {b1, b2, ..., bm}`.

**CARTESIAN merge** — produces all `k × m` combinations:
```
merge(S1, S2) = {a1b1, a1b2, ..., a1bm,
                  a2b1, ..., akbm}
```

**LINEAR merge** — produces only combinations where the source paths
(haplotypes) intersect:
```
merge(S1, S2) = {aᵢbⱼ : intersect(src(aᵢ), src(bⱼ)) ≠ ∅}
```

The merged sources of each result string = `intersect(src(aᵢ), src(bⱼ))`.

This preserves only biologically valid haplotype paths and prevents the
Cartesian explosion common in real population data.

### Iterative Algorithm

```
Iteration:
  1. Load EDS as METADATA_ONLY
  2. Scan symbols left-to-right
  3. For each common symbol with length < l flanked by degenerates on both
     sides:
       - Identify the merge direction (merge with left or right neighbour,
         whichever produces fewer alternatives or is cheaper)
       - Enqueue this as a merge pair
  4. compute_merge_metadata(pairs)  ← no string data, only sizes + sources
  5. stream_merged_symbols_to_file() ← read strings on-demand, concatenate,
                                       write to temp file
  6. Point the input to the new temp file
  7. Repeat until no merges are needed
```

Typical convergence: **2–5 iterations** for real genomic data (most
short contexts are resolved in the first iteration).

Worst case (adversarial: alternating single-base common symbols):
O(log(min_context_gaps)) iterations.

### compute_merge_metadata

Uses only metadata (string lengths, source sets) to determine, for each
merge pair:
- The lengths of all merged result strings (sum of pair lengths)
- The source sets of all merged result strings (set intersections)
- Whether the merge produces an empty result (all intersections are empty)

This step reads **zero bytes of string data** from disk.

### stream_merged_symbols_to_file

For each merged result:
1. Read symbol i's strings via `read_symbol(i)` (one seek + read).
2. Read symbol j's strings via `read_symbol(j)`.
3. Concatenate `strings_i[a] + strings_j[b]` directly into the output stream.
4. Flush after each symbol.

For unmodified symbol ranges (not involved in any merge), the raw SEDS
bytes are bulk-copied with `copy_range_to_stream()` — one seek + sequential
read for the entire run.

### SEDS Batching

When copying unmodified source ranges, consecutive unmodified symbols are
accumulated into a single `copy_range_to_stream()` call per run, rather than
one call per symbol. This reduces SEDS write calls from ~N to ~(number of
merge boundaries), typically 2–5× fewer per iteration.

---

## VCF → EDS: Block-Based Variant Processing

**File:** `src/cpp/lib/transforms/vcf_transforms.cpp`

### Overview

The VCF stream is read line-by-line (forward-only, cannot seek). The
reference FASTA supports random-access seeking.

### Genomic Block Algorithm

```
block_start = 0
carryover_queue = []

while not eof(vcf_stream):
    block_end = block_start + block_size

    Read VCF lines until variant.pos >= block_end:
        if variant.pos < block_end:
            add to current_block_variants
        else:
            add to carryover_queue (belongs to next block)

    Read reference bases [block_start, block_end) via read_fasta_region()
    Sort current_block_variants by position
    group_overlapping_variants()    ← merge overlapping into single degenerate
    generate_eds_from_variants()    ← produce EDS text for this block
    Write EDS and SEDS to output streams, flush
    Free current_block_variants

    current_block_variants = carryover_queue
    carryover_queue = []
    block_start = block_end
```

### read_fasta_region

Reads a contiguous range of bases from the FASTA file:
1. Seek to the chromosome start + column-adjust for line-length headers.
2. Read bytes, skipping newlines, into a pre-reserved `std::string`.
3. Return the result.

Uses `reserve()` on the output string to avoid repeated reallocations.

### Overlap Merging

When two variants overlap (both affect base position P), they are merged
into a single degenerate symbol:

```
Variant A: pos=100, REF=ACG, ALT=A
Variant B: pos=101, REF=CG,  ALT=TT

After overlap merge:
  Combined REF span = [100, 103)
  Alternatives: {A__}, {ATT} (padding with reference bases)
```

`generate_eds_from_variants()` returns the group count to avoid calling
`group_overlapping_variants()` twice.

### Structural Variant Handling

| Type | Mechanism |
|------|-----------|
| `<DEL>` | Empty string alternative `""` |
| `<INS>` | Extract bases from INFO/SEQ tag |
| `<INV>` | Reverse-complement the REF bases |
| `<CN0>` | Empty string (deletion) |
| `<CN1>` | REF string (no change) |
| `<CN2..N>` | REF repeated N times |

---

## Source Intersection Semantics

The universal marker `{0}` represents "all known paths traverse this
string". The rules:

```
{0} ∩ {0}         = {0}       — both universal
{0} ∩ {1,2,3}     = {1,2,3}  — concrete wins
{1,2,3} ∩ {2,3,4} = {2,3}    — standard set intersection
{1,2} ∩ {3,4}     = ∅         — invalid combination (filtered in LINEAR merge)
```

The `{0}` marker is written by `genrandomeds` for all non-degenerate
symbols (all paths traverse the reference sequence).

---

## Pattern Generation Algorithm

`EDS::generate_patterns()` extracts patterns that are guaranteed to exist
in the EDS by following actual paths:

1. Pick a random starting position in the EDS (weighted by string length).
2. Follow the path: at each degenerate symbol, randomly pick one alternative.
3. Collect characters until `pattern_length` characters are gathered,
   crossing symbol boundaries as needed.
4. Output the resulting substring.

Patterns always respect degenerate choices — they are valid substring of
at least one EDS path, making them suitable for testing FM-index queries.

---

## Complexity Estimation

`estimate_leds_complexity()` computes three metrics over EDS metadata:

1. **Adjacent degenerate pairs** — count of `(is_degenerate[i], is_degenerate[i+1])` both true.
2. **Short contexts** — count of common symbols with length < context_length
   that are flanked by degenerates.
3. **Average degenerate cluster size** — average run length of consecutive
   degenerate symbols.

Risk tiers:
- `adjacent_pairs == 0` → FAST (no merging needed)
- `adjacent_pairs < 100` and `avg_cluster < 3` → may be SLOW
- `avg_cluster >= 3` or `short_contexts > 500` → EXPONENTIAL_GROWTH_RISK
  (Cartesian expansion can produce exponentially many alternatives)

The `recommendation` field suggests actions:
- Increase context length to skip more symbols without merging
- Use LINEAR merging to prune invalid paths
- Reduce variability when generating synthetic data
