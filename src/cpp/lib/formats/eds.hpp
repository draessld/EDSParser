#ifndef EDSPARSER_EDS_HPP
#define EDSPARSER_EDS_HPP

#include "../common.hpp"
#include "sources.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <set>
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
        std::vector<std::streampos> base_positions;   // Starting position of each symbol in file
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

        // Position checking support (computed from index data)
        std::vector<Position> cum_common_positions;   // Cumulative common chars before each symbol (n+1 entries)
        std::vector<int> cum_degenerate_counts;       // Cumulative degenerate strings before each symbol (n+1 entries)
    };

    // Statistics (for backward compatibility, returns statistics portion of metadata)
    struct Statistics {
        Length min_context_length;
        Length max_context_length;
        double avg_context_length;
        size_t num_degenerate_symbols;
        size_t num_common_chars;
        size_t total_change_size;
        size_t num_empty_strings;
    };

    const Metadata& get_metadata() const { return metadata_; }  // Get full metadata

    // Factory: construct a METADATA_ONLY EDS directly from pre-built metadata + file path.
    // The file at file_path must already exist and contain the EDS data described by metadata.
    // Use this to avoid re-parsing a file whose metadata was captured during writing.
    static EDS from_metadata(Metadata&& metadata,
                             size_t n, size_t m, size_t N,
                             const std::filesystem::path& file_path);
    Statistics get_statistics() const;                           // Get statistics only
    void print_statistics(std::ostream& os = std::cout) const;

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

    // Access to internal data
    // [[deprecated("Direct access to sets is not supported in streaming mode. Use read_symbol(pos) instead.")]]
    const std::vector<StringSet>& get_sets() const;

    const std::vector<bool>& get_is_degenerate() const { return metadata_.is_degenerate; }

    // Streaming access (works in both modes)
    StringSet read_symbol(Position pos) const;  // Read symbol from file or memory
    Length get_symbol_size(Position pos) const { return metadata_.symbol_sizes[pos]; }
    std::streampos get_base_position(Position pos) const { return metadata_.base_positions[pos]; }
    Length get_string_length(size_t string_id) const { return metadata_.string_lengths[string_id]; }

    // Source access (delegated to Sources object)
    bool has_sources() const { return sources_ != nullptr; }
    std::set<int> read_source(size_t string_id) const;  // Delegates to sources_->read_source()

    // Direct Sources object access (for advanced users)
    std::shared_ptr<Sources> get_sources_object() const { return sources_; }
    void set_sources_object(std::shared_ptr<Sources> sources);

    // Source cache management (deprecated - use sources_->set_cache_capacity() directly)
    void set_source_cache_capacity(size_t capacity);    // Delegates to sources_
    void clear_source_cache() const;                    // Delegates to sources_

private:
    // Core state
    bool is_empty_;
    size_t n_;                          // Number of sets
    size_t N_;                          // Total size (characters)
    size_t m_;                          // Cardinality (number of strings in all sets)

    // Metadata (always present, contains index + statistics)
    Metadata metadata_;

    // String data (only if mode_ == FULL)
    std::vector<StringSet> sets_;       // The actual EDS data

    // File streaming (only if mode_ == METADATA_ONLY)
    std::filesystem::path file_path_;
    mutable std::ifstream stream_;      // Mutable to allow reading in const methods

    // Optional source support (delegated to Sources class)
    std::shared_ptr<Sources> sources_;  // nullptr if no sources loaded

    // Helper methods
    // with_strings=true: also populate sets_ for in-memory access (stream/string ctors)
    void parse(std::istream& is, bool with_strings = false);
    void calculate_statistics();
    std::string normalize_eds_format(const std::string& input) const;

    // Streaming helpers
    StringSet read_symbol_from_stream(Position pos) const;

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
    std::set<int> calculate_path_intersection(size_t start_symbol,
                                              Position offset_in_symbol,
                                              const std::vector<int>& degenerate_strings,
                                              Length pattern_length) const;
};

} // namespace edsparser

#endif // EDSPARSER_EDS_HPP
