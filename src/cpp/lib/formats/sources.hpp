#ifndef SOURCES_HPP
#define SOURCES_HPP

#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <memory>
#include <list>
#include <unordered_map>
#include <cstdint>
#include <mutex>

// Sorted, deduplicated list of integer path IDs.
// Invariant: elements are in ascending order, no duplicates.
using PathSet = std::vector<int>;

/**
 * Sources class - Manages provenance/source tracking for EDS strings
 *
 * Uses streaming architecture with LRU cache for memory-efficient access.
 * Sources are streamed from disk on-demand rather than loaded into RAM.
 *
 * Supported formats:
 * - SEDS: Text format with range+complement encoding: {0}{1-3,7}{0,100-500}
 *     0 = universal set (all paths); {0,e1,e2} = complement (all except e1,e2)
 * - EDZ: Binary bitset format — each entry is ceil(num_paths/8) bytes;
 *     bit k (0-indexed, LSB-first) = path ID (k+1) in set; all-ones = universal.
 *     No index section; O(1) random access by byte-offset arithmetic.
 *     24-byte header: magic(4) + flags(2,=0x0002) + reserved(2) +
 *                     cardinality(8) + num_paths(8)
 * - EDZ_COMPRESSED: planned (zstd compression of EDZ data section)
 */
class Sources {
public:
    // Format enum
    enum class Format {
        SEDS,             // Text format with range+complement encoding (dense)
        SEDS_SPARSE,      // Text format, sparse: {0} omitted; bitvec+trailer appended
        EDZ,              // Binary bitset format (dense)
        EDZ_SPARSE,       // Binary bitset, sparse: non-degen entries only; bitvec at end
        EDZ_COMPRESSED    // Binary format with zstd block compression (planned)
    };

    // ── Streaming EDZ write API (dense) ──────────────────────────────────────
    // Call in order:
    //   1. write_edz_header(os, num_paths)  — writes 24-byte placeholder header
    //   2. write_edz_entry(os, paths, num_paths)  — one call per EDS string
    //   3. write_edz_finalize(os, cardinality)  — seeks back, fills cardinality
    static void write_edz_header  (std::ostream& os, size_t num_paths);
    static void write_edz_entry   (std::ostream& os, const PathSet& paths, size_t num_paths);
    static void write_edz_finalize(std::ostream& os, size_t cardinality);

    // ── Streaming EDZ_SPARSE write API ────────────────────────────────────────
    // Skip write_edz_entry calls for universal ({0}) entries; track them in bitvec.
    // Header is 32 bytes; finalize patches cardinality + m_degenerate + appends bitvec.
    //   1. write_edz_sparse_header(os, num_paths)           — 32-byte placeholder
    //   2. write_edz_entry(os, paths, num_paths)            — only for non-universal
    //   3. write_edz_sparse_finalize(os, cardinality,
    //                                m_degenerate, bitvec)  — patches + appends bitvec
    static void write_edz_sparse_header  (std::ostream& os, size_t num_paths);
    static void write_edz_sparse_finalize(std::ostream& os, size_t cardinality,
                                          size_t num_paths,
                                          size_t m_degenerate,
                                          const std::vector<uint8_t>& presence_bitvec);

    // ── Streaming SEDS_SPARSE write API ───────────────────────────────────────
    // Write text entries for non-universal strings only, then call finalize to
    // append the bitvec + trailer. The trailer stores num_paths so complement
    // encoding ({0,e1,...}) can be expanded exactly on load without inference.
    // Trailer: bitvec | "SED2"(4) | cardinality(8) | m_degenerate(8) | num_paths(8).
    static void write_seds_sparse_finalize(std::ostream& os, size_t cardinality,
                                           size_t m_degenerate,
                                           const std::vector<uint8_t>& presence_bitvec,
                                           size_t num_paths);

    // ── Streaming SEDS (dense) trailer API ────────────────────────────────────
    // After writing all cardinality text entries, append a trailer recording the
    // path universe size so complement entries expand exactly on load.
    // Trailer: "SEDN"(4) | cardinality(8) | num_paths(8).
    static void write_seds_dense_finalize(std::ostream& os, size_t cardinality,
                                          size_t num_paths);

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

    // Save in an explicitly chosen target format, independent of how the Sources
    // object was loaded. Reads each entry via read_source() (format-agnostic) and
    // re-encodes into the requested format. Used by edsparser-source-transform to
    // convert between SEDS / SEDS_SPARSE / EDZ / EDZ_SPARSE. EDZ_COMPRESSED throws
    // (not yet implemented).
    void save_as(const std::filesystem::path& path, Format format) const;

    // Access (always uses streaming with cache)
    PathSet read_source(size_t string_id) const;

    // Fast reference access — avoids the copy on cache hit.
    // The reference is stable for the duration of the call unless the caller
    // triggers further cache mutations (safe for tight loops that read, then act).
    const PathSet& read_source_ref(size_t string_id) const;

    // Bulk byte-copy: writes the raw SEDS bytes for strings [start_idx, start_idx+count)
    // directly to `out` without parsing or reformatting.  One seek + sequential reads;
    // avoids per-string read_source() calls and PathSet allocations.
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
    static PathSet intersect_sources(
        const PathSet& sources1,
        const PathSet& sources2
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
    std::vector<PathSet> merge_adjacent_sources(
        size_t symbol1_start, size_t symbol1_size,
        size_t symbol2_start, size_t symbol2_size
    ) const;

    // Query
    size_t cardinality() const { return cardinality_; }
    Format get_format() const { return format_; }
    size_t num_paths() const { return num_paths_; }
    bool   is_sparse()  const { return is_sparse_; }
    size_t m_degenerate() const { return m_degenerate_; }

    // Set num_paths — required before save_edz() when building Sources in-memory
    void set_num_paths(size_t n) { num_paths_ = n; }

    // Cache management
    void set_cache_capacity(size_t capacity);
    void clear_cache();

    // Format detection
    static Format detect_format(const std::filesystem::path& path);

private:
    // Core data
    size_t   cardinality_;                  // Number of strings (m)
    Format   format_;
    size_t   num_paths_ = 0;               // Total path count (required for EDZ bitset)
    bool     is_bitset_ = false;           // true → EDZ bitset format (flags & 0x0002)
    bool     is_sparse_ = false;           // true → sparse format (universal entries omitted)
    size_t   m_degenerate_ = 0;            // Number of non-universal entries stored

    // Sparse support: bitvec[i/8] bit (i%8) = 1 iff string i is non-universal
    std::vector<uint8_t>   presence_bitvec_;
    // Prefix popcount: prefix_[i] = #set bits in bitvec[0..i-1]; size = bitvec_.size()+1
    std::vector<uint32_t>  bitvec_prefix_;

    // Streaming support
    std::filesystem::path file_path_;
    mutable std::ifstream stream_;

    // Index data structures (format-specific)
    std::vector<std::streampos> base_positions_;           // For .seds: file position per source
    std::vector<std::pair<uint64_t, uint32_t>> binary_index_;  // For varint .edz: (offset, size)

    // ── EDZ bitset helpers ────────────────────────────────────────────────────
    static size_t edz_bpe(size_t num_paths) { return (num_paths + 7) / 8; }

    // Encode a PathSet into a zeroed bitset buffer of `bpe` bytes.
    // PathSet{0} (universal) → all-ones.  PathSet{0,e1,e2} (complement) → all-ones
    // except bits for e1,e2.  Explicit PathSet{p1,p2} → set bits p1-1,p2-1.
    static void pathset_to_bitset(uint8_t* buf, size_t bpe, size_t num_paths,
                                  const PathSet& ps);

    // Decode a bitset buffer of `bpe` bytes into a PathSet.
    // All-ones → PathSet{0} (universal).  Otherwise builds explicit list.
    static PathSet bitset_to_pathset(const uint8_t* buf, size_t bpe, size_t num_paths);

    // LRU cache
    struct CacheEntry {
        size_t string_id;
        PathSet paths;
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
    PathSet read_from_seds(size_t string_id) const;
    PathSet read_from_edz(size_t string_id) const;
    PathSet read_from_edz_compressed(size_t string_id) const;

    // Format-specific saving
    void save_seds(const std::filesystem::path& path) const;
    void save_seds_sparse(const std::filesystem::path& path) const;
    void save_edz(const std::filesystem::path& path) const;
    void save_edz_sparse(const std::filesystem::path& path) const;
    void save_edz_compressed(const std::filesystem::path& path) const;

    // Number of paths to use when encoding as EDZ. Returns num_paths_ if known,
    // otherwise derives it by scanning all entries for the maximum path ID.
    size_t effective_num_paths() const;

    // Helper: Add to cache (takes by value to enable move)
    void add_to_cache(size_t string_id, PathSet paths) const;
};

#endif // SOURCES_HPP
