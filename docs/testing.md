# Testing Guide

EDSParser has four layers of testing:

| Layer | What it tests | Speed | Auto via `ctest`? |
|-------|--------------|:-----:|:-----------------:|
| **Unit** | Core C++ library logic | < 30 s | ✅ Yes |
| **Integration** | End-to-end CLI tool workflows (in-process) | < 60 s | ✅ Yes |
| **End-to-end (e2e)** | Shell invocations of installed CLIs | ~1 min | ❌ Manual |
| **Memory** | Streaming memory bounds and leak detection | 1–30 min | ❌ Manual |

---

## 1. Unit Tests

> **⚠️ The unit suite does not build or pass at HEAD (2026-08-11).** The library
> and CLI tools build fine and the e2e suites pass 100%, but the unit tests have
> drifted from the code: `test_eds` does not compile (`.count()` on `PathSet`,
> now a `std::vector<int>`; plus two tests referencing an `eds` whose constructor
> line is commented out), and `test_sources`, `test_stats`, `test_merge` and
> `test_vcf` each fail on their first assertion. `test_msa` and
> `test_integration` pass. See CLAUDE.md for the per-test diagnosis.

### Running

```bash
# From edsparser/build/tools after build
ctest --output-on-failure

# Run a single executable
./test_eds
./test_sources
./test_merge
./test_msa
./test_vcf
./test_stats
```

### Test Files

| Executable | Source | Coverage |
|------------|--------|---------|
| `test_eds` | `tests/unit/test_eds.cpp` | EDS parsing (full + compact format), symbol access, position checking, `check_position()`, `extract()`, cardinality validation |
| `test_sources` | `tests/unit/test_sources.cpp` | Sources load/save, `read_source()`, `read_source_ref()`, LRU cache eviction, `intersect_sources()`, `merge_adjacent_sources()`, thread safety |
| `test_merge` | `tests/unit/test_merge.cpp` | Symbol merge: CARTESIAN and LINEAR strategies, empty alternatives, source intersection, merge metadata |
| `test_msa` | `tests/unit/test_msa.cpp` | MSA parsing, streaming output, source tracking, gap handling, single-sequence edge case |
| `test_vcf` | `tests/unit/test_vcf.cpp` | VCF parsing: SNPs, indels, `<DEL>`, `<INS>`, `<INV>`, `<CN0..N>`, multi-allelic, overlap merging, block-based |
| `test_stats` | `tests/unit/test_stats.cpp` | Statistics computation, context length bounds, SEDS cardinality check |
| `test_integration` | `tests/unit/test_integration.cpp` | Complete tool workflows using `std::stringstream` (no disk I/O) |

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

> **The suites run whichever tool `PATH` finds first**, falling back to
> `build/tools/` only when the name is not on `PATH` (`find_tool()` in
> `tests/e2e/helpers.sh`). A stale `~/.local/bin` copy will therefore fail tests
> for flags it predates. Run `make install`, or prefix the run with
> `PATH="$PWD/build/tools:$PATH"`, before concluding a failure is real.

Each suite prints individual test results and a per-suite pass/fail count.
`run_all.sh` prints an overall summary.

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

// Peak memory assertion
assert_memory_below(500.0, "l-EDS transform");

// No-growth assertion (< 10 MB total growth allowed)
assert_no_memory_growth(mon, 10.0, "l-EDS transform");

// Detailed analysis
double peak = mon.get_peak_memory_mb();
bool leaked = mon.detect_memory_leak(2.0);  // 2 MB/sec threshold
```

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
| Memory stability (streaming) | `test_memory_smoke` |
| Memory leak detection (linear regression) | `test_memory_stress` |
