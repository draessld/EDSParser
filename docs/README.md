# EDSParser

A C++ library and CLI suite for parsing, transforming, and validating
**Elastic-Degenerate Strings (EDS)** — a compact data structure for representing
population-level sequence variation in pangenomics.

---

## What is an Elastic-Degenerate String?

A classical string is a sequence of characters. An EDS generalises this: each
position holds a *set* of alternative strings instead of a single character.

Formally, an EDS is a sequence

```
T = (T₁, T₂, …, Tₙ)
```

where each **symbol** Tᵢ is a finite, non-empty set of strings over some alphabet.
A symbol with exactly one string is **common** (non-degenerate); a symbol with two
or more is **degenerate**.

Concretely:

```
ACGT{A,ACA}CGT{T,TG,}AACG
```

- `ACGT`, `CGT`, `AACG` are common symbols — all paths agree here.
- `{A,ACA}` is a degenerate symbol — some paths carry `A`, others carry `ACA`
  (an insertion).
- `{T,TG,}` is a degenerate symbol with an empty-string alternative —
  representing a deletion.

The name *elastic* refers to the fact that alternatives can have different lengths,
so the total character count varies across paths. *Degenerate* comes from the
generalised nucleotide notation where ambiguity codes represent multiple bases.

A **path** through an EDS is any sequence obtained by picking exactly one
alternative from each symbol. An EDS with n symbols and k alternatives per
degenerate symbol implicitly encodes up to kⁿ concrete sequences — yet stores
only their shared structure once.

---

## Why EDS for Pangenomics?

Genomics has historically operated on a *reference genome*: one canonical
sequence, with variation stored separately in VCF files. This approach has
two structural problems:

1. **Reference bias.** Any read or pattern that diverges from the reference
   is harder to align, leading to systematic blind spots in variant-rich regions.
2. **Query coupling.** To ask "does this pattern appear in any haplotype?", you
   must query the reference, enumerate VCF alleles, reconstruct each haplotype
   in memory, and repeat. For a population of thousands of samples this is
   prohibitively expensive.

An EDS encodes the entire population in one structure. A single pattern search
over the EDS finds all matching paths simultaneously — no reconstruction loop,
no reference bias.

### Why MSA and VCF as inputs?

These are the two formats in which large-scale genomic variation already exists:

- **MSA** (Multiple Sequence Alignment, FASTA with gap columns) — common output
  of alignment pipelines. Each row is one haplotype; gap columns represent indels.
- **VCF** (Variant Call Format) — standard for population-level variant catalogs;
  requires a reference FASTA to reconstruct alternatives.

EDSParser converts both to EDS and, immediately, to l-EDS — the variant required
by downstream pattern search and indexing tools.

---

## The l-EDS Constraint

Pattern search tools operating on EDS need to reason about the neighbourhood of
each degenerate set unambiguously. If two degenerate sets are separated by only
one common character, a context window of length l straddles both sets — it is
impossible to tell which alternative of the first set is active when inspecting
the second.

The **l-EDS** constraint resolves this: every *internal* common segment (one with
a degenerate symbol on both sides) must have length ≥ l. This guarantees that the
l-character context window around any degenerate set lies entirely within one
non-degenerate segment and is unambiguous.

Boundary segments (at the start or end of the EDS) are exempt — they have no
degenerate neighbour on one side, so context is not ambiguous.

l-EDS is the output format consumed by downstream tools such as
[biofmi](https://github.com/draessld/biofmi).

---

## The Format Pipeline

```
MSA file ─────────────────────────────┐
                                       ├─► EDS ─► l-EDS ─► downstream tools (e.g. biofmi)
VCF + reference FASTA ────────────────┘
```

Or in two steps when you already have an EDS:

```
EDS ──[ eds2leds ]──► l-EDS
```

The pipeline is deliberately split. `msa2eds` and `vcf2eds` produce either
plain EDS or l-EDS directly (with `-l`). For stored EDS files that need to be
re-contextualised at a different `l`, `eds2leds` re-runs the merging step
without re-parsing the source data.

---

## Design Philosophy

### Why streaming everywhere?

The first prototype loaded the entire EDS into RAM. This worked for toy inputs.
On real data — an 84 MB EDS paired with a 13 GB `.seds` source file, or a 100 GB
EDS from a large cohort — it produced memory usage in the tens or hundreds of
gigabytes. The architecture was redesigned from scratch around three principles:

1. **Metadata-only loading by default.** `EDS::load()` reads the file once to
   build an index (byte offsets, string lengths, degenerate flags) and discards
   the string data. Individual symbols are re-read on demand via `read_symbol()`,
   with a sequential-seek optimisation: if the stream is already at the right
   position, the `seekg()` call is skipped.

2. **LRU-cached source streaming.** `.seds` files grow proportionally to the
   number of paths times the number of strings — easily 10–100× larger than the
   EDS itself. `Sources::load()` indexes the file with a single sequential read
   (64 KB chunks, scanning for `{` characters), then serves individual source
   sets on demand with an LRU cache (default: 10 000 entries, ~400 KB). Hit rate
   is typically >98% because merging accesses sources in linear order.

3. **Iterative temp-file chaining for l-EDS.** The merge algorithm must read and
   re-read symbols across iterations. Instead of keeping all data in RAM, each
   iteration writes its output to a new temp file, which the next iteration reads
   in METADATA_ONLY mode. Peak memory is O(metadata + one batch) rather than
   O(file_size × iterations).

### Why two merge strategies?

**CARTESIAN** — compute the cross-product of all alternative combinations.
Correct in the mathematical sense: every possible path is preserved. But for
real population data (e.g. 1000 Genomes with thousands of haplotypes), the
cross-product between adjacent degenerate sets explodes exponentially.

**LINEAR** — compute only the combinations where the two strings share at least
one source path. If haplotype 3 carries alternative `A` at set i and alternative
`GT` at set i+1, the combination `AGT` is valid. If no haplotype carries both,
the combination is biologically impossible and is discarded. This makes l-EDS
merging of phased data both correct and tractable.

The practical consequence: `eds2leds` detects which strategy to use automatically.
If a `.seds` file is supplied, LINEAR is used; without it, CARTESIAN.

### Why per-process temp directories?

Early versions wrote temp files to `/tmp/edsparser_leds_<iteration>/`. Running
two `eds2leds` processes in parallel (e.g. from experiment scripts) caused them
to overwrite each other's files and produce corrupted output. The fix is
`/tmp/edsparser_leds_<pid>/` — each process owns an isolated directory that is
cleaned up on completion or error.

### Why the complexity estimator?

The CARTESIAN merge on dense or adjacent degenerate sets produces exponentially
many alternatives. This is not obvious from file size alone — a 10 MB EDS with
adjacent degenerates can produce a 10 GB l-EDS. `estimate_leds_complexity()`
runs in O(n) over metadata (no string data) and warns before the transformation
starts, giving the user a chance to switch to LINEAR or adjust `l`.

---

## Project Thoughts

EDSParser started as the input-parsing layer for biofmi. The initial assumption
was that the hard part was the downstream indexing; the format conversions were boilerplate.

In practice, the format conversions were where most of the interesting problems
lived:

- **The MSA 3-pass algorithm** emerged because a naïve approach (load all
  sequences, then decide symbol boundaries) required O(num_sequences × alignment_length)
  RAM. Three passes over the file, with only a bit vector and file positions in
  memory, brought this down to O(alignment_length).

- **The iterative merge convergence** was not obvious upfront. One merge pass can
  create new violations (a newly merged string can be shorter or longer than the
  original, changing its neighbourhood's compliance). Proving that the process
  always converges required thinking through the monotonicity of the "minimum
  context length" metric across iterations.

- **Source tracking was the biggest engineering surface.** The initial design had
  sources as a `vector<set<int>>` embedded in the EDS object. It worked for small
  inputs. The first real dataset showed that sources can be 100× larger than the
  EDS. Every subsequent decision — the LRU cache, the `copy_range_to_stream()`
  optimisation, the distinction between `read_source()` (value, thread-safe) and
  `read_source_ref()` (reference, single-threaded only) — was driven by that one
  observation.

- **The sequential-seek elimination** was a profiling surprise. `seekg()` on a
  local file is not free; on the test machine, eliminating redundant seeks on
  forward-sequential access reduced per-iteration time by ~35% on large files.
  The fix is four lines: cache the current stream position and skip the seek call
  if already there.

---

## Concepts Glossary

| Term | Definition |
|------|------------|
| **EDS** | Elastic-Degenerate String — sequence of sets of strings |
| **Symbol** | One element of the EDS sequence; common (one string) or degenerate (≥2) |
| **l-EDS** | EDS where every internal common segment has length ≥ l |
| **Context** | A common symbol; the non-variable region between two degenerate sets |
| **Source / Path** | A concrete sequence (e.g. one haplotype) consistent with the EDS |
| **SEDS** | Source EDS — companion file mapping each string to its source paths |
| **LINEAR merge** | Phasing-aware merge: keeps only combinations that share a source |
| **CARTESIAN merge** | All-combinations merge: cross-product, no source filtering |
| **Cardinality `m`** | Total number of strings across all symbols |
| **`n`** | Number of symbols |
| **`N`** | Total number of characters summed across all strings |

---

## Documentation Map

| Page | Contents |
|------|----------|
| [Algorithms](algorithms.md) | MSA 3-pass, iterative merge, VCF block processing, source intersection |
| [Architecture](architecture.md) | Storage modes, two-phase loading, LRU cache, pipe buffers, thread safety |
| [File Formats](formats.md) | EDS, SEDS, EDZ, MSA, VCF — syntax, semantics, edge cases |
| [CLI Tools](cli-tools.md) | All flags and examples for every tool |
| [Library API](library-api.md) | C++ public API: EDS, Sources, transform functions |
| [Performance](performance.md) | Memory profiles, throughput numbers, tuning by dataset size |
| [Testing](testing.md) | Unit, integration, e2e, memory stress, benchmarks |
