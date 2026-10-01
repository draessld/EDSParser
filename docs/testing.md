# Testing Guide

EDSParser has four layers of testing:

| Layer | What it tests | Speed | Auto via `ctest`? |
|-------|--------------|:-----:|:-----------------:|
| **Unit** | Core C++ library logic | < 30 s | ✅ Yes |
| **Integration** | End-to-end CLI tool workflows (in-process) | < 60 s | ✅ Yes |
| **End-to-end (e2e)** | Shell invocations of installed CLIs | ~1 min | ❌ Manual |
| **Memory** | Streaming memory bounds and leak detection | 1–30 min | smoke: ✅ (Skipped without data); stress: Disabled unless `-DEDSPARSER_CTEST_MEMORY_STRESS=ON` |

---

## 1. Unit Tests

> **All seven unit tests build and pass (repaired 2026-08-20).** They had drifted
> far enough that `test_eds` no longer compiled and four others aborted on their
> first assertion; repairing them turned up four real defects, including an
> out-of-bounds read in `find_symbol_at_common_position()` that segfaulted about
> two runs in three. See CLAUDE.md for what was wrong and what it found.

### Running

```bash
# ctest runs from build/src/cpp, where CTestTestfile.cmake lives
cd build/src/cpp && ctest --output-on-failure

# The executables themselves are written to build/tools/

# Run a single executable
./test_eds
./test_sources
./test_merge
./test_msa
./test_vcf
./test_stats
./test_transform_fuzz                         # ~20 s, seeded
TRANSFORM_FUZZ_SOAK=600 ./test_transform_fuzz # long run on fresh seeds
```

### Test Files

| Executable | Source | Coverage |
|------------|--------|---------|
| `test_eds` | `tests/unit/test_eds.cpp` | EDS parsing (full + compact format), symbol access, pattern generation, `extract()`, cardinality validation |
| `test_sources` | `tests/unit/test_sources.cpp` | Sources load/save, `read_source()`, `read_source_ref()`, LRU cache eviction, `intersect_sources()` (also over the EDZ bitset backend), thread safety |
| `test_merge` | `tests/unit/test_merge.cpp` | Symbol merge: CARTESIAN and LINEAR strategies, empty alternatives, source intersection, merge metadata |
| `test_msa` | `tests/unit/test_msa.cpp` | MSA parsing, streaming output, source tracking, gap handling, single-sequence edge case |
| `test_vcf` | `tests/unit/test_vcf.cpp` | VCF parsing: SNPs, indels, `<DEL>`, `<INS>`, `<INV>`, `<CN0..N>`, multi-allelic, overlap merging (each genome on exactly one alternative of a merged group), block-based |
| `test_stats` | `tests/unit/test_stats.cpp` | Statistics computation, context length bounds, SEDS cardinality check |
| `test_integration` | `tests/unit/test_integration.cpp` | Complete tool workflows using `std::stringstream` (no disk I/O) |
| `test_transform_fuzz` | `tests/unit/test_transform_fuzz.cpp` | Seeded differential fuzzing: `eds2leds` linear/cartesian/block, `msa2eds`, `vcf2eds` (vs `bcftools consensus`), parser round trips, all against brute-force path expansion; failures print seed + minimised reproducer. Properties and knobs in CLAUDE.md § Differential fuzzing |

### Writing Unit Tests

Tests use `<cassert>` (no external framework). Typical pattern:

```cpp
#include <cassert>
#include <sstream>
#include "formats/eds.hpp"

void test_basic_parse() {
    using namespace edsparser;
    EDS eds = EDS::from_string("ACGT{A,C}GT");
    assert(eds.length() == 3);
    assert(eds.cardinality() == 4);   // ACGT, A, C, GT
    const auto& degen = eds.get_metadata().is_degenerate;
    assert(!degen[0]);
    assert(degen[1]);
    assert(!degen[2]);
}

int main() {
    test_basic_parse();
    // ...
    return 0;
}
```

---

## 2. Integration Test (`test_integration`)

`test_integration` runs complete pipeline workflows in-process using
`std::stringstream` for all I/O, so no installed tools or filesystem
access is needed.

Covered workflows:
- MSA → EDS → l-EDS (LINEAR and CARTESIAN)
- VCF + FASTA → EDS
- VCF + FASTA → l-EDS
- `eds_to_leds_linear` with phasing output
- Statistics collection
- Pattern generation

Run with:

```bash
cd build/tools && ./test_integration
```

---

## 3. End-to-End (Shell) Tests

Shell tests invoke the **installed CLI tools** against reference input
files and compare output to expected files stored under
`tests/e2e/expected/`.

### Prerequisites

Tools must be installed:

```bash
./INSTALL.sh        # or ensure ~/.local/bin is on PATH
```

### Running

```bash
# All suites
bash tests/e2e/run_all.sh

# Individual suites
bash tests/e2e/test_msa2eds.sh
bash tests/e2e/test_vcf2eds.sh
bash tests/e2e/test_eds2leds.sh
bash tests/e2e/test_leds_incremental.sh
bash tests/e2e/test_source_transform.sh
bash tests/e2e/test_seds_edz.sh
bash tests/e2e/test_stats.sh
bash tests/e2e/test_genpatterns.sh
bash tests/e2e/test_genrandomeds.sh
```

| Suite | What it covers |
|-------|----------------|
| `test_msa2eds.sh` | MSA → EDS/l-EDS, source output, argument errors |
| `test_vcf2eds.sh` | VCF → EDS/l-EDS, variant types, `--block-size`, `--keep-eds`, `-z`, argument errors |
| `test_eds2leds.sh` | Linear/cartesian merge, `-s`/`-z` sources, `--max-memory` (exit 3), `--source-format`, `--block-size` byte-identity vs whole-file, `--estimate-memory` |
| `test_leds_incremental.sh` | Idempotence/monotonicity: building l=B from an l=A l-EDS is byte-identical to building l=B from the raw EDS |
| `test_source_transform.sh` | `edsparser-source-transform` SEDS ↔ EDZ conversions and `--verify` |
| `test_seds_edz.sh` | SEDS/EDZ format robustness — trailers, sparse variants, complement entries, misnamed files |
| `test_stats.sh` | `edsparser-stats` output formats, source-aware stats, `-s`/`-z` |
| `test_genpatterns.sh` | Pattern generation |
| `test_genrandomeds.sh` | Synthetic EDS generation |

> **The suites test the build tree, and refuse a binary that is not it**
> (`resolve_tool()` in `tests/e2e/helpers.sh`, 2026-10-01). Tools come from
> `build/tools/` (`EDSPARSER_TOOLS_DIR` overrides) and never fall back to `PATH`;
> `EDSPARSER_TOOLS_FROM_PATH=1` tests installed binaries deliberately. Each tool's
> path and `--version` are printed, and the suite exits 1 if its `COMMIT` is not
> `git rev-parse HEAD`, its `DIRTY` flag disagrees with the work tree, or (both
> dirty) a modified tracked file is newer than it. Rebuild, or opt out loudly
> with `EDSPARSER_ALLOW_STALE_TOOLS=1`. A missing tool is a failure;
> `EDSPARSER_ALLOW_MISSING_TOOLS=1` turns a missing *optional* tool into skips.

Each suite prints individual results as PASS / FAIL / SKIP and a per-suite
`passed, failed, skipped` count; a test skips by returning 77 (`skip "reason";
return`), and a skip is never counted as a pass. `run_all.sh` totals all three
across suites and names the skip count when it is non-zero.

### Test Data

| File | Description |
|------|-------------|
| `tests/e2e/data/simple.eds` | Tiny EDS with 2 degenerate symbols |
| `tests/e2e/data/small.eds` | Small EDS derived from `small.msa` |
| `tests/e2e/data/small.msa` | 3-sequence MSA alignment |
| `tests/e2e/data/small.fa` | Reference FASTA for VCF tests |
| `tests/e2e/data/small.vcf` | VCF with SNPs and small indels |
| `tests/e2e/data/small.seds` | Sources for `small.eds` |
| `tests/e2e/data/test_overlaps.vcf` | VCF with overlapping variants |
| `tests/e2e/data/test_iterative*.eds` | EDS requiring multiple l-EDS iterations |
| `tests/e2e/data/test_compact_input.eds` | EDS in compact format |

### Expected Output Files

Reference outputs live under `tests/e2e/expected/<tool>/`. When fixing
a bug, update the expected files to match the corrected output.

### Expected Result

Every e2e test is expected to pass. The suites once carried deliberately
failing tests documenting a compact-output formatting bug; that bug is fixed
and those tests are gone, so any failure now is a real regression (or a stale
tool on `PATH` — see the note above).

---

## 4. Memory Tests

Their input is generated (`tests/stress/generate_quick_data.sh`,
`generate_data.sh`), not committed. With no input present they exit **77** and
ctest, through `SKIP_RETURN_CODE`, reports them as *Skipped* — before
2026-10-01 `test_memory_smoke` exited 0 having tested nothing. A run where only
some sizes exist exits 0 and prints how many were skipped.
`test_memory_stress` is registered with ctest but Disabled unless configured
with `-DEDSPARSER_CTEST_MEMORY_STRESS=ON`.

### test_memory_smoke (Quick, ~1–2 min)

Validates that all streaming operations stay within 2 GB peak and do not
exhibit memory growth, using 10–50 MB input files.

```bash
cd build/tools && ./test_memory_smoke
```

Checks:
- `genrandomeds` (10 MB): peak < 50 MB
- `msa2eds` streaming (20 MB MSA): peak < 200 MB
- `eds_to_leds_linear` (10 MB EDS): peak < 500 MB
- `eds_to_leds_cartesian` (10 MB EDS): peak < 500 MB
- LRU cache: no growth over 100 sequential iterations

### test_memory_stress (Full, ~30+ min)

Tests with 100–500 MB files, uses linear regression on periodic memory
samples to detect leaks.

```bash
cd build/tools && ./test_memory_stress
```

Checks (threshold: 1.0 MB/sec growth rate):
- `eds_to_leds_linear` (100 MB EDS, 100 iterations): no leak
- `eds_to_leds_cartesian` (100 MB EDS, 100 iterations): no leak
- `Sources` LRU cache (500 MB SEDS): no leak
- MSA streaming (500 MB alignment): no leak

### Using MemoryMonitor in Your Tests

```cpp
#include <edsparser/memory_monitor.hpp>

MemoryMonitor mon(1.0);  // 1-second sampling interval
mon.start();
mon.add_label("before transform");

// ... run the operation under test ...

mon.add_label("after transform");
mon.stop();

double peak   = mon.get_peak_memory_mb();
double growth = mon.get_memory_growth_mb();          // first sample to last
bool   leaked = mon.detect_memory_leak(2.0);         // 2 MB/sec threshold

assert(peak < 500.0);
assert(growth < 10.0);
assert(!leaked);
```

(`assert_memory_below()` and `assert_no_memory_growth()` were removed on
2026-10-01 — no test ever called them; assert on the accessors directly.)

---

## 5. Benchmarks

Quick reference:

```bash
bash tests/bench/bench.sh --size quick     # ~30 s
bash tests/bench/bench.sh --size standard  # ~5 min (default)
bash tests/bench/bench.sh --size large     # ~25 min

bash tests/bench/bench_compare.sh          # regression check vs baseline
```

---

## Test Coverage Summary

| Scenario | Test |
|----------|------|
| EDS compact format parsing | `test_eds` |
| EDS full format parsing | `test_eds` |
| Empty alternative (deletion) | `test_eds`, `test_vcf` |
| Adjacent degenerate symbols | `test_merge`, e2e |
| l-EDS boundary segments (short context at start/end) | `test_eds`, e2e |
| MSA gap columns | `test_msa` |
| MSA single-sequence edge case | `test_msa` |
| VCF SNP | `test_vcf` |
| VCF small indel | `test_vcf` |
| VCF `<DEL>` | `test_vcf` |
| VCF `<INV>` (reverse complement) | `test_vcf` |
| VCF `<CN0..N>` | `test_vcf` |
| VCF multi-allelic site | `test_vcf` |
| VCF overlapping variants | `test_vcf` |
| LINEAR merge preserves sources | `test_merge` |
| CARTESIAN merge produces cross-product | `test_merge` |
| Source intersection `{0}` universal marker | `test_sources` |
| LRU cache eviction | `test_sources` |
| Thread-safe concurrent read_source() | `test_sources` |
| cardinality mismatch detection | `test_eds` |
| EDS→l-EDS convergence (2 iterations) | e2e `test_eds2leds.sh` |
| EDS→l-EDS iterative (multi-iteration) | `test_iterative*.eds` |
| edsparser-stats JSON output | e2e `test_stats.sh` |
| edsparser-stats CSV output | e2e `test_stats.sh` |
| genrandomeds reproducibility (`--seed`) | e2e `test_genrandomeds.sh` |
| Path-by-path genome preservation through every transform | `test_transform_fuzz` |
| `--block-size` byte identity, all source formats | `test_transform_fuzz`, e2e `test_eds2leds.sh` |
| vcf2eds genomes vs `bcftools consensus` | `test_transform_fuzz` |
| Memory stability (streaming) | `test_memory_smoke` |
| Memory leak detection (linear regression) | `test_memory_stress` |
