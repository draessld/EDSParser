# EDSParser – known issues and planned work

Last reprioritised **2026-08-20**, after repairing the unit suite (P0 item 0); before
that, **2026-08-06**, after the complement-source fix (20d8ff1) and the first
M. tuberculosis panels. Items are ordered by what blocks the next result, not by
how interesting they are. Closed items and the reasoning for dropping them are at the
bottom rather than deleted silently.

**The one-line state of things:** the linear merge is correct again and cheap on haploid
data (Mtb, 500 paths, l=100 in 3.3 s at 111 MB once long alleles are filtered), so the open
problems are (a) results produced before the fix that must be regenerated, (b) `vcf2eds`
emitting one full-span haplotype per allele, which makes the EDS grow *superlinearly* with
panel size on any data containing long indels, and (c) diploid/heterozygous data, now the
only surviving cause of combinatorial blow-up in the merge itself.

---

## P0 — correctness; blocks publishable numbers

### 0. Repair the unit test suite — **done 2026-08-20**

`ctest` is 7/7 and e2e is 9/9. Triage answered the question this item asked:
neither `test_stats` nor `test_vcf` was a regression — every failure was a stale
expectation — but repairing them surfaced four real defects, now fixed:

- **`find_symbol_at_common_position()` out-of-bounds read** for a common position
  at or past the total common-character count (`upper_bound` → `end()` →
  `symbol_idx == n`). Segfaulted ~2 runs in 3, reachable from the public
  `check_position()`. Valgrind catches it, ASan does not.
- **Out-of-range degenerate string id** reported as an "Internal error" rather
  than `out_of_range`.
- **Stray `}` absorbed as a sequence character**, so truncated files parsed
  "successfully" with braces inside their strings.
- **`vcf2eds` counted out-of-range variants as processed** and then dropped them,
  reporting 100% success while discarding the tail of a mismatched reference.

Also settled: `total_change_size` counts **characters** in degenerate symbols
(matching its declaration and its display next to `num_common_chars`, which it
now partitions N with) rather than alternatives-beyond-the-first, which was
exactly `m - n`.

Follow-ups this opened, both small and both new items below: 0d (`check_position`
has no caller and so no real specification) and 0e (`vcf2eds` never validates REF
against the reference).

### 0a. Regenerate every result produced at ≤ 63 paths

The bitset fast path in `compute_merge_metadata()` read complement source sets (`{0,e1,e2}`
= all paths except e1,e2) as universal until 20d8ff1, so intersections never pruned and the
merge kept combinations no path carries. It engages only when all path IDs fit in [1,63],
which makes the boundary exact: **≤ 63 samples/sequences affected, > 63 unaffected.**

- **Invalid:** the whole `experiments/results/yeast1011_50` sweep (50 samples), at *every*
  l — not only the rows marked OOM. Those `.leds` files contain strings no strain carries,
  so sizes, string-expansion ratios and peak-memory figures are all measuring the bug.
  Also `experiments/results/tb_100` (collected before the rerun; superseded by `tb_p100`).
- **Valid:** anything at more than 63 paths — `hgp1000` (2504 samples) never took the fast
  path. Also `tb_p100`/`tb_p500` and their `snv50` twins, produced after the fix.
- **Action:** delete or clearly mark the invalid bundles; re-run yeast if it is still wanted
  as a dataset (see 1b — its diploid heterozygosity is a separate problem the fix does not
  solve).

### 0b. Add the regression test that would have caught it — **done**

`test_merge.cpp` has `test_linear_merge_respects_path_count_bound` (3 paths, 6
adjacent degenerate symbols, complement-spelled sources: cartesian would give 64,
correct pruning gives 3) plus `test_universal_marker_still_matches_every_path`,
so a genuinely universal `{0}` is not "fixed" into behaving like a complement.
Both pass. They were present but unreachable while the suite was red, and one
asserted `size()` where it meant `length()`.

### 0c. Surface the boundary-context exemption where users meet it *(mostly resolved)*

`needs_merge()` skips the first and last symbol (`i > 0 && i < n - 1`), so a leading or
trailing common symbol shorter than l survives untouched. Minimal repro:
`{ACGT}{A,C}{160bp}{G,T}{160bp}` at `-l 20` keeps its 4 bp first symbol. Real instance: every
TB panel reports `context_min` 27–31 at l=50 and l=100, because the first variant sits ~30 bp
into H37Rv.

**This is intended design, not a bug** — `docs/README.md` has defined l-EDS as constraining
every *internal* common segment since the beginning, on the grounds that a boundary segment
has a degenerate neighbour on only one side and so is never ambiguous. Interior contexts do
satisfy the invariant.

What remains is presentation: `edsparser-stats` reports a single `context_min` that includes
the boundary symbols, so a correct l-EDS looks like it violates its own constraint. Report
interior minimum separately (or alongside), and confirm biofmi does not assume every context
is ≥ l.

### 0d. `check_position()` has no caller, and so no specification

Nothing in the repo calls it outside `eds.cpp` — presumably biofmi is the intended
consumer. Its tests were the only statement of what it means, and they
contradicted themselves about the coordinate convention (some assertions counted
positions in the expanded string, others in common characters). The
implementation is self-consistent and is now pinned by
`test_check_position_basic`:

- `common_pos` indexes **common characters only**; alternatives inside a
  degenerate symbol occupy no common position, so a match cannot *begin* inside
  one.
- `degenerate_strings` are **global** degenerate-string ids in symbol order, and
  each must belong to a symbol the match traverses (`invalid_argument` otherwise);
  ids for symbols outside the traversed range are warned about and ignored.

Confirm this is the convention biofmi expects before building on it — if it wants
to start a match inside a degenerate symbol, the API cannot express that today.

### 0e. `vcf2eds` never checks REF against the reference

It takes only the *length* of the REF field and reads the allele from the FASTA,
so a VCF whose coordinates belong to a different assembly produces a
well-formed EDS built from the wrong spans, silently. Every position in the
CNV/INV unit fixture was off by one and nothing noticed for as long as the
fixture has existed.

The reference span is already read in order to emit it, so comparing it to REF is
nearly free. Wire it to a counter plus a warning (as 0-fix did for out-of-range
POS) rather than a hard failure, since real VCFs do contain mismatches, and
decide the threshold at which the run should refuse to continue.

---

## P1 — needed before the next round of experiments

### 1a. `vcf2eds` emits one full-span haplotype per (variant, allele) in a group

Discovered 2026-08-06 evaluating the Mtb panels, and it is now the **dominant** cost on
assembly-derived data.

- **Symptom:** `tb_p100` (100 isolates, 4.41 Mb genome) has `N_characters` 48,136,917 —
  11× the genome — of which only 4,121,154 are the common backbone. 91% of the EDS text
  lives inside degenerate symbols.
- **It is superlinear in panel size, and the filter makes it flat.** This is the decisive
  measurement, from the 2026-08-06 panels:

  | dataset | EDS | ctx_avg | l=10 | l=100 |
  |---|---|---|---|---|
  | `tb_p100` | 46.0 MB | 241 bp | 0.6 s / 66 MB | 0.8 s / 69 MB |
  | `tb_p100_snv50` | 4.3 MB | 229 bp | 0.2 s / 8 MB | 0.2 s / 9 MB |
  | `tb_p500` | 689.5 MB | 73 bp | 9.0 s / 744 MB | 25.2 s / 2 092 MB |
  | `tb_p500_snv50` | 4.6 MB | 69 bp | 0.5 s / 14 MB | 3.3 s / 111 MB |
  | `tb_p1141` | **2 990 MB** | 40 bp | 38.2 s / 1 354 MB | 261 s / 3 627 MB |
  | `tb_p1141_snv50` | **5.0 MB** | 39 bp | 1.1 s / 20 MB | killed at 20 GB |

  11.4× the isolates gives **65× the EDS** unfiltered (46 → 2 990 MB, i.e. 3.13 billion
  characters for a 4.41 Mb genome), but **4.3 → 5.0 MB** filtered — essentially constant,
  which is what saturation of variable sites predicts (210 → 107 sites per isolate).
- **The unfiltered branch loses to storing the genomes.** At 1 141 isolates the unfiltered
  l=100 output is **8.2 GB against a 4.69 GB raw panel** — the index is bigger than
  concatenating all 1 141 genomes. The filtered l=50 output is 91.6 MB, 51× *smaller* than
  the panel. There is no reading of these numbers in which the unfiltered representation is
  defensible at scale.
- **Mechanism:** `group_overlapping_variants()` merges every variant whose span overlaps,
  then `merge_variant_group()` builds `merged_haplotypes` by applying **one variant at a
  time** to the whole group span. A long deletion therefore swallows every variant inside
  it and emits one full-span string per allele. Modelled on the equivalent 32-isolate panel:
  predicted 18.8 MB against 18.0 MB actual, with the **top 10 groups accounting for 89% of
  all degenerate text** and a single 19,122 bp group holding 325 variants emitting 334
  haplotypes for 6.4 MB by itself.
- **Note this is also a correctness question**, not only size: because haplotypes are built
  one variant at a time, a sample carrying ALTs at two variants inside the same group is
  recorded as present in *both* single-variant strings rather than in one combined string.
  That is the same "path is in several strings at one symbol" shape as 9c/1b, arising from
  grouping rather than from zygosity.
- **Workaround in place:** `experiments/scripts/make_allele_subset.sh` drops alleles over
  N bp before conversion — 3.9% of sites removed took that EDS from 18.0 MB to 4.3 MB,
  i.e. down to reference size. Good enough to keep experiments moving, but it discards real
  structural variation rather than representing it.
- **Action:** decide whether groups should emit *combined* haplotypes (correct, but that is
  the cartesian product again) or whether long/overlapping variants should be split into
  separate symbols. Until then, document the behaviour and keep publishing both the filtered
  and unfiltered datasets.

### 1b. Haplotype-resolved paths — the only surviving cause of blow-up (was 9c)

With the complement bug fixed, this is what remains. Measured on synthetic 50-path clustered
data: the haploidised VCF runs l=5..30 in ~0.1 s under 19 MB, while its **heterozygous
diploid twin is still killed at an 8 GB cap at every l**. Real confirmation: 1000G chr7 at
l=5 (2504 samples, above the fast-path threshold, so never affected by the bug) genuinely
peaks at 28.3 GiB and expands 17.9×.

- **Cause:** `parse_genotype()` (`vcf_transforms.cpp` ~L340) and the per-sample allele
  collection in `merge_variant_group()` (~L646-677) map each diploid **sample** to a single
  path. Phase is ignored (`0/1` is treated as `0|1`), the two alleles go into a per-sample
  `std::set<int>`, so a het sample is marked present in *both* the reference and the ALT
  string. `intersect_sources()` then rarely returns empty and a chain of k adjacent
  degenerate sites survives up to 2^k combinations.
- **Consequence for interpretation:** traversing one path does not reconstruct a physical
  chromosome. `num_paths` counts samples, not haplotypes.
- **Fix:** split each phased diploid sample into two paths (`2*n_samples`), assigning a0 and
  a1 to separate paths so each path is a consistent single-chromosome walk; keep the
  sample-level collapse as a documented fallback for unphased input. 1000G phase 3 is phased.
- **Why this before the alternatives:** it should remove the 2^k explosion rather than
  bounding it, and it is a modelling correction justified independently of performance.
  Two fallbacks if it does not suffice:
  1. *Do not materialise the product* — keep a merged symbol's strings factored (per-position
     alternatives plus a source-compatibility structure) and expand on read. Changes the
     on-disk l-EDS model, so it is a research contribution rather than an optimisation.
  2. *Accept and document a limit* — publish the tractable-path-count curve and treat
     subsetting as the supported workflow. Honest, but caps what the index can claim about
     population-scale pangenomes.
- **Interim workaround that works today:** haploidise the VCF before conversion (one allele
  per sample). This is what makes the Mtb pipeline cheap and is legitimate for clonal or
  inbred panels; it is a modelling choice, not a fix, for genuinely diploid data.

### 1b-bis. Build biofmi indexes on the TB family *(the actual goal; dataset is ready)*

Everything so far has been dataset preparation. The filtered family — 3 panels × l ∈ {10, 20,
50}, all correct, all under 100 MB, all converging in 1 iteration — is ready to index. Measure
index size, build time and query time against panel size and l. This is the first experiment
that answers the thesis question rather than enabling it.

Confirm 0c first: biofmi must not assume every context is ≥ l.

### 1c. Re-run the path-count curve on the fixed binary

The 2026-08-02 yeast 1011-vs-50 comparison is **void**: the two runs straddled the 63-path
threshold, so it compared a correct run against a buggy one. No magnitude from it survives —
only the mechanism in 1b.

Tooling is ready: `make_sample_subset.sh` (seeded, nested subsets) and `run_subset_dataset.sh`
(each run under a kernel-enforced `MemoryMax` scope). The haploid arm is partly delivered —
`tb_p100` and `tb_p500`, cheap in their filtered form (see the table in 1a; unfiltered they
measure 1a rather than path count) — so what is missing is the **diploid arm**, which is where
the curve actually bends. Note also that Mtb contexts fall faster than expected with panel
size (ctx_avg 241 at 100 paths, 73 at 500), so record `ctx_avg` alongside cost: it, not the
path count alone, is what decides which l values stay meaningful.

Sweep path count × l × dataset, recording peak RSS, runtime, output size and string expansion:
- 50 / 100 / 250 / 500 / all, on 1000G chr21 (small) and chr7 (known-bad), and on yeast;
- l in 3 / 5 / 10 / 20.

Deliverables: the cost-vs-paths curve (2^k, or does it saturate?), the largest tractable path
count per dataset at a given budget, and a calibration set for 1d.

### 1d. `--max-memory` / `--estimate-memory` under-predicts 45× on het VCF data

Measured 2026-08-01 on 1000G chr7, l=5: predicted `RECOMMENDED_BUDGET_BYTES` 0.64 GiB against
an **actual peak of 28.3 GiB**. Unaffected by the complement fix — chr7 has 2504 paths.

- **Root cause:** `estimate_worst_case_merge_memory()` caps each group's `merged_size` at
  `path_cap = num_paths`, which is exactly the invariant 1b breaks. Without the cap the raw
  cartesian bound estimates ~TB for the same input and would refuse everything, so neither
  existing bound is usable.
- **Fix direction:** bound it by *doing* the fold, cheaply and partially — rank groups by
  their cartesian bound, run a counting-only fold over the sources for the top-K (a few
  thousand) tracking only surviving source sets and their count, abort past a limit (1e6) and
  report `≥ limit`, then use the capped bound for the tail.
- **Until then:** do not use `--max-memory` for admission control on VCF-derived input;
  treat `run_leds.sh`'s `ADMIT_BUDGET_GB` as advisory. `MEM_KILL_GB` is the only guard that
  does not depend on prediction quality. Emitting `UNRELIABLE=1` whenever any group's
  cartesian product exceeds `path_cap` would at least stop schedulers trusting a guess.
- **`--block-size` does not help here:** it bounds the per-symbol index, not one group's
  merge metadata. A 2^k-combination group is the same size however the file is cut.

---

## P2 — performance and scale

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
  all. Needs per-format sequential decode (text SEDS brace scan, sparse bitvec, EDZ fixed-width
  records, EDZ_COMPRESSED in block order). A true ceiling for linear mode too.

Also worth documenting for users: barrier availability is data-dependent — at large l, or in
dense variation, barriers thin out and blocks grow, with a clean whole-file fallback when none
exist.

### 2b. Sequential-reader rewrite *(lower priority — block mode got the ceiling more cheaply)*

Every phase already walks positions left-to-right (`select_merge_groups()`, `compute_merge_metadata()`,
`MergeStreamWriter`'s monotone cursor). The per-symbol index exists only because `read_symbol(pos)`
is a random-access API; a sequential reader with a bounded lookahead (`needs_merge` needs at most
`context_length` characters ahead) would make an iteration O(batch + lookahead) instead of O(n).

Blockers: `EDS::from_metadata()` hands the next iteration a full metadata struct; `compute_merge_metadata()`
parallelises over groups indexing metadata by absolute position and would need position-relative
slices; the raw-copy pass-through computes byte spans from `base_positions`, which a sequential
reader knows as it parses. Shares its core with 2c.

### 2c. Single-pass `vcf2eds -l`

Today `vcf2eds -l N` is two-stage: `parse_vcf_to_leds_streaming_direct()` writes the full stage-1
EDS/SEDS to temp files, then runs `eds_to_leds_linear()` over them. Temp files rather than a pipe
are required because the merge consumes its input to EOF before emitting output, so `make_pipe()`
would deadlock.

Two possible shapes: fuse the chain-merge state into the VCF walk (needs a windowed convergence
proof, since the linear pipeline converges over full-file iterations), or buffer per VCF block plus
a carryover window large enough to cover any chain crossing the boundary. Value: removes the
stage-1 disk write and the reparse. Must stay byte-identical to the two-stage output; `--keep-eds`
would bypass it.

### 2d. The merge pipeline still writes only dense text SEDS internally

*Partly addressed.* `eds2leds --source-format {seds,seds-sparse,edz,edz-sparse,edz-compressed}`
re-encodes once at the end via `Sources::save_as()`, so the final artifact can be compact
(edz-compressed measured 4.7× smaller than SEDS at 500 paths, 5.9× at 2504). But the pipeline
writes dense text SEDS for every iteration and for the pre-conversion output, so **peak disk is
unchanged** and the conversion costs an extra pass. `vcf2eds -l` has no equivalent flag at all.

To close it: give `stream_merged_symbols_to_file()` / the SEDS batching path a sparse and EDZ
writer, mirroring `write_seds_sparse_finalize()` / `write_edz_entry()`.

### 2e. Re-measure the source formats — the deciding factor is allele frequency, not path count

The guidance "EDZ pays off above a few hundred paths" is **wrong**, and wrong in a way that
gets worse with scale. A bitset costs ⌈paths/8⌉ bytes per entry whatever it contains, so it
wins only when entries are dense. Measured on the clonal TB panels, where most variants are
rare:

| Paths | text SEDS | `gzip -6` | EDZ sparse |
|---:|---:|---:|---:|
| 100 | 464 KB | **57 KB** | 529 KB |
| 500 | 2.98 MB | **345 KB** | 8.36 MB |
| 1 141 | 7.83 MB | **1.02 MB** | 34.07 MB |

At 1 141 paths sparse EDZ is **4.4× larger than the text it replaces** — a variant carried by
3 isolates is 143 bytes as a bitset and ~12 as `{5,88,900}` — while gzip beats every built-in
format by ~8× at every size. The opposite ranking on 1000G-like data (edz-compressed 4.7-5.9×
smaller than SEDS) is real too; the two datasets differ in allele-frequency spectrum, not path
count.

Actions: (1) add `edz-compressed` and gzip to `collect_results.sh`, which currently only tries
`--to edz --sparse`, and re-measure all three panels — edz-compressed on rare-variant data is
simply unmeasured; (2) once the picture is complete, either document the rule properly or pick
the format automatically from the observed mean set density rather than from `num_paths`.

---

### 2f. Pin down the l / ctx_avg law

Output expansion is a clean function of **l divided by the average context**, not of l alone
`[measured, TB panels]`:

| Panel | ctx_avg | l=10 | l=20 | l=50 | l=100 |
|---|---:|---|---|---|---|
| 100 | 229 bp | 0.04 → 1.0× | 0.09 → 1.0× | 0.22 → 1.1× | 0.44 → 1.5× |
| 500 | 69 bp | 0.14 → 1.0× | 0.29 → 1.2× | 0.72 → 2.5× | 1.44 → 13.7× |
| 1 141 | 39 bp | 0.26 → 1.1× | 0.51 → 1.7× | 1.28 → 18.3× | 2.56 → OOM |

Free below ~0.3, ~1.7× at 0.5, non-linear past 1.0. Since contexts shorten as a panel grows,
the usable l *shrinks* with panel size. This is already the practical rule in the docs
(keep l ≲ ctx_avg/2), but it rests on 11 points from one organism.

Sweep l at fixed ratios of each panel's ctx_avg (0.1, 0.25, 0.5, 0.75, 1.0, 1.5) so the three
curves collapse onto one, and check the collapse holds on a second organism. The filtered
datasets are ~5 MB, so the whole sweep costs minutes and produces a figure that tells any user
how to choose l.

---

## P3 — documentation and benchmark debt

Numbers in `docs/performance.md` that are theoretical, or measured on unspecified hardware.
Several are now contradicted by real runs, which makes them worse than missing.

### 3a. Document benchmark hardware *(still undone; still the cheapest unblocker)*

"Benchmark Baseline (2026-05 Reference System)" names no machine, so no number in the file is
reproducible. Re-run `bench.sh --size standard` on a documented machine and add a Hardware section.
`bench_plot.py` already embeds machine info in plot footers.

### 3b. Correct the iterations-to-convergence table *(measurement now exists)*

`performance.md` claims 2-4 iterations for MSA-derived and **3-6 for VCF-derived** data. Real runs
disagree: hgp1000 converges in **2** iterations on every chromosome at l=5, and Mtb and yeast both
converge in **1** at every l tested. The chain-merging change (2026-06-28) is why. This is now a
docs edit, not an experiment.

### 3c. Replace the projected memory table with measured numbers

The old-vs-new table (100×-3000× reduction) is projected from architecture analysis. Real figures
now exist — 2380 → 540/80 MB under block mode at 31.7M symbols, chr7 at 28.3 GiB, the full hgp1000
run — and should replace it, with the machine documented.

### 3d. Compressed disk-footprint comparison for 1000G *(was 9a)*

Two gaps: the charts compare VCF vs EDS vs SEDS and ignore the reference FASTA, when the fair
accounting is `vcf` vs `eds + seds + ref`; and 1000G ships as `.vcf.gz`, so the honest comparison is
`vcf.gz` vs `eds.gz + seds.gz + ref.gz`. **SEDS dominates the output (~33 GB vs ~3.4 GB EDS)**, so it
is the thing to shrink; quantify EDZ_COMPRESSED against `seds.gz` here. `hgp1000/*/other/` currently
holds only `*_disk_size.txt` — no gzip measurements — while the newer bundles from
`collect_results.sh` capture them automatically, so re-collecting hgp1000 may be most of the work.

### 3e. Remaining unmeasured claims

- **OpenMP thread scaling** (was #5): the 2026-06-14 contention fix preloads sources so workers never
  take `io_mutex_`; scaling vs `--threads 1,2,4,8,16` on a fixed 1 GB input has never been measured.
- **LRU hit rate** (was #6): "~98%" is asserted, not measured. Instrument `read_source()` behind a
  compile-time flag and vary capacity 1K/10K/100K.
- **MSA throughput** (was #2): "50-200 MB/s" and the scalability table are pure calculation; no
  `msa2eds` benchmark exists. Vary alignment length and sequence count.
- **VCF block-size trade-off** (was #3): the table is a memory formula only, with no wall-clock. Sweep
  1M/5M/10M/50M/100M for peak RSS and runtime.
- **Linear vs cartesian on real data** (was #7): the 1.26× ratio is from synthetic round-robin data.
  Real data has different alternative counts and source distributions; also quantify how much linear's
  pruning shrinks the output. **Runner exists, results do not**: `run_subset_dataset.sh` now runs both
  arms per l (`MODES`, default both) into `leds_l<N>/` and `leds_l<N>_cart/`, records every attempt in
  `leds_runs.tsv` so a cartesian run killed at `MEM_CAP` is a recorded outcome, and
  `experiments/compare_merge_modes.py` joins the two into string/byte/runtime/RSS ratios. Re-run the TB
  matrix to fill it in. Expect the cartesian arm to hit the cap first at high l on the dense panels —
  that cell is the measurement, not a gap. Note the two arms are not symmetric on disk: linear must
  carry a `.seds` that is often larger than its `.leds`, so at low l cartesian can be the smaller
  artifact while being the wrong language.

### 3f. chr21 is ~1.7× faster than chr22 at identical input size *(was 9b)*

11 GB VCF each, 1,105,538 vs 1,103,547 variants, ~1.10M groups each, yet 1017.9 s vs 1772.6 s with
similar peak memory — a per-variant runtime gap, not a memory effect. Re-run both in isolation with
`/usr/bin/time -v` to rule out IO contention first (most likely if chromosomes ran concurrently);
if it persists, check N-content/block distribution and multiallelic/indel density, then profile.

---

## Closed since the last revision

- **Complement source sets read as universal in the merge** — fixed in 20d8ff1. 50-path haploid VCF
  at l=5: 6,557,335 strings / 3159 MB / 21.2 s → 2,491 strings / max 50 per symbol / 5.9 MB / 0.01 s.
  A 40-sequence 3 kb MSA at l=20 went from killed at 6 GB to 9.5 MB. Consequences tracked in 0a.
- **"Path count is THE bottleneck"** — retired as a headline. The measurement behind it was void
  (see 1c) and the mechanism is more precisely heterozygosity, not path count as such: 500 haploid
  Mtb paths are cheap. What survives is 1b.
- **`vcf2eds` sources are sample-level, not haplotype-level** — the documentation half is done
  (recorded in CLAUDE.md); the modelling half is 1b.
- **`ctx_run_len` n-entry vector** — replaced by `CtxRunCursor` (2026-07-30). Removed a 4 B/symbol
  transient; did not move peak RSS, since that allocation never overlapped the peak.
- **`--source-format` for `eds2leds` output** — shipped 2026-07-30. The pipeline-internal half remains
  as 2d.
- **`-z` with `-l` writing SEDS text into a `.edz` file** — fixed 2026-07-02; now warns and falls back
  to honest dense-text naming instead of producing an unreadable file.
- **First real-data 1000G run** (was #9) — done: 84.8M variants, 99.98% processed, 37 h, peak ~2.5 GB.
  Follow-ups live on as 3d and 3f.
- **Convergence on real genomic data** (was #1) — measured; the remaining work is the docs edit in 3b.
