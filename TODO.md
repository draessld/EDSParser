# EDSParser – known issues and planned work

**Scope (2026-09-02):** this file is library, tool and documentation work. Everything to do
with *running* experiments — datasets, specs, sweeps, write-ups and measurement debt — moved
to `~/Data/experiments/edsparser/experiment_design.md`, the project that owns them. Moved
items keep their old numbers (0a, 1b, 1c, 2f, 3a, 3d, 3e, 3f, 3g) so references from `docs/`,
`CLAUDE.md` and the specs still resolve. Closed items are no longer carried here — git
history has them. What survives of them is **Standing decisions**: choices that constrain the
open items, recorded so they are not proposed again.

Ordered by what blocks a correct build. Last reprioritised **2026-09-02**; before that
2026-08-30 (REF validation, and the decision to keep sources sample-level), 2026-08-20 (unit
suite), 2026-08-06 (complement-source fix, 20d8ff1).

**State of things:** `ctest` 7/7, e2e 9/9, and the linear merge is correct and cheap on
haploid data (Mtb, 500 paths, l=100 in 3.3 s at 111 MB once long alleles are filtered). Two
code defects matter: `vcf2eds` grows **superlinearly** with panel size on long indels (1a),
and the memory estimate cannot be trusted for admission control (1d). The rest is performance
headroom (2a–2e) and documentation (3b, 3c).

---

## Standing decisions — constraints, not tasks

### Sources stay sample-level — decided 2026-08-30, won't fix

One path per **sample**, not per chromosome copy: `num_paths = n_samples`. A path answers
*"did this sample carry this combination on some copy"*, not *"does this chromosome carry
it"*. A deliberate modelling choice, not an oversight.

Haplotype-resolved paths were **prototyped and reverted on 2026-08-30**, and the prototype
worked: building each group's haplotype per path made every symbol's source sets partition
the path universe, and the synthetic 50-sample clustered heterozygous case that is killed at
an 8 GB cap at every l ran in **22 MB and 0.5 s at l ∈ {5,10,20,30}**. It was rejected on
artifact size, not mechanism: it doubles the path universe (2504 → 5008 on 1000G) and a
bitset entry costs ⌈paths/8⌉ bytes, so every SEDS/EDZ entry doubles with it — and sources
already dominate on disk (~33 GB against ~3.4 GB of EDS on 1000G).

What the model costs, so nobody re-derives it: a heterozygous sample is marked present in
*both* the reference and the ALT string at a site, so `intersect_sources()` rarely returns
empty and a chain of k adjacent degenerate sites can survive up to 2^k combinations. 1000G
chr7 at l=5 peaks at 28.3 GiB and expands 17.9×; a synthetic 50-sample heterozygous diploid
VCF is killed at 8 GB at every l while its haploidised twin runs l=5..30 under 19 MB.

Three consequences, all permanent:

- `path_cap = num_paths` in `estimate_worst_case_merge_memory()` is **not** a true bound, so
  1d cannot be fixed by trusting it.
- Traversing one path does not reconstruct a physical chromosome. `num_paths` counts samples.
- biofmi's source validation can only answer "this sample could carry this combination",
  never "this haplotype does" — see its TODO B4.

**Supported workflow for genuinely diploid data:** haploidise the VCF before conversion (one
allele per sample), or subset samples. Both are modelling choices and should be stated
wherever the resulting numbers are published. This is what makes the Mtb pipeline cheap and
is legitimate for clonal or inbred panels.

### `check_position()` is gone, not pending a specification — decided 2026-09-02

Deleted with its exclusive helpers (`decode_degenerate_string_number()`,
`reconstruct_from_memory()` — dead even from it — `reconstruct_from_file()`,
`calculate_path_intersection()`) and the `cum_degenerate_counts` metadata array they alone
needed. It was the last unspecified public API: nothing here called it, and biofmi does not
either — the presumed consumer vendors this library under `external/edsparser/` and uses only
`length()`, `cardinality()`, `size()`, `read_symbol()` and `get_metadata().is_degenerate` /
`.num_degenerate_symbols`. Its own tests were its entire specification and had contradicted
themselves about the coordinate convention.

`find_symbol_at_common_position()` and the lazy `cum_common_positions` stay —
`generate_patterns()` uses them. Test 26 no longer needs a locate oracle: it enumerates the
4-string language of its fixture EDS and checks substring containment. If biofmi later wants
position checking, specify the convention first (in particular whether a match may begin
inside a degenerate symbol, which the deleted API could not express) rather than restoring
this implementation.

---

## P0 — correctness

### 1a. `vcf2eds` emits one full-span haplotype per (variant, allele) in a group

Discovered 2026-08-06 on the Mtb panels; the **dominant** cost on assembly-derived data.

- **Symptom:** `tb_p100` (100 isolates, 4.41 Mb genome) has `N_characters` 48,136,917 — 11×
  the genome — of which only 4,121,154 are the common backbone. 91% of the EDS text lives
  inside degenerate symbols.
- **Superlinear in panel size; the allele filter makes it flat.** 11.4× the isolates gives
  **65× the EDS** unfiltered (`tb_p100` 46.0 MB → `tb_p1141` 2 990 MB, i.e. 3.13 billion
  characters for a 4.41 Mb genome), but **4.3 MB → 5.0 MB** filtered — essentially constant,
  which is what saturation of variable sites predicts (210 → 107 new sites per isolate). Full
  panel × l table in `~/Data/experiments/edsparser/` (`results/tb_p*`, notebook
  `eds2leds_tb`).
- **The unfiltered branch loses to storing the genomes.** At 1 141 isolates the unfiltered
  l=100 output is **8.2 GB against a 4.69 GB raw panel** — the index is bigger than
  concatenating all 1 141 genomes. The filtered l=50 output is 91.6 MB, 51× *smaller* than
  the panel.
- **Mechanism:** `group_overlapping_variants()` merges every variant whose span overlaps,
  then `merge_variant_group()` builds `merged_haplotypes` by applying **one variant at a
  time** to the whole group span. A long deletion swallows every variant inside it and emits
  one full-span string per allele. Modelled on the equivalent 32-isolate panel: predicted
  18.8 MB against 18.0 MB actual, with the **top 10 groups holding 89% of all degenerate
  text** and a single 19,122 bp group holding 325 variants emitting 334 haplotypes for
  6.4 MB by itself.
- **Workaround in place:** `make_allele_subset.sh` drops alleles over N bp before conversion
  — 3.9% of sites removed took that EDS from 18.0 MB to 4.3 MB. Good enough to keep
  experiments moving, but it discards real structural variation rather than representing it.
- **Action:** decide whether groups should emit *combined* haplotypes (correct, but that is
  the cartesian product again) or whether long/overlapping variants should be split into
  separate symbols. Until then, document the behaviour.

### 1d. `--max-memory` / `--estimate-memory` under-predicts 45× on het VCF data

Measured 2026-08-01 on 1000G chr7, l=5: predicted `RECOMMENDED_BUDGET_BYTES` 0.64 GiB against
an **actual peak of 28.3 GiB**. Unaffected by the complement fix — chr7 has 2504 paths.

- **Root cause:** `estimate_worst_case_merge_memory()` caps each group's `merged_size` at
  `path_cap = num_paths`, which the sample-level source model breaks (see Standing decisions):
  a sample sits in several strings at one symbol, so a merge can outnumber the paths. Without
  the cap the raw cartesian bound estimates ~TB for the same input and would refuse
  everything, so neither existing bound is usable.
- **Fix direction:** bound it by *doing* the fold, cheaply and partially — rank groups by
  their cartesian bound, run a counting-only fold over the sources for the top-K (a few
  thousand) tracking only surviving source sets and their count, abort past a limit (1e6) and
  report `≥ limit`, then use the capped bound for the tail.
- **Until then:** do not use `--max-memory` for admission control on VCF-derived input.
  Emitting `UNRELIABLE=1` whenever any group's cartesian product exceeds `path_cap` would at
  least stop schedulers trusting a guess.
- **`--block-size` does not help here:** it bounds the per-symbol index, not one group's
  merge metadata. A 2^k-combination group is the same size however the file is cut.

### 0c. `edsparser-stats` reports a `context_min` that includes the exempt boundary symbols

The exemption is intended and documented — l-EDS constrains every *internal* common segment;
`needs_merge()` skips the first and last symbol (`i > 0 && i < n - 1`), which is sound because
a boundary segment has a degenerate neighbour on only one side. Its *presentation* is not: a
correct l-EDS looks like it violates its own constraint because the single reported
`context_min` counts the first and last common symbol too. `{ACGT}{A,C}{160bp}{G,T}{160bp}` at
`-l 20` converges in 0 iterations and reports `context_min` 4; every TB panel reports 27–31 at
l=50 and l=100 because the first variant sits ~30 bp into H37Rv. Report the interior minimum
separately (or alongside), and confirm biofmi does not assume every context is ≥ l. The
caveat is now stated in `docs/cli-tools.md`, `docs/library-api.md` and the root `README.md`;
the definition itself is in `docs/formats.md`, `docs/README.md` and `docs/algorithms.md`.

---

## P1 — performance and scale

### 2a. The `Sources` index is the remaining memory floor in linear block mode

`--block-size` gave the ceiling: on a 949 MB / 31.7M-symbol input at `-l 10`, whole-file
2380 MB → 540 MB linear at 10M blocks, and **80 MB cartesian, independent of file size**.
Linear stops improving below ~50 MB blocks because block mode still loads one whole-file
`Sources` index to slice each block: 8 B/string, i.e. 509 MB for that input's 63.7M strings,
which is exactly the 540 MB observed. Two ways out:

- *Sampled index* — keep every 16th–32nd entry offset and scan forward (8 → 0.25-0.5 B/string,
  509 MB → 16-32 MB). Less code, helps every tool rather than just block mode, but touches the
  hot `read_source` / `copy_range_to_stream` paths. **Cheapest real win; do this one first.**
- *Streaming source slice* — one sequential pass writing each block's slice with no index at
  all. Needs per-format sequential decode (text SEDS brace scan, sparse bitvec, EDZ
  fixed-width records, EDZ_COMPRESSED in block order). A true ceiling for linear mode too.

Also worth documenting for users: barrier availability is data-dependent — at large l, or in
dense variation, barriers thin out and blocks grow, with a clean whole-file fallback when none
exist.

### 2b. Sequential-reader rewrite *(lower priority — block mode got the ceiling more cheaply)*

Every phase already walks positions left-to-right (`select_merge_groups()`,
`compute_merge_metadata()`, `MergeStreamWriter`'s monotone cursor). The per-symbol index
exists only because `read_symbol(pos)` is a random-access API; a sequential reader with a
bounded lookahead (`needs_merge` needs at most `context_length` characters ahead) would make
an iteration O(batch + lookahead) instead of O(n).

Blockers: `EDS::from_metadata()` hands the next iteration a full metadata struct;
`compute_merge_metadata()` parallelises over groups indexing metadata by absolute position and
would need position-relative slices; the raw-copy pass-through computes byte spans from
`base_positions`, which a sequential reader knows as it parses. Shares its core with 2c.

### 2c. Single-pass `vcf2eds -l`

Today `vcf2eds -l N` is two-stage: `parse_vcf_to_leds_streaming_direct()` writes the full
stage-1 EDS/SEDS to temp files, then runs `eds_to_leds_linear()` over them. Temp files rather
than a pipe are required because the merge consumes its input to EOF before emitting output,
so `make_pipe()` would deadlock.

Two possible shapes: fuse the chain-merge state into the VCF walk (needs a windowed
convergence proof, since the linear pipeline converges over full-file iterations), or buffer
per VCF block plus a carryover window large enough to cover any chain crossing the boundary.
Value: removes the stage-1 disk write and the reparse. Must stay byte-identical to the
two-stage output; `--keep-eds` would bypass it.

### 2d. The merge pipeline still writes only dense text SEDS internally

*Partly addressed.* `eds2leds --source-format {seds,seds-sparse,edz,edz-sparse,edz-compressed}`
re-encodes once at the end via `Sources::save_as()`, so the final artifact can be compact
(edz-compressed measured 4.7× smaller than SEDS at 500 paths, 5.9× at 2504). But the pipeline
writes dense text SEDS for every iteration and for the pre-conversion output, so **peak disk is
unchanged** and the conversion costs an extra pass. `vcf2eds -l` has no equivalent flag at all.

To close it: give `stream_merged_symbols_to_file()` / the SEDS batching path a sparse and EDZ
writer, mirroring `write_seds_sparse_finalize()` / `write_edz_entry()`.

### 2e. Choose the source format from set density, not from `num_paths`

The documented rule "EDZ pays off above a few hundred paths" is **wrong**, and wrong in a way
that gets worse with scale: a bitset costs ⌈paths/8⌉ bytes per entry whatever it contains, so
it wins only when entries are dense. On the rare-variant TB panels sparse EDZ is **4.4× larger
than the text it replaces** at 1 141 paths (34.07 MB vs 7.83 MB) — a variant carried by 3
isolates is 143 bytes as a bitset and ~12 as `{5,88,900}` — while on 1000G-like data
edz-compressed is 4.7–5.9× *smaller* than SEDS. The datasets differ in allele-frequency
spectrum, not path count.

Code action: pick the format automatically from the observed mean set density (or at minimum
replace the `num_paths` rule in the docs with the density one). Blocked on the completed
measurement — `source_formats` in `experiment_design.md` (2e there), which still lacks
edz-compressed and gzip arms on rare-variant data.

---

## P2 — documentation

`docs/performance.md` carries numbers that are theoretical, or measured on unspecified
hardware. Several are contradicted by real runs, which makes them worse than missing.

### 3b. Correct the iterations-to-convergence table *(pure docs edit — the measurement exists)*

`performance.md` claims 2-4 iterations for MSA-derived and **3-6 for VCF-derived** data. Real
runs disagree: hgp1000 converges in **2** iterations on every chromosome at l=5, and Mtb and
yeast both converge in **1** at every l tested. The chain-merging change (2026-06-28) is why.

### 3c. Replace the projected memory table with measured numbers

The old-vs-new table (100×-3000× reduction) is projected from architecture analysis. Real
figures now exist — 2380 → 540/80 MB under block mode at 31.7M symbols, chr7 at 28.3 GiB, the
full hgp1000 run — and should replace it. Needs the hardware section from 3a
(`experiment_design.md`) to be meaningful.

### Docs claims blocked on measurements that now live in `experiment_design.md`

`performance.md` cites TODO ids that moved; the citations still resolve, but the work is
there, not here: **3a** (hardware baseline, line 355), **3d** (compressed 1000G footprint,
line 79), **3e** (MSA throughput line 59, block-size wall-clock line 91, LRU hit rate line
330). Update those sections as each measurement lands.
