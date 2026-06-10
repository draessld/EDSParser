# C++ Library API Reference

EDSParser exposes a C++17 library (`libedsparser_lib.a`) installed under
`~/.local/`. Integrate it with CMake:

```cmake
find_package(EDSParser REQUIRED)
target_link_libraries(my_target PRIVATE EDSParser::EDSParser)
```

All public symbols live in the `edsparser` namespace. Headers are included as:

```cpp
#include <edsparser/common.hpp>
#include <edsparser/formats/eds.hpp>
#include <edsparser/formats/sources.hpp>
#include <edsparser/transforms/eds_transforms.hpp>
#include <edsparser/transforms/msa_transforms.hpp>
#include <edsparser/transforms/vcf_transforms.hpp>
#include <edsparser/memory_monitor.hpp>
```

---

## common.hpp — Fundamental Types

### Type Aliases

```cpp
using String    = std::string;
using StringSet = std::vector<String>;
using Position  = uint64_t;   // 0-based symbol or character index
using Length    = uint32_t;   // String or context length
```

### File Extension Constants

```cpp
constexpr const char* EXT_MSA  = ".msa";
constexpr const char* EXT_VCF  = ".vcf";
constexpr const char* EXT_EDS  = ".eds";
constexpr const char* EXT_SEDS = ".seds";
constexpr const char* EXT_LEDS = ".leds";
constexpr const char* EXT_EDZ  = ".edz";
constexpr const char* EXT_EDP  = ".edp";
```

### Timer

```cpp
class Timer {
public:
    Timer();
    void start();
    void stop();
    double elapsed_seconds() const;
    double elapsed_milliseconds() const;
    double elapsed_microseconds() const;
};
```

### Memory Helper

```cpp
// Returns peak RSS in MB from /proc/self/status (Linux only; 0.0 elsewhere)
double get_peak_memory_mb();
```

### Error Codes

```cpp
enum class ErrorCode {
    SUCCESS = 0,
    FILE_NOT_FOUND = 1,
    INVALID_FORMAT = 2,
    INVALID_PARAMETER = 3,
    BUILD_FAILED = 4,
    QUERY_FAILED = 5,
    UNKNOWN_ERROR = 99
};
```

---

## EDS Class (`formats/eds.hpp`)

The central data structure. Move-only (copy is deleted because of the
internal file stream).

### Construction

```cpp
// From a stream (no sources)
EDS eds(some_istream);

// From a string literal (convenience)
EDS eds = EDS::from_string("{A,C}GT{T,}");

// From file (metadata + optional full load)
EDS eds = EDS::load("/path/to/data.eds");
EDS eds = EDS::load("/path/to/data.eds", "/path/to/data.seds");

// From pre-computed metadata (avoids re-parsing; see streaming use-cases)
EDS eds = EDS::from_metadata(std::move(meta), n, m, N, file_path);
```

`EDS::load()` always uses streaming (METADATA_ONLY) mode internally;
the `from_string()` / stream constructor paths store all strings in RAM.

### Core Queries

```cpp
bool   empty()       const;   // true if constructed from empty stream
size_t length()      const;   // n — number of symbols
size_t size()        const;   // N — total characters
size_t cardinality() const;   // m — total number of strings

const EDS::Metadata& get_metadata() const;
EDS::Statistics      get_statistics() const;
```

### Metadata Structure

```cpp
struct EDS::Metadata {
    // Index
    std::vector<std::streampos> base_positions;  // file offset per symbol
    std::vector<Length> symbol_sizes;             // strings-per-symbol (length n)
    std::vector<Length> string_lengths;           // char length per string (length m)
    std::vector<Length> cum_set_sizes;            // cumulative string IDs
    std::vector<bool>   is_degenerate;            // true if symbol has ≥2 alternatives

    // Statistics
    Length min_context_length;
    Length max_context_length;
    double avg_context_length;
    size_t num_degenerate_symbols;
    size_t num_common_chars;
    size_t total_change_size;
    size_t num_empty_strings;

    // Position-check support (for downstream locate() implementations)
    std::vector<Position> cum_common_positions;   // length n+1
    std::vector<int>      cum_degenerate_counts;  // length n+1
};
```

### Streaming Access

These methods work in **both** FULL and METADATA_ONLY modes:

```cpp
// Read all strings of symbol at position pos
StringSet read_symbol(Position pos) const;

// Per-string metadata accessors (O(1), no disk I/O)
Length        get_symbol_size(Position pos)       const;
std::streampos get_base_position(Position pos)    const;
Length        get_string_length(size_t string_id) const;
const std::vector<bool>& get_is_degenerate()      const;
```

### Legacy In-Memory Access

```cpp
// Only valid when constructed from a string or stream (not from file)
// Throws in METADATA_ONLY mode
const std::vector<StringSet>& get_sets() const;
```

### Source Access

```cpp
bool   has_sources() const;

// Delegates to Sources::read_source() — works in both modes
std::set<int> read_source(size_t string_id) const;

// Direct object access for advanced operations
std::shared_ptr<Sources> get_sources_object() const;
void set_sources_object(std::shared_ptr<Sources> sources);

// Cache tuning
void set_source_cache_capacity(size_t capacity);
void clear_source_cache() const;
```

### Output

```cpp
enum class EDS::OutputFormat { FULL, COMPACT };

void print(std::ostream& os = std::cout) const;
void save(std::ostream& os, OutputFormat fmt = OutputFormat::FULL) const;
void save(const std::filesystem::path& path, OutputFormat fmt) const;
```

### Pattern Generation

```cpp
void generate_patterns(std::ostream& os, size_t count, Length pattern_length) const;
```

Writes `count` random patterns of `pattern_length` characters to `os`, one
per line. Patterns follow actual EDS paths.

### Position Checking

```cpp
bool check_position(
    Position        common_pos,
    const std::vector<int>& degenerate_strings,
    const String&   pattern) const;
```

Returns `true` if `pattern` occurs at `common_pos` using the given
degenerate string choices. Used by downstream locate oracles in tests.

---

## Sources Class (`formats/sources.hpp`)

Manages provenance (source path tracking) for EDS strings.
Shared via `std::shared_ptr`. Thread-safe for concurrent reads.

### Construction

```cpp
// Preferred: factory loader (auto-detects format)
std::shared_ptr<Sources> src = Sources::load("/path/to/data.seds");

// Explicit format
std::shared_ptr<Sources> src = Sources::load("/path/data.seds", Sources::Format::SEDS);
```

### Accessing Sources

```cpp
// Returns by value — safe for concurrent calls and parallel code
std::set<int> read_source(size_t string_id) const;

// Returns a const reference into the LRU cache — faster on cache hit,
// but ONLY safe in single-threaded contexts (reference may be invalidated
// if another thread evicts the entry)
const std::set<int>& read_source_ref(size_t string_id) const;
```

**Rule:** Always use `read_source()` in multithreaded / OpenMP code.

### Bulk Copy

```cpp
// Write the raw SEDS bytes for strings [start_idx, start_idx+count) to out.
// Much faster than calling read_source() in a loop — one seek + sequential
// read, no std::set allocation per entry.
void copy_range_to_stream(size_t start_idx, size_t count, std::ostream& out) const;
```

### Source Merging (Set Operations)

```cpp
// Intersection of two source sets; handles the {0} universal marker
static std::set<int> intersect_sources(
    const std::set<int>& sources1,
    const std::set<int>& sources2);

// All valid merged source sets for two adjacent symbols
std::vector<std::set<int>> merge_adjacent_sources(
    size_t symbol1_start, size_t symbol1_size,
    size_t symbol2_start, size_t symbol2_size) const;
```

`merge_adjacent_sources()` throws `std::runtime_error` if all intersections
are empty (no valid haplotype traverses the merged symbol).

### Cache Management

```cpp
size_t cardinality() const;              // m — total strings indexed
void set_cache_capacity(size_t capacity); // default: 10 000 entries
void clear_cache();
```

Cache size guidelines:
- `< 100 K strings` → FULL mode or cache all (set to `m`)
- `100 K – 10 M strings` → 10 K–100 K entries
- `> 10 M strings` → 100 K–1 M entries

---

## EDS Transform Functions (`transforms/eds_transforms.hpp`)

### Complexity Estimation

```cpp
struct TransformComplexity {
    size_t adjacent_degenerate_pairs;
    size_t short_contexts;
    double avg_degenerate_cluster_size;
    bool   warn_slow;
    bool   warn_exponential;
    std::string recommendation;
};

TransformComplexity estimate_leds_complexity(const EDS& eds, size_t context_length);
```

Call this before `eds_to_leds_*` to check for potential exponential growth.

### LINEAR Merge (Phasing-Aware)

```cpp
void eds_to_leds_linear(
    std::istream& input,
    std::ostream& output,
    Length        context_length,
    std::istream* phasing_input  = nullptr,  // .seds stream
    std::ostream* phasing_output = nullptr,  // output .seds stream
    size_t        num_threads    = 1,
    bool          compact        = true      // compact vs full format
);
```

Merges adjacent symbols until every internal common segment reaches
`context_length` characters. Preserves valid haplotypes only.

### CARTESIAN Merge (All Combinations)

```cpp
void eds_to_leds_cartesian(
    std::istream& input,
    std::ostream& output,
    Length        context_length,
    size_t        num_threads = 1,
    bool          compact     = true
);
```

Like linear merge but generates every possible combination (cross-product).
No source information needed; no source output produced.

---

## MSA Transform Functions (`transforms/msa_transforms.hpp`)

### EDS Output

```cpp
void parse_msa_to_eds_streaming(
    std::istream& msa_stream,
    std::ostream& eds_out,
    std::ostream& seds_out);
```

### l-EDS Output

```cpp
void parse_msa_to_leds_streaming(
    std::istream& msa_stream,
    std::ostream& eds_out,
    std::ostream& seds_out,
    size_t        context_length);
```

Both functions are **streaming** — only the reference sequence and bit
vectors are kept in RAM during the transformation.

---

## VCF Transform Functions (`transforms/vcf_transforms.hpp`)

### Statistics Struct

```cpp
struct VCFStats {
    size_t total_variants     = 0;
    size_t processed_variants = 0;
    size_t skipped_malformed  = 0;
    size_t skipped_unsupported_sv = 0;
    size_t variant_groups     = 0;  // after overlap merging

    size_t total_skipped() const { return skipped_malformed + skipped_unsupported_sv; }
};
```

### VCF → EDS (Streaming File Output — Recommended)

```cpp
void parse_vcf_to_eds_streaming(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    std::ostream& eds_output,
    std::ostream& seds_output,
    VCFStats*     stats      = nullptr,
    size_t        block_size = 10'000'000);
```

Writes EDS and SEDS incrementally per genomic block. Prefer this for large
files.

### VCF → EDS (String Return — Convenience)

```cpp
std::pair<std::string, std::string> parse_vcf_to_eds_streaming_str(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    VCFStats*     stats      = nullptr,
    size_t        block_size = 10'000'000);
```

Returns `{eds_string, seds_string}`. Accumulates full output in RAM —
only use for small inputs or tests.

### VCF → l-EDS (Streaming File Output — Recommended)

```cpp
void parse_vcf_to_leds_streaming_direct(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    std::ostream& leds_output,
    std::ostream& seds_output,
    size_t        context_length,
    VCFStats*     stats      = nullptr,
    size_t        block_size = 10'000'000);
```

Two-stage pipeline: VCF→EDS (temp file) → l-EDS (output). Temp files are
cleaned up automatically on completion or exception.

### VCF → l-EDS (String Return)

```cpp
std::pair<std::string, std::string> parse_vcf_to_leds_streaming(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    size_t        context_length,
    VCFStats*     stats      = nullptr,
    size_t        block_size = 10'000'000);
```

⚠️ Accumulates full output in RAM. Use only for small inputs or unit tests.

---

## MemoryMonitor (`memory_monitor.hpp`)

For diagnostics and test validation.

```cpp
class MemoryMonitor {
public:
    explicit MemoryMonitor(double sample_interval_sec = 1.0);

    void start();
    void stop();
    void add_label(const std::string& label);    // tagged sample point

    std::vector<MemorySample> get_samples() const;
    double get_peak_memory_mb() const;
    double get_average_memory_mb() const;
    double get_memory_growth_mb() const;

    // Linear regression on samples; threshold = MB/sec growth rate
    bool detect_memory_leak(double threshold_mb_per_sec = 1.0) const;
};

// Test assertions
void assert_memory_below(double max_mb, const std::string& context);
void assert_no_memory_growth(const MemoryMonitor& monitor,
                             double max_growth_mb,
                             const std::string& context);
```

---

## Minimal Complete Example

```cpp
#include <edsparser/formats/eds.hpp>
#include <edsparser/formats/sources.hpp>
#include <edsparser/transforms/eds_transforms.hpp>
#include <fstream>
#include <iostream>

int main() {
    using namespace edsparser;

    // --- Load EDS with sources ---
    EDS eds = EDS::load("data.eds", "data.seds");
    auto stats = eds.get_statistics();
    std::cout << "Symbols: " << eds.length()
              << "  Strings: " << eds.cardinality()
              << "  Min context: " << stats.min_context_length << "\n";

    // --- Stream first 5 symbols ---
    for (size_t i = 0; i < std::min(size_t(5), eds.length()); ++i) {
        auto sym = eds.read_symbol(i);
        std::cout << "Symbol " << i << ": {";
        for (size_t j = 0; j < sym.size(); ++j) {
            if (j) std::cout << ",";
            std::cout << sym[j];
        }
        std::cout << "}\n";
    }

    // --- EDS → l-EDS (linear, l=10) ---
    std::ifstream eds_in("data.eds");
    std::ifstream seds_in("data.seds");
    std::ofstream leds_out("output_l10.leds");
    std::ofstream seds_out("output_l10.seds");

    eds_to_leds_linear(eds_in, leds_out, 10, &seds_in, &seds_out);

    return 0;
}
```
