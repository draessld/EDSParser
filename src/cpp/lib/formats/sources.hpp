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
#include <mutex>

/**
 * Sources class - Manages provenance/source tracking for EDS strings
 *
 * Uses streaming architecture with LRU cache for memory-efficient access.
 * Sources are streamed from disk on-demand rather than loaded into RAM.
 *
 * Supports multiple formats:
 * - SEDS: Text format {path_ids}{path_ids}... (currently implemented)
 * - EDZ: Binary format with varint encoding (future)
 * - EDZ_COMPRESSED: Binary format with zstd compression (future)
 */
class Sources {
public:
    // Format enum
    enum class Format {
        SEDS,             // Text format (default, backward compatible)
        EDZ,              // Binary format with varint encoding (not yet implemented)
        EDZ_COMPRESSED    // Binary format with zstd block compression (not yet implemented)
    };

    // Construction
    explicit Sources(size_t cardinality, Format format = Format::SEDS);

    // Destructor
    ~Sources();

    // Copy constructor and assignment (deleted - use shared_ptr for sharing)
    Sources(const Sources&) = delete;
    Sources& operator=(const Sources&) = delete;

    // Move constructor and assignment
    Sources(Sources&&) noexcept;
    Sources& operator=(Sources&&) noexcept;

    // I/O - File-based loading only (streaming requires file paths)
    static std::shared_ptr<Sources> load(const std::filesystem::path& path, Format format);
    static std::shared_ptr<Sources> load(const std::filesystem::path& path);  // Auto-detect format
    void save(const std::filesystem::path& path) const;

    // Access (always uses streaming with cache)
    std::set<int> read_source(size_t string_id) const;

    // Fast reference access — avoids the copy on cache hit.
    // The reference is stable for the duration of the call unless the caller
    // triggers further cache mutations (safe for tight loops that read, then act).
    const std::set<int>& read_source_ref(size_t string_id) const;

    // Bulk byte-copy: writes the raw SEDS bytes for strings [start_idx, start_idx+count)
    // directly to `out` without parsing or reformatting.  One seek + sequential reads;
    // avoids per-string read_source() calls and std::set allocations.
    // Only valid for SEDS format sources that were loaded from a file.
    void copy_range_to_stream(size_t start_idx, size_t count, std::ostream& out) const;

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

    // Cache management
    void set_cache_capacity(size_t capacity);
    void clear_cache();

    // Format detection
    static Format detect_format(const std::filesystem::path& path);

private:
    // Core data
    size_t cardinality_;                    // Number of strings (m)
    Format format_;

    // Streaming support
    std::filesystem::path file_path_;
    mutable std::ifstream stream_;

    // Index data structures (format-specific)
    std::vector<std::streampos> base_positions_;           // For .seds: file position per source
    std::vector<std::pair<uint64_t, uint32_t>> binary_index_;  // For .edz: (offset, size) per source

    // LRU cache
    struct CacheEntry {
        size_t string_id;
        std::set<int> paths;
    };
    mutable std::list<CacheEntry> cache_;
    mutable std::unordered_map<size_t, std::list<CacheEntry>::iterator> cache_map_;
    size_t cache_capacity_;                 // Default: 10000

    // Mutex protecting stream_ and cache_ for thread-safe concurrent reads
    mutable std::mutex io_mutex_;

    // Format-specific parsing (builds index only)
    void parse_seds(std::istream& is);
    void parse_edz(std::istream& is);
    void parse_edz_compressed(std::istream& is);

    // Format-specific streaming
    std::set<int> read_from_seds(size_t string_id) const;
    std::set<int> read_from_edz(size_t string_id) const;
    std::set<int> read_from_edz_compressed(size_t string_id) const;

    // Format-specific saving
    void save_seds(const std::filesystem::path& path) const;
    void save_edz(const std::filesystem::path& path) const;
    void save_edz_compressed(const std::filesystem::path& path) const;

    // Helper: Add to cache (takes by value to enable move)
    void add_to_cache(size_t string_id, std::set<int> paths) const;
};

#endif // SOURCES_HPP
