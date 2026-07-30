#ifndef EDSPARSER_EDS_HPP
#define EDSPARSER_EDS_HPP

#include "../common.hpp"
#include "sources.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <list>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <memory>

namespace edsparser {

/**
 * Elastic-Degenerate String (EDS) representation
 *
 * An EDS is a sequence where each position can contain multiple alternative strings.
 * Format: {str1,str2,...}{str3}{str4,str5}...
 * Compact format (optional): str1{str2,str3}str4 (brackets only on degenerate symbols)
 * Empty strings are represented as empty entries between commas.
 *
 * Uses streaming architecture with metadata/index for memory-efficient access.
 * Strings are read on-demand from disk rather than loaded into RAM.
 */
class EDS {
public:
    // Output format options
    enum class OutputFormat {
        FULL,     // Always use brackets: {ACGT}{A,ACA}{CGT}
        COMPACT   // Omit brackets on non-degenerate: ACGT{A,ACA}CGT
    };

    // Default constructor
    EDS() : is_empty_(true), sources_(nullptr) {}

    // Stream-based constructor (EDS only, no sources support)
    explicit EDS(std::istream& eds_stream);

    // String-based constructor (EDS only, for convenience - wraps streams internally)
    explicit EDS(const std::string& eds_string);

    // File-based loaders (always uses streaming for memory efficiency)
    static EDS load(const std::filesystem::path& path);
    static EDS load(const std::filesystem::path& eds_path, const std::filesystem::path& seds_path);

    // Convenience factory for string construction
    static EDS from_string(const std::string& eds_string);

    // Destructor
    ~EDS() = default;

    // Copy and move constructors/assignments
    // Note: Copy is deleted because of ifstream member (non-copyable in METADATA_ONLY mode)
    EDS(const EDS&) = delete;
    EDS& operator=(const EDS&) = delete;
    EDS(EDS&&) = default;
    EDS& operator=(EDS&&) = default;

    // Query methods
    bool empty() const { return is_empty_; }
    size_t length() const { return n_; }           // Number of sets
    size_t size() const { return N_; }             // Total characters
    size_t cardinality() const { return m_; }      // Total number of strings

    // Metadata structure (combines index data and statistics)
    // This is the core of memory-efficient streaming EDS
    struct Metadata {
        // Index data (position/size information)
        // Byte offset of each symbol in the file. Stored as uint64_t, not
        // std::streampos: streampos carries an mbstate_t and is 16 bytes, which
        // doubled the largest per-symbol array for no benefit (EDS files are
        // byte streams, never multibyte-stateful).
        std::vector<uint64_t> base_positions;         // Starting position of each symbol in file
        std::vector<Length> symbol_sizes;             // Number of strings per symbol (n entries)
        std::vector<Length> string_lengths;           // Length of each string (m entries total)
        std::vector<Length> cum_set_sizes;            // Cumulative string IDs (for mapping)
        std::vector<bool> is_degenerate;              // Degenerate flag per symbol

        // Statistics (computed from index data)
        Length min_context_length;        // Minimum non-degenerate symbol length
        Length max_context_length;        // Maximum non-degenerate symbol length
        double avg_context_length;        // Average non-degenerate symbol length
        size_t num_degenerate_symbols;    // Count of degenerate symbols
        size_t num_common_chars;          // Total chars in non-degenerate symbols
        size_t total_change_size;         // Total chars in degenerate symbols
        size_t num_empty_strings;         // Count of empty string alternatives

        // Position checking support — LAZY: both arrays are empty until the first
        // call to a position-lookup method (decode_degenerate_string_number(),
        // find_symbol_at_common_position(), check_position()), which materialises
        // them via ensure_position_index(). They cost 12 bytes per symbol and are
        // pure prefix sums of symbol_sizes / string_lengths / is_degenerate, so
        // the l-EDS merge — which never looks up positions but does hold both an
        // input and an output metadata at once — no longer pays for them.
        // Read them through ensure_position_index(), never directly.
        mutable std::vector<Position> cum_common_positions;   // Cumulative common chars before each symbol (n+1 entries)
        mutable std::vector<int> cum_degenerate_counts;       // Cumulative degenerate strings before each symbol (n+1 entries)
    };

    const Metadata& get_metadata() const { return metadata_; }  // Get full metadata

    // Factory: construct a METADATA_ONLY EDS directly from pre-built metadata + file path.
    // The file at file_path must already exist and contain the EDS data described by metadata.
    // Use this to avoid re-parsing a file whose metadata was captured during writing.
    static EDS from_metadata(Metadata&& metadata,
                             size_t n, size_t m, size_t N,
                             const std::filesystem::path& file_path);

    // Output methods
    void print(std::ostream& os = std::cout) const;
    void save(std::ostream& os, OutputFormat format = OutputFormat::FULL) const;
    void save(const std::filesystem::path& path, OutputFormat format = OutputFormat::FULL) const;

    // Pattern generation for benchmarking
    void generate_patterns(std::ostream& os, size_t count, Length pattern_length) const;

    // Extract substring from EDS
    String extract(Position pos, Length len, const std::vector<int>& changes) const;

    // Position checking: verify if pattern occurs at position with given degenerate string choices
    bool check_position(Position common_pos,
                       const std::vector<int>& degenerate_strings,
                       const String& pattern) const;

    // Streaming access (works in both modes)
    StringSet read_symbol(Position pos) const;  // Read symbol from file or memory
    // In-memory (FULL) mode only: return a const reference to the stored symbol,
    // avoiding the by-value copy of read_symbol().  Throws if the EDS is
    // file-backed (METADATA_ONLY), where the symbol must be freshly read from
    // disk — use read_symbol() there.
    const StringSet& read_symbol_ref(Position pos) const;
    // Bulk-copy the raw on-disk bytes of symbols [start, start+count) to `out`.
    // METADATA_ONLY only. Precondition: those symbols are stored in full-bracket
    // format as one contiguous byte run (no inter-symbol padding), so the copy
    // is byte-for-byte and needs no parse/reserialise. Used by the l-EDS merge
    // pass-through to raw-copy unmodified symbols; the caller verifies the
    // precondition per symbol before batching.
    void copy_symbol_range_to_stream(Position start, size_t count, std::ostream& out) const;
    Length get_symbol_size(Position pos) const { return metadata_.symbol_sizes[pos]; }
    uint64_t get_base_position(Position pos) const { return metadata_.base_positions[pos]; }
    Length get_string_length(size_t string_id) const { return metadata_.string_lengths[string_id]; }

    // Source access (delegated to Sources object)
    bool has_sources() const { return sources_ != nullptr; }
    PathSet read_source(size_t string_id) const;  // Delegates to sources_->read_source()

    // Direct Sources object access (for advanced users)
    std::shared_ptr<Sources> get_sources_object() const { return sources_; }
    void set_sources_object(std::shared_ptr<Sources> sources);

private:
    // Core state
    bool is_empty_;
    size_t n_;                          // Number of sets
    size_t N_;                          // Total size (characters)
    size_t m_;                          // Cardinality (number of strings in all sets)

    // Metadata (always present, contains index + statistics)
    Metadata metadata_;

    // String data (populated by stream/string constructors; empty in file-streaming mode)
    std::vector<StringSet> sets_;       // The actual EDS data

    // File streaming (only if mode_ == METADATA_ONLY)
    std::filesystem::path file_path_;
    mutable std::ifstream stream_;      // Mutable to allow reading in const methods
    mutable std::streamoff file_size_ = -1;  // Lazily cached file size (for last-symbol byte spans)

    // Optional source support (delegated to Sources class)
    std::shared_ptr<Sources> sources_;  // nullptr if no sources loaded

    // Helper methods
    // with_strings=true: also populate sets_ for in-memory access (stream/string ctors)
    void parse(std::istream& is, bool with_strings = false);
    std::string normalize_eds_format(const std::string& input) const;

    // Streaming helpers
    StringSet read_symbol_from_stream(Position pos) const;
    std::streamoff stream_file_size() const;  // File size via the open stream (cached)

    // Return a const reference to symbol `pos` without copying when possible.
    // FULL mode: references sets_[pos] directly (no copy, `scratch` untouched).
    // METADATA_ONLY mode: reads the symbol into `scratch` and references that.
    // The reference is valid until `scratch` is reused or the next call.
    const StringSet& symbol_view(Position pos, StringSet& scratch) const;

    // Build metadata_.cum_common_positions / cum_degenerate_counts if they have
    // not been materialised yet (see the note on those fields). O(n), one pass
    // over the already-resident per-symbol arrays; a no-op once built.
    void ensure_position_index() const;

    // Position checking helpers
    std::pair<size_t, size_t> decode_degenerate_string_number(int abs_string_num) const;
    size_t find_symbol_at_common_position(Position common_pos, Position& offset_out) const;
    String reconstruct_from_memory(size_t start_symbol,
                                   Position offset_in_symbol,
                                   const std::vector<int>& degenerate_strings,
                                   Length pattern_length) const;
    String reconstruct_from_file(size_t start_symbol,
                                 Position offset_in_symbol,
                                 const std::vector<int>& degenerate_strings,
                                 Length pattern_length) const;
    PathSet calculate_path_intersection(size_t start_symbol,
                                              Position offset_in_symbol,
                                              const std::vector<int>& degenerate_strings,
                                              Length pattern_length) const;
};

} // namespace edsparser

#endif // EDSPARSER_EDS_HPP
