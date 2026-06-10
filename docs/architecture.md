# Architecture & Design Patterns

This document describes the internal design of EDSParser — how data flows,
why particular trade-offs were chosen, and how the components connect.

---

## High-Level Component Map

```
┌───────────────────────────────────────────────────────────────────┐
│                         CLI Tools (tools/)                        │
│  msa2eds  vcf2eds  eds2leds  edsparser-stats  genpatterns  genrandom│
└─────────────────┬────────────┬───────────────────────┬────────────┘
                  │            │                       │
          msa_transforms  vcf_transforms         eds_transforms
                  │            │                       │
                  └─────┬──────┘                       │
                        │                              │
                   ┌────▼────┐                   ┌────▼────┐
                   │   EDS   │◄──────────────────│ Sources │
                   └────┬────┘   (shared_ptr)    └─────────┘
                        │
               ┌────────▼────────┐
               │  Metadata       │  (always in RAM)
               │  + index tables │
               └────────────────┘
                        │
               ┌────────▼────────┐
               │  File stream    │  (METADATA_ONLY: lazy I/O)
               │  or in-RAM sets │  (in-memory: from string/stream ctor)
               └─────────────────┘
```

---

## Storage Modes

There is no explicit `LoadMode` enum. The mode is inferred at runtime:
if `sets_` is populated the EDS is in-memory; if `sets_` is empty and
the file stream is open it is METADATA_ONLY.

(`FULL` / `COMPACT` are **output format** names — `OutputFormat::FULL`
always emits brackets, `OutputFormat::COMPACT` omits them for single
alternatives. They are unrelated to storage.)

### In-Memory Mode

Used when constructing EDS from a C++ string literal or `std::istream`.
All strings are stored in `sets_` (a `vector<StringSet>`) in RAM.
`read_symbol(pos)` returns directly from `sets_` without any I/O.
Unsuitable for large files; use `EDS::load()` for those.

### METADATA_ONLY Mode

Used by `EDS::load()` (the standard file-loading path).
Only the **Metadata** struct is populated:
- `base_positions[]` — file offset of each symbol (so `read_symbol()` can
  seek directly to it)
- `symbol_sizes[]`, `string_lengths[]`, `is_degenerate[]` — derived during
  the single parse pass
- Statistical summaries — computed in the same pass

On each call to `read_symbol(pos)`, the implementation seeks to
`base_positions[pos]` and reads exactly that symbol from disk.
**Sequential-seek elimination**: if the stream is already at the required
position (detected via a position cache), the `seekg()` call is skipped —
important for forward-sequential iteration.

---

## EDS Parsing: Two-Phase Loading

Even in METADATA_ONLY mode the file is still read **once** completely at
load time to build the index and statistics. After that:

1. **Metadata phase** (load): scan the file, record positions, compute
   statistics, build `cum_common_positions` and `cum_degenerate_counts`.
2. **Access phase** (on-demand): `read_symbol()` seeks + reads one symbol
   per call.

This separates the *knowing what is there* (cheap, O(file size) once) from
the *reading the data* (only when needed, O(symbol size) per access).

---

## Sources: LRU Streaming Cache

The `Sources` class mirrors the EDS two-phase pattern for `.seds` files.

### Index Building (`parse_seds`)

During `Sources::load()`, the file is read in **64 KB chunks** to record
the file position of every opening brace `{`. This builds `base_positions_`
in a single sequential pass with ~32 `read()` syscalls per 2 MB file,
avoiding the old per-character `get()` + `tellg()` approach that caused
millions of syscalls.

### LRU Cache

Subsequent `read_source(idx)` calls:
1. Check the LRU map for a hit — return the cached `set<int>` by value.
2. On a miss, seek to `base_positions_[idx]`, parse the `{...}` entry,
   evict the least-recently-used entry if at capacity, store the new entry.

>Default capacity: **10 000 entries** (~400 KB). Hit rates are typically
>98% because merging operations access sources in linear order.

### Thread Safety

A `mutable std::mutex io_mutex_` protects the file stream and LRU cache.
Both `read_source()` and `read_source_ref()` acquire this mutex.
`read_source()` returns by **value** — safe across threads.
`read_source_ref()` returns a `const std::set<int>&` — only safe for
single-threaded code, because the entry can be evicted by another thread
between the return and the dereference.

### Bulk Copy (`copy_range_to_stream`)

For contiguous ranges of unmodified strings (common in l-EDS iteration),
`copy_range_to_stream(start, count, out)` computes the exact byte span
via `base_positions_[start+count] − base_positions_[start]` and copies
those bytes directly without parsing. This makes sequential calls skip
`seekg()` entirely.

---

## l-EDS Iteration Chain

`eds_to_leds_linear` / `eds_to_leds_cartesian` uses an iterative
temp-file chaining architecture designed to handle 100 GB+ inputs:

```
Input.eds (METADATA_ONLY)
    │
    ▼  Iteration 1: collect merge pairs with context < l
temp0.eds (written via stream_merged_symbols_to_file)
    │
    ▼  Iteration 2: check remaining short contexts
temp1.eds
    │
    ▼ … 
tempN.eds
    │
    ▼  rename / copy → output.leds
```

Each temp file is opened as METADATA_ONLY for the next iteration.
Temp files are stored under:
```
/tmp/edsparser_leds_<pid>/
```

The PID-scoped directory allows multiple `eds2leds` instances to run in
parallel without interference (e.g. from experiment scripts). Files are
cleaned up automatically on normal exit or exception.

### Metadata-Only Merge Calculation

Before writing any strings, `compute_merge_metadata()` calculates — using
only the metadata (string lengths and source sets, no actual characters):
- Which symbol pairs will be merged in this iteration
- The merged string lengths (addition, no concatenation)
- The merged source sets (set intersections)

This produces a list of `MergeMetadata` structs. Only then does
`stream_merged_symbols_to_file()` read strings from disk, merge them
in-place, and write directly to the output stream (no intermediate
`ostringstream`).

### Batch Processing

Merge pairs are processed in batches (default 1 000 pairs per batch):
```
for each batch of 1000 pairs:
    compute_merge_metadata(batch)   → ~10 MB metadata
    stream_merged_symbols_to_file() → reads, merges, writes
    free metadata                   → back to baseline memory
```

---

## Pipe Streaming (VCF → l-EDS Direct)

For the `vcf2eds -l` pipeline, two stages would normally require a
temp file (VCF→EDS then EDS→l-EDS). The `PipeStreamBuffer` class
implements a 64 MB thread-safe circular buffer as a `std::streambuf`:

```
Producer thread:          Consumer thread:
  parse_vcf_to_eds()  ──▶  PipeStreamBuffer  ──▶  eds_to_leds_linear()
```

`make_pipe()` returns a connected `PipeOutputStream` + `PipeInputStream`
pair. The producer writes EDS bytes; the consumer reads them without any
intermediate temp file.

This is used in the `vcf2eds -l` pipeline to avoid writing a potentially
large intermediate EDS to disk.

---

## MSA Transformation: Three-Pass Algorithm

```
Pass 1: parse MSA
  - Read reference sequence into RAM (O(alignment_length))
  - Record file positions of each sequence start
  - Build variant bit vector: variant_col[col] = true if any seq differs

Pass 2: compute symbol boundaries
  - Without -l: every transition between variant and non-variant is a boundary
  - With -l: merge adjacent symbols until internal common segments ≥ l

Pass 3: generate output (streaming)
  - For each symbol boundary:
    - Seek to each sequence's file position for the columns in this symbol
    - Collect alternatives, deduplicate, write {alt1,alt2,...} or plain string
    - Immediately flush to output stream (no accumulation)
```

Memory footprint: O(reference + bit vectors), independent of the number of
sequences or the output size.

---

## VCF Block Processing

Reference genomes are not loaded into RAM. Instead:

1. Divide the genome into blocks of `block_size` bases (default 10 Mbp).
2. For each block:
   a. Read VCF variants belonging to this block (maintaining a *carryover
      queue* for variants that started before block end but were read early).
   b. Call `read_fasta_region()` to load just the reference bases for this
      block via random-access seeking (pre-allocated `string::reserve`).
   c. Sort variants by position, group overlapping ones.
   d. Call `generate_eds_from_variants()` to produce EDS for this block.
   e. Write directly to the output stream and flush.
   f. Free block memory, move to next block.

Memory: O(variants per block), not O(total variants).

---

## Complexity Estimation

`estimate_leds_complexity()` scans EDS metadata (no string data) and
classifies the transformation into three risk tiers:

| Category | Condition | Warning |
|----------|-----------|---------|
| FAST | Few or no adjacent degenerate pairs | None |
| SLOW | Many short contexts between degenerates | `warn_slow = true` |
| EXPONENTIAL_GROWTH_RISK | Dense degenerate clusters | `warn_exponential = true` |

The function returns a `recommendation` string with actionable advice
(e.g., "increase context length" or "consider fewer alternatives").

---

## Thread Safety Summary

| Component | Thread Safety |
|-----------|--------------|
| `EDS` read methods (`read_symbol`, `get_metadata`) | **Not thread-safe** — no mutex on `stream_` |
| `EDS` write methods | Single-threaded only |
| `Sources::read_source()` | **Thread-safe** (mutex on `io_mutex_`) |
| `Sources::read_source_ref()` | **Not safe** across threads (reference may dangle) |
| `Sources::copy_range_to_stream()` | **Thread-safe** (acquires `io_mutex_`) |
| `MemoryMonitor` | Thread-safe (internal `samples_mutex_`) |
| `PipeStreamBuffer` | Thread-safe (mutex + condition variables) |
