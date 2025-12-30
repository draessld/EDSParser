#include "sources.hpp"
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>

// Constants for .seds format
static const char SET_OPEN = '{';
static const char SET_CLOSE = '}';
static const char SET_SEPARATOR = ',';

// ================================================================================
// CONSTRUCTION AND DESTRUCTION
// ================================================================================

Sources::Sources(size_t cardinality, Format format, StoringMode mode)
    : cardinality_(cardinality)
    , format_(format)
    , mode_(mode)
    , cache_capacity_(10000)  // Default cache size
    , has_statistics_(false)
    , num_paths_(0)
    , max_paths_per_string_(0)
    , avg_paths_per_string_(0.0)
{
    if (cardinality == 0) {
        throw std::invalid_argument("Sources: Cardinality must be > 0");
    }

    // Pre-allocate in FULL mode
    if (mode_ == StoringMode::FULL) {
        sources_.reserve(cardinality_);
    }
}

Sources::~Sources() {
    // Close file streams if open
    if (stream_.is_open()) {
        stream_.close();
    }
}

Sources::Sources(Sources&& other) noexcept
    : cardinality_(other.cardinality_)
    , format_(other.format_)
    , mode_(other.mode_)
    , sources_(std::move(other.sources_))
    , file_path_(std::move(other.file_path_))
    , stream_(std::move(other.stream_))
    , base_positions_(std::move(other.base_positions_))
    , binary_index_(std::move(other.binary_index_))
    , cache_(std::move(other.cache_))
    , cache_map_(std::move(other.cache_map_))
    , cache_capacity_(other.cache_capacity_)
    , has_statistics_(other.has_statistics_)
    , num_paths_(other.num_paths_)
    , max_paths_per_string_(other.max_paths_per_string_)
    , avg_paths_per_string_(other.avg_paths_per_string_)
{
}

Sources& Sources::operator=(Sources&& other) noexcept {
    if (this != &other) {
        cardinality_ = other.cardinality_;
        format_ = other.format_;
        mode_ = other.mode_;
        sources_ = std::move(other.sources_);
        file_path_ = std::move(other.file_path_);
        stream_ = std::move(other.stream_);
        base_positions_ = std::move(other.base_positions_);
        binary_index_ = std::move(other.binary_index_);
        cache_ = std::move(other.cache_);
        cache_map_ = std::move(other.cache_map_);
        cache_capacity_ = other.cache_capacity_;
        has_statistics_ = other.has_statistics_;
        num_paths_ = other.num_paths_;
        max_paths_per_string_ = other.max_paths_per_string_;
        avg_paths_per_string_ = other.avg_paths_per_string_;
    }
    return *this;
}

// ================================================================================
// I/O - LOADING
// ================================================================================

std::shared_ptr<Sources> Sources::load(const std::filesystem::path& path,
                                         Format format, StoringMode mode) {
    // Validate file exists
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Sources file does not exist: " + path.string());
    }

    // Open file to determine cardinality (quick scan)
    std::ifstream temp_stream(path);
    if (!temp_stream.is_open()) {
        throw std::runtime_error("Failed to open sources file: " + path.string());
    }

    // Count sets to determine cardinality
    size_t cardinality = 0;
    if (format == Format::SEDS) {
        // Count '{' occurrences
        char ch;
        while (temp_stream.get(ch)) {
            if (ch == SET_OPEN) {
                cardinality++;
            }
        }
    } else {
        // For binary formats, read cardinality from header
        throw std::runtime_error("Binary formats (.edz) not yet implemented");
    }

    temp_stream.close();

    if (cardinality == 0) {
        throw std::runtime_error("Sources file contains no source sets");
    }

    // Create Sources object
    auto sources = std::make_shared<Sources>(cardinality, format, mode);
    sources->file_path_ = path;

    // Open file for parsing
    std::ifstream stream(path);
    if (!stream.is_open()) {
        throw std::runtime_error("Failed to open sources file: " + path.string());
    }

    // Parse based on format
    switch (format) {
        case Format::SEDS:
            if (mode == StoringMode::FULL) {
                sources->parse_seds(stream);
            } else {
                sources->parse_seds_metadata_only(stream);
                // Keep stream open for METADATA_ONLY mode
                sources->stream_ = std::move(stream);
            }
            break;

        case Format::EDZ:
            throw std::runtime_error("EDZ format not yet implemented (Phase 2)");

        case Format::EDZ_COMPRESSED:
            throw std::runtime_error("EDZ_COMPRESSED format not yet implemented (Phase 3)");
    }

    return sources;
}

std::shared_ptr<Sources> Sources::load(const std::filesystem::path& path,
                                         StoringMode mode) {
    // Auto-detect format from file extension
    Format format = detect_format(path);
    return load(path, format, mode);
}

std::shared_ptr<Sources> Sources::from_stream(std::istream& is, size_t cardinality,
                                                Format format) {
    // Create Sources object in FULL mode (stream-based loading always uses FULL)
    auto sources = std::make_shared<Sources>(cardinality, format, StoringMode::FULL);

    // Parse based on format
    if (format == Format::SEDS) {
        sources->parse_seds(is);
    } else if (format == Format::EDZ) {
        sources->parse_edz(is);
    } else if (format == Format::EDZ_COMPRESSED) {
        sources->parse_edz_compressed(is);
    }

    // Verify cardinality matches
    if (sources->sources_.size() != cardinality) {
        throw std::runtime_error("Source cardinality mismatch: expected " +
                                 std::to_string(cardinality) + ", got " +
                                 std::to_string(sources->sources_.size()));
    }

    // Calculate statistics (only in FULL mode)
    sources->calculate_statistics();

    return sources;
}

// ================================================================================
// FORMAT DETECTION
// ================================================================================

Sources::Format Sources::detect_format(const std::filesystem::path& path) {
    std::string ext = path.extension().string();

    if (ext == ".seds") {
        return Format::SEDS;
    } else if (ext == ".edz") {
        // Check magic bytes to determine if compressed
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) {
            throw std::runtime_error("Failed to open file for format detection: " + path.string());
        }

        char magic[4];
        stream.read(magic, 4);
        if (stream.gcount() < 4) {
            throw std::runtime_error("File too small to determine format: " + path.string());
        }

        if (magic[0] == 'E' && magic[1] == 'D' && magic[2] == 'Z' && magic[3] == '\0') {
            // Read flags to check compression
            uint16_t flags;
            stream.read(reinterpret_cast<char*>(&flags), sizeof(flags));
            if (stream.gcount() < static_cast<std::streamsize>(sizeof(flags))) {
                throw std::runtime_error("Failed to read format flags");
            }

            if (flags & 0x0001) {
                return Format::EDZ_COMPRESSED;
            } else {
                return Format::EDZ;
            }
        } else {
            throw std::runtime_error("Invalid .edz file (bad magic bytes): " + path.string());
        }
    } else {
        // Default to SEDS for unknown extensions
        return Format::SEDS;
    }
}

// ================================================================================
// PARSING - SEDS FORMAT
// ================================================================================

void Sources::parse_seds(std::istream& is) {
    // Read entire input into string
    std::stringstream buffer;
    buffer << is.rdbuf();
    std::string input = buffer.str();

    // Remove whitespace
    input.erase(std::remove_if(input.begin(), input.end(), ::isspace), input.end());

    if (input.empty()) {
        throw std::runtime_error("sEDS input is empty");
    }

    // Parse flattened sEDS format: {path_ids}{path_ids}...
    size_t pos = 0;
    sources_.clear();
    size_t string_count = 0;

    while (pos < input.length()) {
        // Expect '{'
        if (input[pos] != SET_OPEN) {
            throw std::runtime_error("sEDS: Expected '{' at position " + std::to_string(pos));
        }
        pos++; // Skip '{'

        // Parse path IDs for this string
        std::set<int> path_set;
        std::string current_number;

        while (pos < input.length() && input[pos] != SET_CLOSE) {
            if (input[pos] == SET_SEPARATOR) {
                // End of current path ID
                if (!current_number.empty()) {
                    int path_id = std::stoi(current_number);
                    if (path_id < 0) {
                        throw std::runtime_error("sEDS: Invalid path ID (must be >= 0): " + current_number);
                    }
                    path_set.insert(path_id);
                    current_number.clear();
                }
                pos++;
            } else if (std::isdigit(input[pos])) {
                // Digit, add to current number
                current_number += input[pos];
                pos++;
            } else {
                throw std::runtime_error("sEDS: Invalid character '" + std::string(1, input[pos]) +
                                       "' at position " + std::to_string(pos));
            }
        }

        // Add last path ID if present
        if (!current_number.empty()) {
            int path_id = std::stoi(current_number);
            if (path_id < 0) {
                throw std::runtime_error("sEDS: Invalid path ID (must be >= 0): " + current_number);
            }
            path_set.insert(path_id);
        }

        // Expect '}'
        if (pos >= input.length() || input[pos] != SET_CLOSE) {
            throw std::runtime_error("sEDS: Expected '}' at position " + std::to_string(pos));
        }
        pos++; // Skip '}'

        // Validate path set is not empty
        if (path_set.empty()) {
            throw std::runtime_error("sEDS: Empty path set at string " + std::to_string(string_count));
        }

        // Store source set
        sources_.push_back(path_set);
        string_count++;
    }

    // Validate source count matches cardinality
    if (sources_.size() != cardinality_) {
        throw std::runtime_error("sEDS: Source count (" + std::to_string(sources_.size()) +
                               ") does not match cardinality (" + std::to_string(cardinality_) + ")");
    }

    // Calculate statistics
    calculate_statistics();
}

void Sources::parse_seds_metadata_only(std::istream& is) {
    base_positions_.clear();
    base_positions_.reserve(cardinality_);

    std::streampos current_pos = is.tellg();
    size_t string_count = 0;
    char ch;

    // Single-pass scan to build index
    while (is.get(ch)) {
        if (ch == SET_OPEN) {
            // Record starting position of this source set
            base_positions_.push_back(current_pos);

            // Skip until matching '}'
            int depth = 1;
            while (depth > 0 && is.get(ch)) {
                if (ch == SET_OPEN) depth++;
                else if (ch == SET_CLOSE) depth--;
            }

            current_pos = is.tellg();
            string_count++;
        } else if (!std::isspace(static_cast<unsigned char>(ch))) {
            throw std::runtime_error("Unexpected character in sEDS file: " +
                                   std::string(1, ch));
        } else {
            current_pos = is.tellg();
        }
    }

    // Validate count matches cardinality
    if (string_count != cardinality_) {
        throw std::runtime_error("sEDS: Source count (" +
            std::to_string(string_count) + ") does not match cardinality (" +
            std::to_string(cardinality_) + ")");
    }

    // Note: Statistics cannot be calculated without loading actual data
}

// ================================================================================
// PARSING - BINARY FORMATS (Placeholders for Phase 2 & 3)
// ================================================================================

void Sources::parse_edz(std::istream& is) {
    throw std::runtime_error("EDZ format parsing not yet implemented (Phase 2)");
}

void Sources::parse_edz_metadata_only(std::istream& is) {
    throw std::runtime_error("EDZ metadata-only parsing not yet implemented (Phase 2)");
}

void Sources::parse_edz_compressed(std::istream& is) {
    throw std::runtime_error("EDZ_COMPRESSED format parsing not yet implemented (Phase 3)");
}

void Sources::parse_edz_compressed_metadata_only(std::istream& is) {
    throw std::runtime_error("EDZ_COMPRESSED metadata-only parsing not yet implemented (Phase 3)");
}

// ================================================================================
// STREAMING ACCESS
// ================================================================================

std::set<int> Sources::read_from_seds(size_t string_id) const {
    if (!stream_.is_open()) {
        throw std::runtime_error("Sources file stream not available");
    }

    // Seek to position
    stream_.clear();  // Clear error flags
    stream_.seekg(base_positions_[string_id]);

    if (!stream_) {
        throw std::runtime_error("Failed to seek to source position " +
                                std::to_string(string_id));
    }

    // Parse one source set: {path_id1,path_id2,...}
    std::set<int> result;
    char ch;
    std::string current_number;

    // Expect '{'
    if (!stream_.get(ch) || ch != SET_OPEN) {
        throw std::runtime_error("Expected '{' for source set " +
                                std::to_string(string_id));
    }

    // Parse path IDs
    while (stream_.get(ch) && ch != SET_CLOSE) {
        if (ch == SET_SEPARATOR) {
            if (!current_number.empty()) {
                result.insert(std::stoi(current_number));
                current_number.clear();
            }
        } else if (std::isdigit(static_cast<unsigned char>(ch))) {
            current_number += ch;
        } else if (!std::isspace(static_cast<unsigned char>(ch))) {
            throw std::runtime_error("Invalid character in source set: " +
                                   std::string(1, ch));
        }
    }

    // Add last number
    if (!current_number.empty()) {
        result.insert(std::stoi(current_number));
    }

    return result;
}

std::set<int> Sources::read_from_edz(size_t string_id) const {
    throw std::runtime_error("EDZ streaming not yet implemented (Phase 2)");
}

std::set<int> Sources::read_from_edz_compressed(size_t string_id) const {
    throw std::runtime_error("EDZ_COMPRESSED streaming not yet implemented (Phase 3)");
}

// ================================================================================
// PUBLIC ACCESS
// ================================================================================

std::set<int> Sources::read_source(size_t string_id) const {
    // Validation
    if (string_id >= cardinality_) {
        throw std::out_of_range("String ID " + std::to_string(string_id) +
                               " out of range (cardinality=" + std::to_string(cardinality_) + ")");
    }

    // FULL mode: return directly from vector
    if (mode_ == StoringMode::FULL) {
        return sources_[string_id];
    }

    // METADATA_ONLY mode: check cache first
    auto cache_it = cache_map_.find(string_id);
    if (cache_it != cache_map_.end()) {
        // Cache hit: move to front of LRU list
        cache_.splice(cache_.begin(), cache_, cache_it->second);
        return cache_it->second->paths;
    }

    // Cache miss: read from file
    std::set<int> paths;
    switch (format_) {
        case Format::SEDS:
            paths = read_from_seds(string_id);
            break;
        case Format::EDZ:
            paths = read_from_edz(string_id);
            break;
        case Format::EDZ_COMPRESSED:
            paths = read_from_edz_compressed(string_id);
            break;
    }

    // Add to cache
    add_to_cache(string_id, paths);

    return paths;
}

void Sources::set_source(size_t string_id, const std::set<int>& paths) {
    if (string_id >= cardinality_) {
        throw std::out_of_range("String ID " + std::to_string(string_id) +
                               " out of range (cardinality=" + std::to_string(cardinality_) + ")");
    }

    if (mode_ != StoringMode::FULL) {
        throw std::runtime_error("set_source() only available in FULL mode");
    }

    if (paths.empty()) {
        throw std::invalid_argument("Cannot set empty path set");
    }

    // Ensure vector is sized correctly
    if (sources_.size() < cardinality_) {
        sources_.resize(cardinality_);
    }

    sources_[string_id] = paths;

    // Invalidate statistics
    has_statistics_ = false;
}

// ================================================================================
// SOURCE MERGING
// ================================================================================

std::set<int> Sources::intersect_sources(
    const std::set<int>& sources1,
    const std::set<int>& sources2
) {
    std::set<int> intersection;
    bool sources1_has_universal = sources1.count(0) > 0;
    bool sources2_has_universal = sources2.count(0) > 0;

    if (sources1_has_universal && sources2_has_universal) {
        // {0} ∩ {0} = {0}
        intersection.insert(0);
    } else if (sources1_has_universal) {
        // {0} ∩ {x,y,...} = {x,y,...}
        intersection = sources2;
    } else if (sources2_has_universal) {
        // {x,y,...} ∩ {0} = {x,y,...}
        intersection = sources1;
    } else {
        // Regular set intersection
        std::set_intersection(
            sources1.begin(), sources1.end(),
            sources2.begin(), sources2.end(),
            std::inserter(intersection, intersection.begin())
        );
    }
    return intersection;
}

std::vector<std::set<int>> Sources::merge_adjacent_sources(
    size_t symbol1_start, size_t symbol1_size,
    size_t symbol2_start, size_t symbol2_size
) const {
    std::vector<std::set<int>> merged_sources;

    // Compute all valid combinations (LINEAR merge: filter by intersection)
    for (size_t i = 0; i < symbol1_size; ++i) {
        const std::set<int> sources1 = read_source(symbol1_start + i);

        for (size_t j = 0; j < symbol2_size; ++j) {
            const std::set<int> sources2 = read_source(symbol2_start + j);

            // Use static helper for intersection
            std::set<int> intersection = intersect_sources(sources1, sources2);

            // Only keep non-empty intersections
            if (!intersection.empty()) {
                merged_sources.push_back(intersection);
            }
        }
    }

    // Validation: merged set must not be empty
    if (merged_sources.empty()) {
        throw std::runtime_error(
            "Merging symbols results in empty set (no valid source intersections)"
        );
    }

    return merged_sources;
}

// ================================================================================
// SAVING
// ================================================================================

void Sources::save(const std::filesystem::path& path) const {
    switch (format_) {
        case Format::SEDS:
            save_seds(path);
            break;
        case Format::EDZ:
            save_edz(path);
            break;
        case Format::EDZ_COMPRESSED:
            save_edz_compressed(path);
            break;
    }
}

void Sources::save_seds(const std::filesystem::path& path) const {
    std::ofstream os(path);
    if (!os.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " + path.string());
    }

    if (mode_ == StoringMode::FULL) {
        // Write from sources_ vector
        for (const auto& source_set : sources_) {
            os << SET_OPEN;
            bool first = true;
            for (int path_id : source_set) {
                if (!first) os << SET_SEPARATOR;
                os << path_id;
                first = false;
            }
            os << SET_CLOSE;
        }
    } else {
        // Read and copy from stream
        for (size_t i = 0; i < cardinality_; i++) {
            std::set<int> paths = read_source(i);
            os << SET_OPEN;
            bool first = true;
            for (int path_id : paths) {
                if (!first) os << SET_SEPARATOR;
                os << path_id;
                first = false;
            }
            os << SET_CLOSE;
        }
    }

    os << '\n';  // Trailing newline
    os.close();
}

void Sources::save_edz(const std::filesystem::path& path) const {
    throw std::runtime_error("EDZ format saving not yet implemented (Phase 2)");
}

void Sources::save_edz_compressed(const std::filesystem::path& path) const {
    throw std::runtime_error("EDZ_COMPRESSED format saving not yet implemented (Phase 3)");
}

// ================================================================================
// STATISTICS
// ================================================================================

void Sources::calculate_statistics() {
    if (mode_ != StoringMode::FULL) {
        throw std::runtime_error("Statistics calculation requires FULL mode");
    }

    if (sources_.empty()) {
        has_statistics_ = false;
        return;
    }

    // Track all unique path IDs
    std::set<int> all_paths;
    size_t total_paths = 0;
    max_paths_per_string_ = 0;

    for (const auto& source_set : sources_) {
        // Track max paths in any single string
        if (source_set.size() > max_paths_per_string_) {
            max_paths_per_string_ = source_set.size();
        }

        // Accumulate all unique paths
        for (int path_id : source_set) {
            all_paths.insert(path_id);
        }

        // Count total for average
        total_paths += source_set.size();
    }

    num_paths_ = all_paths.size();
    avg_paths_per_string_ = static_cast<double>(total_paths) / sources_.size();
    has_statistics_ = true;
}

size_t Sources::num_paths() const {
    if (!has_statistics_) {
        throw std::runtime_error("Statistics not available (requires FULL mode with loaded data)");
    }
    return num_paths_;
}

size_t Sources::max_paths_per_string() const {
    if (!has_statistics_) {
        throw std::runtime_error("Statistics not available (requires FULL mode with loaded data)");
    }
    return max_paths_per_string_;
}

double Sources::avg_paths_per_string() const {
    if (!has_statistics_) {
        throw std::runtime_error("Statistics not available (requires FULL mode with loaded data)");
    }
    return avg_paths_per_string_;
}

// ================================================================================
// CACHE MANAGEMENT
// ================================================================================

void Sources::add_to_cache(size_t string_id, const std::set<int>& paths) const {
    if (cache_capacity_ == 0) {
        return;  // Caching disabled
    }

    // Evict if cache full
    if (cache_.size() >= cache_capacity_) {
        size_t evict_id = cache_.back().string_id;
        cache_map_.erase(evict_id);
        cache_.pop_back();
    }

    // Add to front of list (most recently used)
    cache_.push_front({string_id, paths});
    cache_map_[string_id] = cache_.begin();
}

void Sources::set_cache_capacity(size_t capacity) {
    cache_capacity_ = capacity;

    // Trim cache if needed
    while (cache_.size() > cache_capacity_) {
        size_t evict_id = cache_.back().string_id;
        cache_map_.erase(evict_id);
        cache_.pop_back();
    }
}

void Sources::clear_cache() {
    cache_.clear();
    cache_map_.clear();
}
