# EDSParser – known issues and planned work

**Scope (since 2026-09-02):** this file is library, tool and documentation work. Everything to
do with *running* experiments — datasets, specs, sweeps, write-ups and measurement debt — lives
in `~/Data/experiments/edsparser/experiment_design.md`, the project that owns them. Moved items
keep their old numbers (0a, 1b, 1c, 2f, 3a, 3d, 3e, 3f, 3g) so references from `docs/`,
`CLAUDE.md` and the specs still resolve. Closed items are not carried here — git history has
them, and the last revision lists what closed. What survives of them is **Standing
decisions**: choices that constrain the open items, recorded so they are not proposed again.

Ordered by what blocks a correct build. Last reprioritised **2026-10-01** (branch
`integration2`: test hygiene, the transform fuzzer and its six fixes, the bcftools overlap
check, the API cleanup); before that 2026-09-02 (scope split), 2026-08-30 (REF validation,
sources stay sample-level), 2026-08-20 (unit suite), 2026-08-06 (complement fix, 20d8ff1).

**State of things:** `ctest` 8/8 run green (plus `test_memory_smoke` *Skipped* without its
generated data and `test_memory_stress` *Disabled* by default), e2e 9/9 suites, 155 tests, 0
skipped. `test_transform_fuzz` checks every transform against brute-force path expansion and
`vcf2eds` against `bcftools consensus` exactly. Two code items remain: the l-EDS cost of long
deletions is inherent to an exact l-EDS (1a — what is left there is a modelling decision), and
the memory estimate cannot be trusted for admission control (1d). The rest is performance
headroom (2a–2e) and documentation (3c).

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
  never "this haplotype does" — see its TODO 4a.

**Supported workflow for genuinely diploid data:** haploidise the VCF before conversion (one
allele per sample), or subset samples. Both are modelling choices and should be stated
wherever the resulting numbers are published. This is what makes the Mtb pipeline cheap and
is legitimate for clonal or inbred panels.

### `check_position()` is gone, not pending a specification — decided 2026-10-01 (was 0d)

Deleted with its exclusive helpers (`decode_degenerate_string_number()`,
`reconstruct_from_memory()` — dead even from it — `reconstruct_from_file()`,
`calculate_path_intersection()`) and the `cum_degenerate_counts` metadata array they alone
needed, in the same cleanup as the other uncalled API (CLAUDE.md, "Removed API"). It was the
last unspecified public API: nothing here called it, and biofmi does not either (re-grepped
over its `src/` and `tests/` the day it went). Its own tests were its entire specification and
had contradicted themselves about the coordinate convention.

`find_symbol_at_common_position()` and the lazy `cum_common_positions` stay —
`generate_patterns()` uses them. The pattern test that used `check_position()` as a locate
oracle enumerates the language of its fixture EDS and checks substring containment instead.
If biofmi later wants position checking, specify the convention first (in particular whether
a match may begin inside a degenerate symbol, which the deleted API could not express) rather
than restoring this implementation.

### Overlapping calls: vcf2eds keeps its rule, and says where bcftools disagrees — decided 2026-10-01

*Closes the spec question `test_transform_fuzz` opened on 2026-10-01.* `vcf2eds` and
`bcftools consensus -s` (1.19) resolve calls at overlapping records differently. vcf2eds
applies the first ALT a copy carries in file order and lets a REF or missing call block
nothing (`collect_copy_alleles()`). bcftools — whose `-s` path applies a REF call as a no-op
variant — lets *any* non-missing call claim its REF span, skips a later record starting inside
it, and applies on top only a pure indel (htslib's typing) anchored on the last claimed base
with the same first base, not after an insertion. On the fuzzer's VCFs about 20–25% of the
compared genomes differed; `0` at `24 G>GCG` with `1` at `24 GCGT>C` is the canonical case
(vcf2eds applies the deletion, bcftools does not).

Neither rule is wrong — a sample with calls at overlapping records is contradictory input —
so the conversion keeps vcf2eds's rule. What was wrong was the *silence*: experiment oracles
are bcftools-materialised. `check_overlap_divergence()` now runs both rules over every group
of two or more records for every called copy and reports only the copies whose spelling
differs: `VCFStats::overlap_divergent_{copies,groups,records,samples}`, a warning naming
position, sample and both spellings, and `--strict-overlaps` (exit 4, outputs removed) for
pipelines whose ground truth is bcftools. The detector is exact against bcftools 1.19 itself:
`test_transform_fuzz` P5 requires a silent sample to equal `bcftools consensus` byte for byte
and a flagged one to differ (1000 cases: 983 differ, all 983 flagged, none flagged that agree;
a 10-minute soak, 8,250 VCF cases: 31,241 genomes compared, 7,671 differ, all flagged, none
falsely). Not modelled: symbolic ALTs, diploid IUPAC output (each copy
is compared as if haploid), unsorted input.

Real data, 2026-10-01 (`H37Rv`, default `-b`):

| VCF | overlapping ALT calls ignored | samples differing from bcftools | copies / groups / records |
|---|---:|---:|---|
| `panel_100` raw | 243 | **100 of 100** | 5,984 / 306 / 3,602 |
| `panel_100_snv50` | 44 | **100 of 100** | 2,022 / 219 / 636 |
| `panel_500` raw | 2,531 | **500 of 500** | 64,372 / 663 / 23,246 |
| `panel_100_norm` (`normalise_vcf.py`) | 0 | 0 | — |
| `panel_500` normalised (`normalise_vcf.py`, 4,215 of 68,920 records dropped) | 0 | 0 | — |

On a raw panel *every* genome bcftools writes differs from the one vcf2eds encodes — e.g. at
`NC_000962.3:24689` a REF call on a 36 bp deletion claims the span in bcftools and keeps the
reference there, while vcf2eds applies a later overlapping deletion (checked against
`bcftools consensus` on `GCF_000706665.1`). The normalised panels, which is what every biofmi
oracle uses, are provably clean.

---

## P0 — correctness

### 1a. Long deletions make the l-EDS large — **the EDS is fixed; what is left is a modelling choice**

**Measured state (2026-10-01, `d2cef03` + `--split-groups`, on `integration`).** Both
candidates for the per-group haplotype explosion were built and measured on the TB panels;
neither changes what biofmi indexes, and the reason is structural.

- **(a) Per-copy combined haplotypes — default** (`d2cef03`, the 2026-09-12 partition fix):
  each allele copy gets the span with *every* ALT it carries applied, so a symbol holds the
  distinct haplotypes actually observed — at most one per copy, never a cartesian product —
  and the partition is exact. That alone cuts the EDS 3.2–3.4×. It also needed
  `std::stable_sort` for the block: `std::sort` reordered same-POS records, so which of two
  overlapping calls a copy kept depended on `-b` (tb_p500, sample 2).
- **(b) Atomic segments — `vcf2eds --split-groups`, opt-in.** The span is cut at every record
  start/end and each segment is its own symbol: REF, the ALT beginning there, or empty inside
  a carried deletion; a segment every sample spells joins the common text. Same genome per
  path (md5-identical on tb_p100 and tb_p500, at EDS and l-EDS level; `test_transform_fuzz`
  P5 checks it on every case), same partition, and the EDS goes flat: 5.3 → 8.8 MB from 100
  to 500 isolates, against 14.3 → 223 MB for (a).
- **Why (b) is not the default.** The l-EDS merge must re-join the segments — a group has no
  common text inside it — so with sources it reproduces (a)'s haplotypes (l-EDS within 0.1%
  of (a) at l=10 and 50) at 2–5× the `eds2leds` time, and **without sources the re-join is a
  cartesian product** that did not finish in 120 s on tb_p100 (whole-span: 2 s). It is the
  right output when the EDS itself is the artifact; it buys nothing for an index.
- **Why the l-EDS cost is inherent.** A deletion carried by one isolate removes the common
  text over its whole span for *every* path, so an exact l-EDS must spell each distinct path
  string across it in one symbol. tb_p500's largest symbol is a 118 kb span with 348 distinct
  haplotypes, 34 MB on its own; the top 100 symbols are 98% of the degenerate text. No
  grouping or splitting in `vcf2eds` changes that — only dropping or truncating long alleles
  (lossy: `make_allele_subset.sh`), or a different index model, does.

Measured 2026-10-01 (Ryzen 7 PRO 6850U laptop, shared, single-threaded tools, `MemoryMax=8G`;
"old" = `1cba45e`; filtered = `make_allele_subset.sh … 50` run through (a); dense text SEDS;
breaches from `source_partition_audit.py`):

| panel | mode | EDS | SEDS | vcf2eds | peak RSS | breaches |
|---|---|---:|---:|---:|---:|---:|
| tb_p100 | old | 48.2 MB | 0.56 MB | 2.0 s | 237 MB | 396 |
| tb_p100 | (a) default | 14.3 MB | 0.55 MB | 0.8 s | 211 MB | 0 |
| tb_p100 | (b) `--split-groups` | 5.3 MB | 0.85 MB | 0.8 s | 156 MB | 0 |
| tb_p100 | filtered, (a) | 4.5 MB | 0.60 MB | 0.8 s | 137 MB | 0 |
| tb_p500 | old | 722.9 MB | 3.51 MB | 88.3 s | 2.61 GB | 869 |
| tb_p500 | (a) default | 223.3 MB | 3.15 MB | 8.3 s | 2.72 GB | 0 |
| tb_p500 | (b) `--split-groups` | 8.8 MB | 10.17 MB | 13.5 s | 2.58 GB | 0 |
| tb_p500 | filtered, (a) | 4.9 MB | 4.17 MB | 9.7 s | 2.07 GB | 0 |

`eds2leds` (LINEAR) on those EDS — l-EDS size / time / peak RSS:

| panel | input | l=10 | l=50 |
|---|---|---|---|
| tb_p100 | (a) | 14.4 MB / 0.14 s / 17 MB | 15.0 MB / 0.18 s / 17 MB |
| tb_p100 | (b) | 14.4 MB / 0.28 s / 17 MB | 15.0 MB / 0.34 s / 17 MB |
| tb_p100 | filtered | 4.5 MB / 0.09 s / 9 MB | 5.0 MB / 0.13 s / 10 MB |
| tb_p500 | (a) | 225.0 MB / 1.7 s / 84 MB | 231.6 MB / 2.1 s / 80 MB |
| tb_p500 | (b) | 225.2 MB / 8.9 s / 118 MB | 231.8 MB / 8.9 s / 104 MB |
| tb_p500 | filtered | 4.9 MB / 0.3 s / 16 MB | 9.9 MB / 0.6 s / 15 MB |

`vcf2eds`'s ~2.6 GB on tb_p500 is the block of parsed records (genotype vectors), not the
haplotypes — the same in every mode, 0.98 GB at `-b 1000000`.

**Still open:** whether biofmi wants long alleles at all (if not, the filter is the answer
and should be stated as a modelling choice wherever numbers are published); tb_p1141 is not
on this machine, so the 1141-isolate row is unmeasured.

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

---

## Results that predate fixes

- **The TB and 1000G bundles are SUPERSEDED.** `~/Data/experiments/edsparser/results/`
  `tb_p100`, `tb_p500`, `tb_p1141`, their `_snv50` twins and `hgp1000` were all built before the
  `vcf2eds` grouping fix (`d2cef03`), so their EDS sizes are inflated and their source sets
  break the partition; each carries a `SUPERSEDED.md` saying what changed and what to
  regenerate. `tb_100` is superseded by `tb_p100`. Only `yeast1011_50` is **INVALID**
  (complement bug, 0a in `experiment_design.md`). Regenerate before quoting any of them.
- **The six fuzz fixes (2026-10-01) do not reach the published biofmi l-EDS.** Old
  (`b799f70`) vs new `eds2leds`, regenerated from the published inputs at every l the specs
  use, LINEAR and CARTESIAN: byte-identical `.leds` and `.seds` wherever both runs finished
  (covid294 and tb_p100_norm at l ∈ {3,5,9,11,14,19,29,39,59}, tb_p100_snv50 likewise,
  syn_ctx200_2mb at the 13 `l_sweep` values — 70 cells), and identical to the l-EDS the
  experiments stored (`~/Data/covid/derived/leds`, `~/Data/tb/derived/p100_{norm,snv50}`, 43
  files). Ten CARTESIAN cells OOM at 7 GB in both versions (covid294 l ≥ 19, both TB panels
  l ≥ 29, as in the published runs); none of bugs 3–5 can fire on these inputs (no leading run
  of regular symbols, no empty regular symbol, text sources, no block mode) and bug 6 is
  LINEAR-only, so they are unaffected too. See biofmi's TODO for the table. The edsparser
  bundles above are superseded anyway, and every one of them has > 63 paths, so bug 6 (below)
  could have reached them; regenerating them with this build settles that too.

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
  Block slices already go through `copy_range_to_stream()` (2026-10-01, fuzz fix 5), so a
  streaming slice must keep each entry's input spelling to stay byte-identical.

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
so a bounded pipe deadlocks (the never-used pipe headers were deleted 2026-10-01).

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

### 3c. Replace the projected memory table with measured numbers

The old-vs-new table (100×-3000× reduction) is projected from architecture analysis. Real
figures now exist — 2380 → 540/80 MB under block mode at 31.7M symbols, chr7 at 28.3 GiB, the
full hgp1000 run — and should replace it. Needs the hardware section from 3a
(`experiment_design.md`) to be meaningful.

### Docs claims blocked on measurements that now live in `experiment_design.md`

`performance.md` cites the moved ids as `experiment_design` 3a (hardware baseline), 3d
(compressed 1000G footprint) and 3e (MSA throughput, block-size wall-clock, LRU hit rate).
Update those sections as each measurement lands.

---

## Closed in this revision (2026-10-01)

- **0c — the boundary-context exemption is surfaced.** `edsparser-stats` reports `Internal
  minimum` beside `Minimum` (JSON `internal_min`, CSV `context_internal_min`), context
  statistics are per segment (maximal run of regular symbols), and `vcf2eds` / `eds2leds`
  write the canonical form (no two regular symbols in a row). biofmi checks internal runs
  only. Docs stopped showing an "l-EDS compliant" line the tool never printed.
- **0d — `check_position()`** deleted; see Standing decisions.
- **3b — iterations-to-convergence table** corrected in `docs/performance.md` (1000G: 2
  iterations at every l; Mtb and synthetic: 1).
- **3g — the empty run directories** were `--dry-run` leftovers (xbench materialised dry runs
  under `runs/`), not an aborted batch; all 24 deleted and xbench now dry-runs in a temp dir.
  Details in `experiment_design.md` 3g.
- **The overlap-rule spec question** — closed by detection; see Standing decisions.
- **4b (biofmi) — grouped records keep the source partition**, `d2cef03` (written 2026-09-12
  in the standalone clone, committed 2026-10-01). `test_vcf` 17.
- **Six bugs found by `test_transform_fuzz`**, each with a regression test (`c3dc461`..
  `e470819`): `msa2eds` segfaulted on a one-sequence MSA (`test_msa` 9); `vcf2eds` wrote every
  universal source of a one-sample VCF as `{0,1}`, carried by nobody (`test_vcf` 24);
  `eds2leds` measured a leading run of regular symbols from its second symbol, over-merging and
  breaking block-mode identity (bug 3); compact output dropped an empty regular symbol, so
  `.leds`/`.seds` cardinality broke and block mode lost a whole degenerate symbol (bug 4); a
  transform needing no merge copied EDZ/sparse input sources verbatim into an "SEDS" output,
  and block mode re-spelled untouched entries (bug 5); **above 63 paths a complement ∩
  complement covering every path was kept** as a string no genome carries — 20d8ff1's bug on
  the `PathSet` side (bug 6). Impact on published l-EDS: none (Results that predate fixes).
- **`Sources::merge_adjacent_sources()`** deleted: no caller, and its bitset path still
  expanded complements against an unmasked universe (20d8ff1's bug).
- **Uncalled API** deleted (CLAUDE.md, "Removed API"), including the pipe-streaming headers
  CLAUDE.md said connected the `vcf2eds -l` stages — they never did.
- **Build stamp:** `COMMIT_DATE` is true UTC (`068f8ed`); it used to be local time labelled
  `Z`, two hours fast in summer. Gates written against the old stamps move by −2 h.
- **Test hygiene:** e2e suites refuse a binary that is not the tree under test and count
  skips apart from passes; memory tests report *Skipped*/*Disabled* instead of passing on no
  data; `test_integration` and `test_msa` compare values; `-UNDEBUG` is per test target.
