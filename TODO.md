# EDSParser – known issues and planned work

---
## Planned Features

### Single-pass `vcf2eds -l` (one-phase VCF→l-EDS, no intermediate EDS on disk)

- **Today:** `vcf2eds -l N` is a **two-stage** pipeline — `parse_vcf_to_leds_streaming_direct()`
  (`src/cpp/lib/transforms/vcf_transforms.cpp` ~L1246) writes the full stage-1 VCF→EDS/SEDS to temp
  files, then runs `eds_to_leds_linear(temp_eds, ..., temp_seds, ...)` over them. Temp files (not a
  pipe) are required because `eds_to_leds_linear()` consumes its EDS input to EOF before emitting
  output and copies its inputs into its own temp dir — a `make_pipe()` producer/consumer would
  deadlock. So the entire intermediate EDS+SEDS is materialised to disk even though the user only
  wants the l-EDS.
- **Goal:** a **one-phase** path that produces l-EDS directly from the VCF stream without writing the
  whole intermediate EDS, ideally keeping VCF's block/streaming memory profile. Two possible shapes:
  - **Fused merge in the VCF walk:** carry the l-EDS chain-merge state (`select_merge_groups()` /
    `compute_merge_metadata()`) alongside variant-group generation so merged symbols are emitted as
    the reference is scanned. Hard part: merging needs bounded lookahead across adjacent degenerate
    runs and the linear pipeline currently converges over *full-file* iterations — a streaming
    version needs a windowed convergence proof (or a bounded-context guarantee) so it can commit
    output without a second pass.
  - **Bounded-window buffering:** merge within each VCF processing block plus a carryover window
    large enough to cover any chain that could still merge across the block boundary, flushing settled
    prefixes. Reuses the existing block machinery; only defers a small tail per block instead of the
    whole file.
- **Value:** removes the stage-1 EDS/SEDS disk write (relevant for the 100 GB+ population-VCF runs in
  §9 where SEDS dominates footprint) and cuts VCF→l-EDS wall-clock by avoiding the reparse.
- **Notes / constraints:** must stay behavior-identical to the two-stage output (add a byte-for-byte
  e2e check against the current path); `--keep-eds` explicitly wants the intermediate materialised, so
  that flag would bypass the fused path. Interacts with the *"l-EDS merge output never writes sparse
  or EDZ sources"* item below — a fused writer should honor the requested source format from the start.

### Path count is THE bottleneck — run the sample-count experiment, then decide what to do about it

This is the headline finding of 2026-08-01/02 and it reframes the memory work: peak RAM
of the linear (phasing-aware) merge is governed by the **number of source paths**, not by
genome size, and it grows super-linearly.

- **The 2026-08-02 sample-count measurement is void and must be redone.** It compared
  yeast chromosome1 at 1011 samples (killed at a 20 GB cap) against 50 samples
  (43.7 MB, 0.34 s) and concluded that a 20x cut in paths bought ~500x in memory. The
  two runs did not execute the same code: above 63 paths the merge falls back to the
  `PathSet` implementation, which handles complement source sets correctly, while at or
  below 63 paths it took the bitset fast path, which until 20d8ff1 read every complement
  set as universal and therefore never pruned. So the 1011-sample run was correct and the
  50-sample run was not — the comparison says nothing reliable about path count. Redo the
  ladder on the fixed binary before drawing the curve. The *direction* (more paths => more
  het carriers => more surviving combinations) still follows from the mechanism below, but
  no measured magnitude survives.
- **Unaffected by that bug:** 1000G chr7 at l=5 (2504 samples, 179 MB EDS) peaks at
  **28.3 GiB** and expands 17.9x — above 63 paths, so it ran the correct path. This
  remains the reference case for the genuine explosion. Yeast is *denser* per bp than
  human (1 variant / 7 bp vs 1 / 38 bp) with chains averaging 5.9 symbols vs 3.0, so a
  small genome is not automatically a cheap one.
- **Still real after the fix:** on synthetic 50-path data with clustered variants, the
  haploidised VCF runs l=5..30 in ~0.1 s under 19 MB while its heterozygous diploid twin
  is still killed at an 8 GB cap at every l. Heterozygosity, not path count alone, is what
  drives the surviving-combination count.
- **Mechanism** (see 9c): paths are sample-level, so a het sample sits in *both* the ref
  and the alt string at a site; `intersect_sources()` almost never returns empty and a
  chain of k adjacent degenerate sites survives up to 2^k combinations. More samples =>
  more het carriers => fewer empty intersections => more surviving combinations.

#### The experiment to run

Tooling is ready: `experiments/scripts/make_sample_subset.sh` (seeded, *nested* subsets so
the ladder is a fair comparison; also drops now-monomorphic sites with `-c1 -a`) and
`experiments/scripts/run_subset_dataset.sh` (sequential, every run wrapped in a
kernel-enforced `MemoryMax` scope, so a blow-up is a logged FAILED line rather than a dead
machine).

Sweep **path count x l x dataset** and record peak RSS, runtime, output size, and the
input->output string expansion ratio:
- path counts 50 / 100 / 250 / 500 / all, on 1000G chr21 (small) and chr7 (known-bad),
  and on yeast chromosome1 + chromosome4;
- l in 3 / 5 / 10 / 20.

Deliverables: a cost-vs-paths curve (is it 2^k, or does it saturate?), the largest
tractable path count per dataset at a given memory budget, and a calibration set for
fixing `--estimate-memory`.

**Run it on 20d8ff1 or later.** Every existing result at <= 63 paths predates the
complement fix and measures the bug rather than the data — including the whole
`experiments/results/yeast1011_50` sweep (50 samples), at *every* l and not only the
rows marked OOM: those l-EDS files contain strings no strain carries, so their sizes,
string-expansion ratios and peak-memory figures are all invalid. Regenerate before
using any of it. Results at more than 63 paths (hgp1000, 2504 samples) are unaffected.

#### What to do about it — the actual research question

The experiment measures the problem; it does not solve it. Options, roughly in order of
how fundamental they are:

1. **Haplotype-resolved paths (9c).** Split each phased diploid sample into two paths so a
   path is a single-chromosome walk. Intersections would then prune properly (a haplotype
   takes exactly one allele per site), which should remove the 2^k explosion rather than
   merely bounding it. This is a *modelling* change with a scientific justification
   independent of performance, and it is the most likely real fix. 1000G phase 3 is phased,
   so the data supports it; unphased inputs would need a documented fallback.
2. **Do not materialise the product.** The blow-up is in *representing* every surviving
   combination explicitly in `MergeMetadata`. A merged symbol whose strings are a filtered
   cartesian product could stay implicit (factored per position + a source-compatibility
   structure) and be expanded only on read. This changes the on-disk l-EDS model, so it is
   a research contribution in its own right, not an optimisation.
3. **Accept and document a limit.** Publish the tractable-path-count curve and treat
   sample subsetting as the supported workflow. Cheapest, and honest, but it caps what the
   index can claim about population-scale pangenomes.

Option 1 first (it may make 2 unnecessary), with the experiment above as the before/after
measurement in either case.

### `--max-memory` / `--estimate-memory` under-predict 45x on VCF data (path cap is invalid)

- **Measured 2026-08-01** on 1000G chr7, l=5, binary 6fbe246, threads=1:
  predicted `RECOMMENDED_BUDGET_BYTES` 0.64 GiB, **actual peak 28.3 GiB**. Output
  3.21 GB `.leds` + 6.5 GB `.seds` from a 179 MB `.eds` (17.9x expansion), 55.5M output
  strings from ~20M input strings, converged in 1 iteration, 356 s. The whole gap is the
  merge term, predicted at 4 MB.
- **Root cause:** `estimate_worst_case_merge_memory()` caps each group's `merged_size` at
  `path_cap = num_paths`, assuming a linear merge cannot produce more strings than there
  are paths. False for VCF-derived sources, which are **sample-level, not
  haplotype-level** (item 9c below): a het sample is marked present in *both* the ref and
  the alt string at a site, so `intersect_sources()` rarely returns empty and a chain of k
  adjacent degenerate sites survives up to 2^k combinations. chr7 iter 0: 218,016
  adjacent-degenerate groups.
- **Neither existing bound works:** with the cap the estimate is 45x low; without it the
  raw cartesian product estimates ~TB for the same input and would refuse everything.
- **Fix direction:** the surviving-combination count is data-dependent, so bound it by
  *doing* the fold, cheaply and partially:
  1. Rank groups by their cartesian bound; the peak batch is dominated by the largest.
  2. For the top-K (K ~ a few thousand) run a **counting-only fold** over the sources —
     the same iterative intersection as `compute_merge_metadata()` but tracking only the
     surviving source sets and their count, with an abort once the count passes a limit
     (e.g. 1e6) so estimating never itself blows up. Report `>= limit` in that case.
  3. Use exact counts for those groups and the (capped) bound for the tail.
- **Until then:** do not use `--max-memory` for admission control on VCF-derived input and
  treat `run_leds.sh`'s `ADMIT_BUDGET_GB` as advisory. `MEM_KILL_GB` is the only guard that
  does not depend on prediction quality. Emitting an explicit `UNRELIABLE=1` flag from
  `--estimate-memory` whenever any group's cartesian product exceeded `path_cap` would at
  least stop schedulers trusting a number that is known to be a guess.
- **Note `--block-size` does not help this case:** it bounds the per-symbol index, not a
  single group's merge metadata. A 2^k-combination group is the same size however the file
  is cut. chr7 needs ~30 GB with or without block mode.

### eds2leds memory ceiling — block mode lands, sources index is the remaining floor

- **Status (2026-07-30), two steps done:**
  1. *Constant-factor work:* ~139 → ~74 bytes per input symbol (uint64 byte offsets
     instead of `std::streampos`, lazy `cum_*` arrays, exact `reserve`/`shrink_to_fit`).
     Whole-file `-l 10` linear: 322→162 MB at 1.98M symbols, 4404→2380 MB at 31.7M.
     This shrank the constant but left peak RSS **linear in file size**.
  2. *`--block-size` block mode:* the actual ceiling. Barrier cuts (a common run
     totalling ≥ l can never be crossed by a merge) let each block be merged in
     isolation with the unchanged merge core, output byte-identical to whole-file.
     On the 949 MB / 31.7M-symbol input at `-l 10`: 2380 MB whole-file → 1071 MB
     (200M blocks) → 644 MB (50M) → 540 MB (10M) linear, and **80 MB (10M blocks)
     cartesian** — the cartesian figure is independent of file size, i.e. a genuine
     ceiling. Cost: ~2.7× wall-clock (54 s → 145 s), which is the intended trade.
- **Remaining floor (linear mode only): the whole-file `Sources` index.** Block mode
  loads one `Sources` for the entire input to slice each block's entries, costing
  8 B/string — 509 MB for that input's 63.7M strings, which is exactly the 540 MB
  measured. Blocks below ~50 MB therefore stop helping. Two ways out:
  - *Streaming source slice:* one sequential pass over the sources file writing each
    block's slice, with no index at all. Needs per-format sequential decode: text SEDS
    (brace scan), sparse text (presence bitvec, m/8 bytes — bounded), EDZ dense (fixed
    `⌈paths/8⌉` records, pure arithmetic), EDZ sparse (bitvec + records), EDZ_COMPRESSED
    (decompress blocks in order). Gives a true ceiling for linear mode too.
  - *Sampled index:* keep every 16th–32nd entry offset and scan forward (8 → 0.25-0.5
    B/string, so 509 MB → 16-32 MB). Less code, touches the hot `read_source` /
    `copy_range_to_stream` paths, and helps every tool rather than just block mode.
- **Block mode limits worth documenting for users:** cut availability is data-dependent
  — at large l, or in regions of dense variation, barriers thin out and blocks grow
  (with a clean whole-file fallback when none exist). Real 1000G data at l=10 has
  barriers everywhere (average context 40-60 bp).
- **What still scales with n in whole-file mode:** input `EDS::Metadata` (~23 B/symbol: `base_positions` 8,
  `symbol_sizes` 4, `cum_set_sizes` 4, `string_lengths` 4/string), the output metadata
  built inline by `MergeStreamWriter` (~19 B/symbol), the `Sources` byte-offset index
  (8 B/string), and the `groups` vector (~16 B/group).
- **Sequential-reader rewrite (still open, now lower priority — block mode gets the
  ceiling far more cheaply):** every phase of an iteration already walks positions
  strictly left-to-right — `select_merge_groups()` scans in order, `compute_merge_metadata()`
  consumes groups in order, `MergeStreamWriter` has a monotone `cursor_`. The per-symbol
  index exists only because `EDS::read_symbol(pos)` is a random-access API. A sequential
  symbol reader (parse forward, track the stream offset, keep a bounded lookahead window
  — `needs_merge` needs at most `context_length` characters of context ahead) would let an
  iteration run in O(batch + lookahead) instead of O(n).
- **Blockers to work through:** (1) `EDS::from_metadata()` hands the next iteration a full
  metadata struct — a streaming design must instead re-stream the temp file; (2)
  `compute_merge_metadata()` parallelises over groups via OpenMP and indexes metadata
  arrays by absolute position — it would need per-batch, position-relative slices; (3)
  the raw-copy pass-through computes byte spans from `base_positions[pos+1] - base_positions[pos]`,
  which a sequential reader knows anyway as it parses. Interacts with the single-pass
  `vcf2eds -l` item above — both want the same streaming merge core.
- **Done 2026-07-30:** `ctx_run_len` is no longer an n-entry vector — `CtxRunCursor`
  measures each common run on the fly during `select_merge_groups()`'s existing
  left-to-right walk. This removed a 4 B/symbol transient (127 MB at 31.7M symbols) but
  did not move peak RSS, since that allocation did not overlap the peak.
- **Next cheapest real win:** the `Sources` byte-offset index (8 B/string, ~13 B/symbol —
  roughly 18% of what remains). Because source reads are also overwhelmingly sequential,
  a *sampled* index (every 16th entry plus a short forward scan) would cut it to
  0.5 B/string. `copy_range_to_stream()`'s exact-byte-range fast path is the part to be
  careful with.

### l-EDS (`-l`) merge output never writes sparse or EDZ sources *(partly addressed)*

- **Addressed 2026-07-30 for `eds2leds`:** `--source-format {seds,seds-sparse,edz,edz-sparse,edz-compressed}`
  re-encodes the merged sources once at the end via `Sources::save_as()` and deletes the
  text file, so the *final artifact* can now be compact — edz-compressed measured 4.7×
  smaller than SEDS at 500 paths and 5.9× at 2504 paths. What is described below is still
  true of the pipeline itself: it writes dense text SEDS for every iteration and for the
  pre-conversion output, so peak *disk* is unchanged and the conversion costs one extra
  pass. `vcf2eds -l` still has no equivalent flag.

- **Location:** `eds_to_leds_linear()` in `src/cpp/lib/transforms/eds_transforms.cpp` (both the
  stream-based and path-based overloads) — the merged-output sources writer at ~L735-745 is a
  hardcoded `*sources_out << '{' ... '}'` loop, and reload uses `Sources::load(temp_seds_out,
  Sources::Format::SEDS)` unconditionally (~L1066).
- **Problem:** `vcf2eds -l N` (two-stage VCF→EDS→l-EDS) and `eds2leds -l N` both always write their
  merged output `.seds` as dense text SEDS, regardless of the input source format. Before
  2026-07-02 this silently mislabeled the file: `vcf2eds -l N -z` opened the output in binary mode
  and named it `.edz`, but wrote plain SEDS text into it — an unreadable, corrupted file. Fixed by
  forcing dense-text naming/mode whenever `create_leds` is true and printing a warning when `-z` is
  given together with `-l` (see `vcf2eds.cpp`); `eds2leds` already named its merge output `.seds`
  honestly, so no corruption there — just an undocumented always-downgrades-to-SEDS limitation.
- **Idea:** give `stream_merged_symbols_to_file()` / the SEDS batching path a sparse and/or EDZ
  writer (mirroring `Sources::write_seds_sparse_finalize()` / `write_edz_entry()`) so `-l` mode can
  actually honor the requested source format instead of always falling back to dense SEDS. The
  EDZ_COMPRESSED writer (`Sources::save_edz_compressed()`, shipped 2026-07-07) is a candidate target
  once the merge pipeline can emit binary sources.

---

## Experiments / Validation

All items below correspond to numbers in `docs/performance.md` that are either theoretical estimates
or measured only on synthetic/unspecified hardware. Each experiment should produce a plot or table
that replaces the placeholder claim. Start with #8 (hardware docs) to make all other results
reproducible.

### 8. Document hardware for all benchmarks *(do first)*

The "Benchmark Baseline (2026-05 Reference System)" table in `performance.md` does not specify the
machine. All numbers are therefore not reproducible.

Re-run `bench.sh --size standard` on a documented machine (CPU model, core count, RAM, storage
type). Add a "Hardware" section to `performance.md`. Use `bench_plot.py` — it now embeds machine
info in plot footers automatically.

### 5. OpenMP thread-count scaling *(measure improvement from concurrency fix)*

The `--threads` mutex contention fix (2026-06-14) preloads all source sets before the parallel
region so workers never take `Sources::io_mutex_`. Throughput scaling with thread count has not yet
been measured post-fix.

Benchmark `eds2leds -s file.seds --threads 1,2,4,8,16` on a fixed 1 GB input. Plot throughput vs
thread count before and after the fix. Identify the saturation point and confirm linear speedup in
the low-thread regime.

### 6. LRU cache hit rate measurement *(motivates [MEM] std::set fix)*

`performance.md` claims "~98% hit rate" but this is asserted, not measured. The claim also notes
"> 100 GB SEDS → 99% with 100K cache" — also unmeasured.

Instrument `Sources::read_source()` with a hit/miss counter (behind a compile-time flag). Run
`eds2leds --linear` on real data with varying cache capacities (1K, 10K, 100K). Record actual hit
rate. Update the table.

### 4. eds2leds memory validation on large files

The old-vs-new memory table (100×–3000× reduction) is projected from the architecture analysis, not
from actually running 1 GB / 10 GB / 100 GB inputs.

Run `eds2leds` (cartesian and linear) on inputs of 100 MB, 1 GB, 10 GB with `MemoryMonitor` or
`/usr/bin/time -v`. Record actual peak RSS and compare to the formula. Update the table with real
numbers and document the machine used.

### 1. Verify eds2leds convergence claim on real genomic data

`docs/algorithms.md` and `performance.md` state iteration counts for MSA-derived (2–4) and
VCF-derived (3–6) data. Never validated — only synthetic EDS tested.

Run `eds2leds` on representative real datasets (e.g. 1000 Genomes VCF→EDS, human MSA) and record
iteration count per l value. Update both files.

### 7. Linear vs Cartesian on real genomic data

The 1.26× cartesian/linear throughput ratio was measured on synthetic data (10% variability, 4-path
round-robin, `genrandomeds`). Real VCF/MSA data may have different alternative counts and source
distributions.

Run both modes on real data. Record throughput ratio and output size ratio (linear prunes invalid
paths — quantify the compression).

### 2. MSA throughput and memory vs alignment size / sequence count

`performance.md` claims "50–200 MB/s" and the scalability table is pure calculation. No actual
`msa2eds` benchmark exists.

Benchmark: vary alignment length (1 MB, 10 MB, 100 MB, 1 GB) and sequence count (100, 1K, 10K) —
measure runtime, throughput (MB/s), and peak RSS. Confirm the memory formula matches observed values.

### 3. VCF block size trade-off: memory vs throughput

The block-size table in `performance.md` is theoretical (memory formula only). No throughput or
wall-clock measurements exist for different `--block-size` values.

Benchmark `vcf2eds` on a real or large synthetic VCF with block sizes 1M, 5M, 10M (default), 50M,
100M — measure peak RSS and runtime. Produce a plot of memory vs block-size and time vs block-size.

### 9. First real-data results — 1000 Genomes phase 3 (hs37d5) VCF→EDS

First full run over all 24 chromosomes (evaluation notebook:
`experiments/results/vcf2eds/vcf2eds_evaluation.ipynb`). Headline: 84.8M variants, 99.98%
processed (only ~16.8K unsupported mobile-element/MT insertions skipped), 37 h wall-clock,
peak RSS ~2.5 GB. Two follow-ups fall out of the results:

#### 9a. Disk-footprint comparison is incomplete + EDS/SEDS need compression

- **Missing reference in the comparison.** The disk-footprint charts compare VCF vs EDS vs SEDS but
  ignore the reference FASTA. The fair accounting is **`vcf` vs `eds + seds + ref`** — an EDS is only
  reconstructable *with* its reference, so the reference belongs on the EDS side of the ledger.
  Add `ref_disk_size.txt` (already captured) into the comparison and totals.
- **Compression is the real open question.** Against the *raw* VCF (808 GB) EDS+SEDS (~36.5 GB) is
  ~22× smaller, but 1000G is distributed as **`.vcf.gz`**, not raw VCF — and gzipped VCF is roughly
  15–20× smaller, i.e. comparable to or smaller than the current uncompressed EDS+SEDS. So the honest
  claim ("EDS+SEDS may take *more* disk than the compressed VCF") only holds once both sides are
  compressed. **SEDS dominates the EDS output (~33 GB vs ~3.4 GB EDS)** — it's the thing to shrink.
- **Action:** measure the compressed comparison — `vcf.gz` vs `eds.gz + seds.gz (+ ref.gz)` — and
  decide whether plain gzip on the EDS/SEDS is enough or whether a purpose-built compact form is
  needed. Note EDZ_COMPRESSED (zstd, implemented 2026-07-07) already addresses the *sources* side;
  quantify EDZ_COMPRESSED SEDS vs `seds.gz` here, and consider a compressed EDS encoding for the
  string side.

#### 9b. Why is chr21 ~1.7× faster than chr22 despite identical size? (notebook §5.2)

chr21 and chr22 are near-identical in input (11 GB VCF each; 1,105,538 vs 1,103,547 variants;
~1.10M variant groups each) yet chr21 finished in **1017.9 s** vs chr22's **1772.6 s** —
~1090 vs ~623 variants/s. Peak memory is similar (1467 vs 1377 MB), so it's a runtime-per-variant
gap, not a memory effect. Candidate explanations to check:
- **System/IO contention during the run** (VCFs stream from `raid_storage`; overlapping jobs or disk
  contention would inflate chr22 alone) — most likely if chromosomes were processed concurrently.
- **Reference N-content / block distribution** — chr21 and chr22 are both acrocentric with large
  heterochromatic/masked stretches; differing N-runs change how many of the 200 kb blocks carry
  variants.
- **Variant complexity** — multiallelic site density or indel-length distribution differing between
  the two would change per-variant work even at equal counts.
- **Action:** re-run chr21 and chr22 in isolation (no parallel load) with `/usr/bin/time -v` to rule
  out contention first; if the gap persists, profile to find where the extra time goes.

#### 9c. Paths are sample-level, not haplotype-resolved — phase & zygosity are dropped

- **Location:** `parse_genotype()` (`src/cpp/lib/transforms/vcf_transforms.cpp` ~L340) and
  `merge_variant_group()` per-sample allele collection (~L646-677); source assignment comment at
  ~L748 (*"each sample gets one path ID"*), `path_id = sample_id + 1`, `total_paths = n_samples`.
- **What happens today:** each **diploid sample maps to a single path**, not to its two chromosome
  copies. Phasing is explicitly ignored — `parse_genotype` treats `0/1` the same as `0|1` (the `|`/`/`
  delimiter is only used to split the string). A sample's two alleles are dumped into a per-sample
  `std::set<int>`, so allele order is lost and a heterozygous sample is recorded as present in
  *multiple* allele-strings at the same site (a `0/1` het marks both the reference and the ALT string;
  a `1/1` hom marks only the ALT). Copy number is not represented at all.
- **Consequence:** the EDS is **genotype/sample-level, not haplotype-specific**. Traversing one path
  does *not* reconstruct a single physical chromosome — wherever the sample is heterozygous the path
  belongs to several strings at once, so a path is a *set of alleles a sample carries*, not a phased
  walk. `num_paths` and the paths-per-string stats therefore count samples, not haplotypes, which
  matters when interpreting §7 structural stats and any pangenome-graph comparison. (Note `0/1` and
  `1/1` are not byte-identical in the SEDS — het additionally marks reference-allele membership — but
  neither is phase- or dosage-aware.)
- **Idea / action:** decide whether haplotype-resolved paths are a goal. If so, split each phased
  diploid sample into **two paths** (`2*n_samples`), assigning allele *a0* and *a1* to separate paths
  so each path is a consistent single-chromosome walk; keep the current sample-level collapse as a
  fallback for unphased data (and warn/skip when phasing is required but genotypes are unphased).
  Until then, document in CLAUDE.md that `vcf2eds` sources are sample-level, not haplotype-level.
