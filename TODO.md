# EDSParser – known issues and planned work

---

## Planned Features

### RandomAccess — pointer into EDS data at a located position

**Motivation:** `BioFMI::locate()` returns `(position, changes)` pairs where `position` is
a character-level offset in T₀ and `changes` is a list of **absolute** (0-based global)
alternative indices. Currently there is no way to go from that result back into the EDS
data without re-parsing the whole file. `RandomAccess` closes that gap by returning a
lightweight pointer (file offset + symbol metadata) into the underlying data so the caller
can stream from that point without materialising the string.

**Proposed signature:**

```cpp
// Returns a pointer into the EDS data at the given character position,
// following the given absolute alternative indices when traversing degenerate sets.
// 'changes' uses the same 0-based global numbering as BioFMI::locate() output.
struct EDSPointer {
    std::streampos file_offset;   // byte offset in the .eds / .leds file
    size_t         symbol_idx;    // EDS symbol index (0-based)
    size_t         string_idx;    // alternative index within that symbol (0-based)
    Position       char_offset;   // character offset within that alternative
};

EDSPointer random_access(Position char_pos,
                         const std::vector<int>& changes) const;
```

**Design notes:**
- `char_pos` is a character-level position in the same coordinate space as
  `locate()` results: 0-based index in T₀ if the position is in reference,
  `base_position_of_set + offset_within_alternative` if inside a degenerate alternative.
- `changes` carries **absolute** global alternative indices (the same `changes` vector
  returned by `BioFMI::locate()`), not per-symbol local indices.
- The method must map from that coordinate space back to a `(symbol_idx, string_idx,
  char_offset)` triple using `metadata_.cum_common_positions` and
  `metadata_.cum_degenerate_counts`, then seek to the byte in the file.
- In FULL (in-memory) mode: `file_offset` may be unused; `string_idx` / `char_offset`
  index directly into `sets_`.
- Distinct from the existing `extract(symbol_pos, len, per_symbol_changes)` which takes a
  symbol-level position and per-symbol local alternative indices.

**Implementation sketch:**
1. Determine which symbol owns `char_pos` by binary search on
   `metadata_.cum_common_positions`.
2. Map the global `changes` entry for that symbol to a local string index within the
   symbol.
3. Seek to `metadata_.base_positions[symbol_idx]`, scan past the selected alternative's
   byte offset (using `metadata_.string_lengths`).
4. Return `EDSPointer`.

---

### Extract — materialise a substring starting at a located position

**Motivation:** Complement of `RandomAccess`. Given a `(position, changes)` pair from
`BioFMI::locate()` and a length in characters, return the actual string content. This is
needed to verify match results, compute flanking sequences, or feed downstream tools.

**Proposed signature:**

```cpp
// Extract 'len' characters starting at 'char_pos', following 'changes' across
// degenerate set boundaries as needed.
// 'changes' uses the same 0-based global numbering as BioFMI::locate() output.
String extract_at(Position char_pos,
                  const std::vector<int>& changes,
                  Length len) const;
```

**Relationship to existing `extract()`:**

The existing method (`eds.hpp:124`) takes a **symbol index** and **per-symbol** local
alternative indices:

```cpp
String extract(Position symbol_pos, Length len, const std::vector<int>& per_symbol_changes);
```

`extract_at` takes a **character position** and **absolute global** alternative indices,
matching `locate()` output directly. Internally it can delegate to `random_access()` to
find the starting point and then read forward `len` characters, crossing symbol boundaries
using `metadata_` to follow the correct alternatives.

**Design notes:**
- Must handle spans that cross multiple EDS symbols (both degenerate and non-degenerate).
- `changes` may be shorter than the number of degenerate sets traversed if the span ends
  before the last set; validate accordingly.
- In METADATA_ONLY mode uses `read_symbol()` per traversed symbol; in FULL mode uses
  `sets_` directly.
- Should be a CLI-exposed tool (`edsparser-extract`) as well as a library method, mirroring
  the pattern of other tools in `src/cpp/tools/`.

---

## Optimizations

### [ARCH] eds2leds per-symbol throughput scales inversely with variability
- **Observed (2026-05-27, 10 MB ref, cartesian, --min-context 5):**
  1% variability → ~50 MB/s; 10% variability → ~5.6 MB/s (~9× drop)
  even when input already satisfies the l-EDS constraint (no merging needed)
- **Previous numbers (pre I/O opt, 2026-05):** 1% → 173 MB/s; 10% → 2.9 MB/s (60× drop).
  The 1% figure dropped after I/O optimisations because the old measurement used the
  installed binary (which loaded small files into FULL mode); the 10% figure slightly
  improved (2.9 → 5.6 MB/s) from the seek-guard and SEDS-batching fixes.
- **Root cause:** per-symbol string concatenation and merge-metadata overhead in
  METADATA_ONLY streaming mode; 10% variability produces ~10× more degenerate sets
  than 1%, each requiring string reads, cartesian concatenation, and output writes.
  (Raw disk I/O was a contributor pre-2026-05 but has been largely addressed.)
- **Decision:** do NOT add block-parallel processing to recover this throughput
  Rationale: parallel blocks require holding multiple blocks in RAM simultaneously,
  contradicting the core design goal of constant memory for arbitrarily large files.
  **Memory stability is higher priority than transformation speed.**
- **Acceptable tradeoff:** a one-time transformation of a 100 GB file taking extra
  minutes is fine; running out of memory on a laptop is not