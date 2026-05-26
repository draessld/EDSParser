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

Sources::Sources(size_t cardinality, Format format)
    : cardinality_(cardinality)
    , format_(format)
    , cache_capacity_(10000)  // Default cache size
{
    // cardinality==0 is allowed as a sentinel for auto-detect during load()
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
    , file_path_(std::move(other.file_path_))
    , stream_(std::move(other.stream_))
    , base_positions_(std::move(other.base_positions_))
    , binary_index_(std::move(other.binary_index_))
    , cache_(std::move(other.cache_))
    , cache_map_(std::move(other.cache_map_))
    , cache_capacity_(other.cache_capacity_)
{
}

Sources& Sources::operator=(Sources&& other) noexcept {
    if (this != &other) {
        cardinality_ = other.cardinality_;
        format_ = other.format_;
        file_path_ = std::move(other.file_path_);
        stream_ = std::move(other.stream_);
        base_positions_ = std::move(other.base_positions_);
        binary_index_ = std::move(other.binary_index_);
        cache_ = std::move(other.cache_);
        cache_map_ = std::move(other.cache_map_);
        cache_capacity_ = other.cache_capacity_;
    }
    return *this;
}

// ================================================================================
// I/O - LOADING
// ================================================================================

std::shared_ptr<Sources> Sources::load(const std::filesystem::path& path,
                                         Format format) {
    // Validate file exists
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Sources file does not exist: " + path.string());
    }

    if (format != Format::SEDS) {
        throw std::runtime_error("Binary formats (.edz) not yet implemented");
    }

    // Open file once — parse_seds() will build index and set cardinality_ in one pass
    std::ifstream stream(path);
    if (!stream.is_open()) {
        throw std::runtime_error("Failed to open sources file: " + path.string());
    }

    // Create Sources with cardinality=0 sentinel (parse_seds() will fill it)
    auto sources = std::make_shared<Sources>(0, format);
    sources->file_path_ = path;

    // Parse: builds index + sets cardinality_ in one pass
    sources->parse_seds(stream);

    if (sources->cardinality_ == 0) {
        throw std::runtime_error("Sources file contains no source sets");
    }

    // Keep stream open for on-demand reading
    sources->stream_ = std::move(stream);

    return sources;
}

std::shared_ptr<Sources> Sources::load(const std::filesystem::path& path) {
    // Auto-detect format from file extension
    Format format = detect_format(path);
    return load(path, format);
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
    base_positions_.clear();
    if (cardinality_ > 0) {
        base_positions_.reserve(cardinality_);
    }

    size_t string_count = 0;
    char ch;

    // Single-pass scan to build index.
    // Record position using tellg() BEFORE consuming '{' so the stored
    // streampos values are valid for seekg() calls in read_from_seds().
    while (is) {
        std::streampos pos_before = is.tellg();
        if (!is.get(ch)) break;

        if (ch == SET_OPEN) {
            // Record the position of '{' (obtained from tellg before consuming)
            base_positions_.push_back(pos_before);

            // Skip until matching '}'
            int depth = 1;
            while (depth > 0 && is.get(ch)) {
                if (ch == SET_OPEN) depth++;
                else if (ch == SET_CLOSE) depth--;
            }

            string_count++;
        } else if (!std::isspace(static_cast<unsigned char>(ch))) {
            throw std::runtime_error("Unexpected character in sEDS file: " +
                                   std::string(1, ch));
        }
    }

    if (cardinality_ == 0) {
        // Auto-detect mode: set cardinality from parsed count
        cardinality_ = string_count;
    } else if (string_count != cardinality_) {
        throw std::runtime_error("sEDS: Source count (" +
            std::to_string(string_count) + ") does not match cardinality (" +
            std::to_string(cardinality_) + ")");
    }
}

// ================================================================================
// PARSING - BINARY FORMATS (Placeholders)
// ================================================================================

void Sources::parse_edz(std::istream& is) {
    throw std::runtime_error("EDZ format parsing not yet implemented");
}

void Sources::parse_edz_compressed(std::istream& is) {
    throw std::runtime_error("EDZ_COMPRESSED format parsing not yet implemented");
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

    // Expect '{'
    if (!stream_.get(ch) || ch != SET_OPEN) {
        throw std::runtime_error("Expected '{' for source set " +
                                std::to_string(string_id));
    }

    // Parse path IDs using integer accumulation (no heap allocation)
    int current_number = -1;
    while (stream_.get(ch) && ch != SET_CLOSE) {
        if (ch == SET_SEPARATOR) {
            if (current_number >= 0) {
                result.insert(current_number);
                current_number = -1;
            }
        } else if (std::isdigit(static_cast<unsigned char>(ch))) {
            current_number = (current_number < 0 ? 0 : current_number * 10) + (ch - '0');
        } else if (!std::isspace(static_cast<unsigned char>(ch))) {
            throw std::runtime_error("Invalid character in source set: " +
                                   std::string(1, ch));
        }
    }

    // Add last number
    if (current_number >= 0) {
        result.insert(current_number);
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

    std::lock_guard<std::mutex> lock(io_mutex_);

    // Check cache first
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

const std::set<int>& Sources::read_source_ref(size_t string_id) const {
    if (string_id >= cardinality_) {
        throw std::out_of_range("String ID " + std::to_string(string_id) +
                               " out of range (cardinality=" + std::to_string(cardinality_) + ")");
    }

    std::lock_guard<std::mutex> lock(io_mutex_);

    // Cache hit: move to front and return reference (splice is iterator-stable)
    auto cache_it = cache_map_.find(string_id);
    if (cache_it != cache_map_.end()) {
        cache_.splice(cache_.begin(), cache_, cache_it->second);
        return cache_it->second->paths;
    }

    // Cache miss: read from file, add to cache, return reference to cached entry
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
    add_to_cache(string_id, std::move(paths));
    return cache_.front().paths;
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
    // Preload both symbols' sources once — eliminates repeated read_source() mutex/cache
    // overhead that would otherwise occur symbol1_size times per symbol2 entry.
    std::vector<std::set<int>> src1(symbol1_size), src2(symbol2_size);
    for (size_t i = 0; i < symbol1_size; ++i)
        src1[i] = read_source(symbol1_start + i);
    for (size_t j = 0; j < symbol2_size; ++j)
        src2[j] = read_source(symbol2_start + j);

    // Bitset fast path: if all path IDs fit in [1, 63], represent each source set as
    // a uint64_t bitmask (bit k-1 = path k); universal marker {0} → ~0ULL.
    bool use_bits = true;
    for (size_t i = 0; i < symbol1_size && use_bits; ++i)
        for (int id : src1[i]) if (id > 63) { use_bits = false; break; }
    for (size_t j = 0; j < symbol2_size && use_bits; ++j)
        for (int id : src2[j]) if (id > 63) { use_bits = false; break; }

    auto to_bits = [](const std::set<int>& s) -> uint64_t {
        if (s.count(0)) return ~0ULL;
        uint64_t b = 0;
        for (int id : s) b |= (1ULL << (id - 1));
        return b;
    };
    auto bits_to_set = [](uint64_t b) -> std::set<int> {
        if (b == ~0ULL) return {0};
        std::set<int> s;
        for (int k = 0; k < 63; ++k)
            if (b & (1ULL << k)) s.insert(k + 1);
        return s;
    };

    std::vector<uint64_t> bits1, bits2;
    if (use_bits) {
        bits1.resize(symbol1_size); bits2.resize(symbol2_size);
        for (size_t i = 0; i < symbol1_size; ++i) bits1[i] = to_bits(src1[i]);
        for (size_t j = 0; j < symbol2_size; ++j) bits2[j] = to_bits(src2[j]);
    }

    std::vector<std::set<int>> merged_sources;
    merged_sources.reserve(symbol1_size * symbol2_size);

    for (size_t i = 0; i < symbol1_size; ++i) {
        for (size_t j = 0; j < symbol2_size; ++j) {
            if (use_bits) {
                uint64_t a = bits1[i], b = bits2[j];
                uint64_t isect = (a == ~0ULL) ? b : (b == ~0ULL) ? a : a & b;
                if (isect == 0) continue;
                merged_sources.push_back(bits_to_set(isect));
            } else {
                std::set<int> isect = intersect_sources(src1[i], src2[j]);
                if (isect.empty()) continue;
                merged_sources.push_back(std::move(isect));
            }
        }
    }

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

    // Read sources on-demand and write, batching each set into a string buffer
    std::string buf;
    buf.reserve(64);
    for (size_t i = 0; i < cardinality_; i++) {
        const std::set<int>& paths = read_source_ref(i);
        buf.clear();
        buf += SET_OPEN;
        bool first = true;
        for (int path_id : paths) {
            if (!first) buf += SET_SEPARATOR;
            // Fast integer-to-string without heap allocation
            char tmp[12];
            int n = path_id, len = 0;
            if (n == 0) { tmp[len++] = '0'; }
            else {
                while (n > 0) { tmp[len++] = '0' + (n % 10); n /= 10; }
                std::reverse(tmp, tmp + len);
            }
            buf.append(tmp, len);
            first = false;
        }
        buf += SET_CLOSE;
        os.write(buf.data(), static_cast<std::streamsize>(buf.size()));
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
// CACHE MANAGEMENT
// ================================================================================

void Sources::add_to_cache(size_t string_id, std::set<int> paths) const {
    if (cache_capacity_ == 0) {
        return;  // Caching disabled
    }

    // Evict if cache full
    if (cache_.size() >= cache_capacity_) {
        size_t evict_id = cache_.back().string_id;
        cache_map_.erase(evict_id);
        cache_.pop_back();
    }

    // Add to front of list (most recently used), moving paths to avoid copy
    cache_.push_front({string_id, std::move(paths)});
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
