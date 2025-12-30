#ifndef SOURCES_HPP
#define SOURCES_HPP

#include <vector>
#include <set>
#include <string>
#include <fstream>
#include <filesystem>
#include <memory>
#include <list>
#include <unordered_map>
#include <cstdint>

/**
 * Sources class - Manages provenance/source tracking for EDS strings
 *
 * Supports multiple formats:
 * - SEDS: Text format {path_ids}{path_ids}...
 * - EDZ: Binary format with varint encoding
 * - EDZ_COMPRESSED: Binary format with zstd compression
 *
 * Storage modes:
 * - FULL: All sources loaded into RAM
 * - METADATA_ONLY: Sources streamed from disk with LRU cache
 */
class Sources {
public:
    // Enums
    enum class Format {
        SEDS,             // Text format (default, backward compatible)
        EDZ,              // Binary format with varint encoding
        EDZ_COMPRESSED    // Binary format with zstd block compression
    };

    enum class StoringMode {
        FULL,            // All sources in RAM
        METADATA_ONLY    // Streaming with LRU cache
    };

    // Construction
    Sources(size_t cardinality, Format format = Format::SEDS,
            StoringMode mode = StoringMode::METADATA_ONLY);

    // Destructor
    ~Sources();

    // Copy constructor and assignment (deleted - use shared_ptr for sharing)
    Sources(const Sources&) = delete;
    Sources& operator=(const Sources&) = delete;

    // Move constructor and assignment
    Sources(Sources&&) noexcept;
    Sources& operator=(Sources&&) noexcept;

    // I/O
    static std::shared_ptr<Sources> load(const std::filesystem::path& path,
                                          Format format, StoringMode mode);
    static std::shared_ptr<Sources> load(const std::filesystem::path& path,
                                          StoringMode mode = StoringMode::METADATA_ONLY);  // Auto-detect format
    static std::shared_ptr<Sources> from_stream(std::istream& is, size_t cardinality,
                                                 Format format = Format::SEDS);  // Create from stream (FULL mode only)
    void save(const std::filesystem::path& path) const;

    // Access
    std::set<int> read_source(size_t string_id) const;
    void set_source(size_t string_id, const std::set<int>& paths);

    // Static helpers for source merging
    /**
     * Compute source set intersection when merging two string alternatives.
     *
     * Handles the special universal marker {0}:
     * - {0} ∩ {0} = {0}
     * - {0} ∩ {x,y,...} = {x,y,...}
     * - {x,y,...} ∩ {a,b,...} = regular set intersection
     *
     * @param sources1 Source set for first string alternative
     * @param sources2 Source set for second string alternative
     * @return Intersection of sources (empty if no valid paths)
     */
    static std::set<int> intersect_sources(
        const std::set<int>& sources1,
        const std::set<int>& sources2
    );

    /**
     * Merge sources from two adjacent symbols (analogous to EDS::merge_adjacent).
     *
     * Computes all valid source combinations for merging symbol1 and symbol2,
     * filtering by non-empty source intersection (LINEAR merge semantics).
     *
     * @param symbol1_start Global string index for first symbol
     * @param symbol1_size Number of strings in first symbol
     * @param symbol2_start Global string index for second symbol
     * @param symbol2_size Number of strings in second symbol
     * @return Vector of merged source sets (only non-empty intersections)
     * @throws std::runtime_error if all intersections are empty
     */
    std::vector<std::set<int>> merge_adjacent_sources(
        size_t symbol1_start, size_t symbol1_size,
        size_t symbol2_start, size_t symbol2_size
    ) const;

    // Query
    size_t cardinality() const { return cardinality_; }
    Format get_format() const { return format_; }
    StoringMode get_mode() const { return mode_; }

    // Statistics (only available in FULL mode)
    size_t num_paths() const;
    size_t max_paths_per_string() const;
    double avg_paths_per_string() const;
    bool has_statistics() const { return has_statistics_; }

    // Cache management (for METADATA_ONLY mode)
    void set_cache_capacity(size_t capacity);
    void clear_cache();

    // Format detection
    static Format detect_format(const std::filesystem::path& path);

private:
    // Core data
    size_t cardinality_;                    // Number of strings (m)
    Format format_;
    StoringMode mode_;

    // FULL mode: all sources in RAM
    std::vector<std::set<int>> sources_;

    // METADATA_ONLY mode: streaming + index
    std::filesystem::path file_path_;
    mutable std::ifstream stream_;

    // Index data structures (format-specific)
    std::vector<std::streampos> base_positions_;           // For .seds: file position per source
    std::vector<std::pair<uint64_t, uint32_t>> binary_index_;  // For .edz: (offset, size) per source

    // LRU cache for METADATA_ONLY mode
    struct CacheEntry {
        size_t string_id;
        std::set<int> paths;
    };
    mutable std::list<CacheEntry> cache_;
    mutable std::unordered_map<size_t, std::list<CacheEntry>::iterator> cache_map_;
    size_t cache_capacity_;                 // Default: 10000

    // Statistics (only calculated in FULL mode)
    bool has_statistics_;
    size_t num_paths_;
    size_t max_paths_per_string_;
    double avg_paths_per_string_;

    // Format-specific parsing
    void parse_seds(std::istream& is);
    void parse_seds_metadata_only(std::istream& is);
    void parse_edz(std::istream& is);
    void parse_edz_metadata_only(std::istream& is);
    void parse_edz_compressed(std::istream& is);
    void parse_edz_compressed_metadata_only(std::istream& is);

    // Format-specific streaming
    std::set<int> read_from_seds(size_t string_id) const;
    std::set<int> read_from_edz(size_t string_id) const;
    std::set<int> read_from_edz_compressed(size_t string_id) const;

    // Format-specific saving
    void save_seds(const std::filesystem::path& path) const;
    void save_edz(const std::filesystem::path& path) const;
    void save_edz_compressed(const std::filesystem::path& path) const;

    // Statistics calculation
    void calculate_statistics();

    // Helper: Add to cache
    void add_to_cache(size_t string_id, const std::set<int>& paths) const;
};

#endif // SOURCES_HPP
