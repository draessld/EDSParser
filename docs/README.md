# EDSParser Documentation

> **Version 1.0.0** — C++ library and CLI suite for Elastic-Degenerate Strings

## Contents

| Document | Description |
|----------|-------------|
| [formats.md](formats.md) | File format reference — EDS, SEDS, MSA, VCF, l-EDS |
| [cli-tools.md](cli-tools.md) | Complete CLI tool reference for all six executables |
| [library-api.md](library-api.md) | C++ public API reference (EDS, Sources, transforms) |
| [architecture.md](architecture.md) | Internal design: streaming, two-phase loading, pipe buffers |
| [algorithms.md](algorithms.md) | Algorithm deep-dives: merging, MSA 3-pass, VCF blocks, l-EDS iteration |
| [performance.md](performance.md) | Memory profiles, throughput numbers, tuning guidelines |
| [testing.md](testing.md) | Unit, integration, e2e, memory and benchmark test suites |
| [experiments.md](experiments.md) | Experiment scripts (moved to biofmi/experiments/) |

---

## What is EDSParser?

EDSParser is a **high-performance C++ library and command-line suite** for working with
*Elastic-Degenerate Strings* (EDS) — a compact representation of sequence variation that
encodes multiple alternative strings at each position, used as a data structure for
pangenomic applications.

An EDS encodes a population of sequences as a single string of *symbols*, where each symbol
is an ordered set of alternative strings:

```
ACGT{A,ACA}CGT{T,TG,}AACG
```

Here positions 4–6 and 11–12 are *degenerate* (they carry multiple alternatives including,
in the second case, an empty string representing a deletion).

---

## Quick Start

### Install

```bash
./INSTALL.sh          # builds + installs to ~/.local/
```

### Core Workflow

```bash
# 1. Start from a Multiple Sequence Alignment (FASTA with gaps)
msa2eds -i alignment.msa -l 10 -o data.leds   # → l-EDS + sources directly

# — or from a VCF —
vcf2eds -i variants.vcf --reference genome.fasta -l 10

# — or generate synthetic data —
genrandomeds --ref-size-mb 100 --variability 0.10 --min-context 10 --seed 42 -o data.eds
# then:
eds2leds -i data.eds -s data.seds -l 10

# 2. Inspect what you produced
edsparser-stats -i data.leds --csv

# 3. Generate patterns for benchmarking downstream tools
edsparser-genpatterns -i data.leds -o patterns.txt -n 1000 -l 20
```

---

## Concepts Glossary

| Term | Definition |
|------|------------|
| **EDS** | Elastic-Degenerate String — sequence of sets of strings |
| **Symbol** | One element of the EDS sequence; either *common* (single string) or *degenerate* (≥2 alternatives) |
| **l-EDS** | EDS where every internal common segment has length ≥ l |
| **Context** | A non-degenerate (common) symbol in the EDS |
| **Source / Path** | A concrete sequence (e.g. one haplotype) that is consistent with the EDS |
| **SEDS** | Source EDS — companion file tracking which strings belong to which paths |
| **LINEAR merge** | Phasing-aware merge: only valid haplotype combinations are kept |
| **CARTESIAN merge** | All-combinations merge: cross-product of alternatives |
| **Cardinality `m`** | Total number of strings across all symbols |
| **`n`** | Number of symbols |
| **`N`** | Total number of characters |

---

## Tool Summary

| Tool | Input | Output | Purpose |
|------|-------|--------|---------|
| `msa2eds` | `.msa` (FASTA + gaps) | `.eds` / `.leds` + `.seds` | MSA → EDS or l-EDS |
| `vcf2eds` | `.vcf` + `.fasta` | `.eds` / `.leds` + `.seds` | VCF → EDS or l-EDS |
| `eds2leds` | `.eds` + optional `.seds` | `.leds` + `.seds` | EDS → l-EDS |
| `edsparser-stats` | `.eds` / `.leds` | stdout / `.csv` / `.json` | Statistics + compliance |
| `edsparser-genpatterns` | `.eds` / `.leds` | `.txt` patterns | Pattern generation |
| `genrandomeds` | parameters | `.eds` + `.seds` | Synthetic data generation |

---

## Repository Layout

```
edsparser/
├── src/cpp/
│   ├── lib/                    # Core C++ library (libedsparser_lib.a)
│   │   ├── common.hpp/cpp      # Types, Timer, memory helpers
│   │   ├── formats/
│   │   │   ├── eds.hpp/cpp     # EDS class
│   │   │   └── sources.hpp/cpp # Sources class (provenance)
│   │   ├── transforms/
│   │   │   ├── eds_transforms.hpp/cpp     # EDS → l-EDS
│   │   │   ├── msa_transforms.hpp/cpp     # MSA → EDS/l-EDS
│   │   │   ├── vcf_transforms.hpp/cpp     # VCF → EDS/l-EDS
│   │   │   └── eds_complexity.cpp         # Complexity estimator
│   │   ├── memory_monitor.hpp/cpp         # Memory diagnostics
│   │   ├── pipe_buffer.hpp                # Thread-safe circular streambuf
│   │   └── pipe_stream.hpp                # PipeOutputStream/InputStream
│   └── tools/
│       ├── msa2eds.cpp         # CLI: MSA → EDS/l-EDS
│       ├── vcf2eds.cpp         # CLI: VCF → EDS/l-EDS
│       ├── eds2leds.cpp        # CLI: EDS → l-EDS
│       ├── stats.cpp           # CLI: edsparser-stats
│       ├── genpatterns.cpp     # CLI: edsparser-genpatterns
│       └── genrandomeds.cpp    # CLI: genrandomeds
├── tests/
│   ├── unit/                   # C++ unit tests (ctest)
│   ├── e2e/                    # Shell end-to-end tests
│   ├── stress/                 # Stress test data generators
│   └── bench/                  # Performance benchmarks
├── docs/                       # ← You are here
├── CMakeLists.txt
├── INSTALL.sh
└── README.md
```
