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
    , num_paths_(0)
    , is_bitset_(false)
    , cache_capacity_(10000)
{
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
    , num_paths_(other.num_paths_)
    , is_bitset_(other.is_bitset_)
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
        cardinality_  = other.cardinality_;
        format_       = other.format_;
        num_paths_    = other.num_paths_;
        is_bitset_    = other.is_bitset_;
        file_path_    = std::move(other.file_path_);
        stream_       = std::move(other.stream_);
        base_positions_ = std::move(other.base_positions_);
        binary_index_ = std::move(other.binary_index_);
        cache_        = std::move(other.cache_);
        cache_map_    = std::move(other.cache_map_);
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

    if (format == Format::EDZ_COMPRESSED) {
        throw std::runtime_error("EDZ_COMPRESSED format not yet implemented");
    }

    auto sources = std::make_shared<Sources>(0, format);
    sources->file_path_ = path;

    if (format == Format::SEDS) {
        // Open file once — parse_seds() builds index and sets cardinality_ in one pass
        std::ifstream stream(path);
        if (!stream.is_open()) {
            throw std::runtime_error("Failed to open sources file: " + path.string());
        }
        sources->parse_seds(stream);
        if (sources->cardinality_ == 0) {
            throw std::runtime_error("Sources file contains no source sets");
        }
        sources->stream_ = std::move(stream);
    } else {
        // EDZ: must open in binary mode; parse_edz() reads the header + index
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) {
            throw std::runtime_error("Failed to open sources file: " + path.string());
        }
        sources->parse_edz(stream);
        if (sources->cardinality_ == 0) {
            throw std::runtime_error("EDZ file contains no source sets");
        }
        sources->stream_ = std::move(stream);
    }

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

// ─────────────────────────────────────────────────────────────────────────────
// parse_seds — build the random-access index for a .seds file
// ─────────────────────────────────────────────────────────────────────────────
//
// WHAT IS A .seds FILE?
//
//   A SEDS (Sources EDS) file is the companion to every .eds file when source
//   (haplotype / sample) tracking is enabled.  It is a flat sequence of
//   brace-delimited sets of integer path IDs, one set per string in the EDS:
//
//     {1}{2}{3}{4}{1,3}{2,4}...
//
//   Each set tells us which haplotype paths pass through the corresponding
//   string alternative in the EDS.  If string #5 in the EDS can be taken by
//   paths 2 and 4, the fifth set in the SEDS is "{2,4}".
//
//   The number of sets in the SEDS file equals the *total cardinality* of the
//   EDS — the sum of the sizes of all degenerate symbols plus one for every
//   non-degenerate symbol.  This can easily reach several million for a
//   population-scale EDS (e.g. 200 000 symbols × average 4 paths = 800 000
//   entries in 2 MB of text).
//
// WHAT THIS FUNCTION BUILDS
//
//   `base_positions_` is a std::vector<std::streampos> with one entry per SEDS
//   entry.  base_positions_[i] is the byte offset of the opening '{' of the
//   i-th source set.  This is the only index structure we need: given an index
//   i we can seekg() directly to base_positions_[i] and parse the set.
//   Random-access lookup therefore costs O(1) I/O — one seek + one tiny read.
//
// WHY NOT USE tellg() + get() (THE NAIVE APPROACH)?
//
//   The obvious implementation reads one character at a time and calls tellg()
//   before each character to record the position of each '{':
//
//       while (is) {
//           std::streampos pos = is.tellg();
//           if (!is.get(ch)) break;
//           if (ch == '{') { base_positions_.push_back(pos); … }
//       }
//
//   This works but is catastrophically slow for large SEDS files.  On a 2 MB
//   file with 800 000 entries each entry is ~3 bytes, so the loop runs ~2
//   million iterations.  Each iteration calls both tellg() and get().
//
//   On Linux/glibc, tellg() internally calls lseek(fd, 0, SEEK_CUR) to retrieve
//   the current file offset from the kernel — a full round-trip syscall.  Even
//   though the syscall is "free" in the sense that it moves no data, two million
//   syscalls add up to several hundred milliseconds of wall time.  This is why
//   eds2leds --linear was observed spending ~0.2 s per iteration purely in
//   source-file re-indexing (which happens once per iteration of the merge loop).
//
// THE BULK-READ APPROACH USED HERE
//
//   Instead of asking the kernel "where am I?" 2 million times, we read the
//   entire file in large chunks (64 KB each) and track the absolute byte offset
//   ourselves.  A 2 MB file requires only ~32 read() syscalls.  We then scan
//   the in-memory buffer at full CPU speed — a tight loop over a cache-hot
//   array — updating a depth counter and recording positions as we go.
//
//   DEPTH TRACKING:
//     depth == 0 means we are between source sets (outside any braces).
//     depth == 1 means we are inside the outer '{…}' of a source set.
//     depth > 1 would mean nested braces, which the SEDS format does not use;
//     the code handles it defensively anyway.
//
//     When we see '{' at depth 0 we record the current byte position
//     (file_offset + i) and then increment depth.
//     When we see '}' we decrement depth; reaching 0 again means we have
//     exited a complete source set.
//
//   POSITION ARITHMETIC:
//     file_offset is the byte offset of buf[0] in the file, updated by n
//     (the number of bytes actually read) after each chunk.  The byte offset
//     of buf[i] is therefore exactly (file_offset + i), and that is what we
//     store in base_positions_.  This value is safe to pass to seekg() later.
//
//   CHUNK SIZE (64 KB):
//     A 64 KB buffer sits comfortably in L2 cache on modern CPUs, so the inner
//     scan loop runs from cache rather than from RAM.  The specific value is not
//     critical; anything from 8 KB to 1 MB would give similar throughput.
//
// ─────────────────────────────────────────────────────────────────────────────
void Sources::parse_seds(std::istream& is) {
    base_positions_.clear();
    // Reserve capacity upfront if cardinality is already known (e.g. when
    // loading a file whose EDS has already told us its string count).
    // Avoids repeated reallocation as base_positions_ grows.
    if (cardinality_ > 0) {
        base_positions_.reserve(cardinality_);
    }

    constexpr std::streamsize CHUNK = 64 * 1024;   // 64 KB per I/O call
    std::vector<char> buf(CHUNK);
    std::streamoff file_offset = 0;  // byte offset of buf[0] in the file
    int depth = 0;                   // brace nesting depth (0 = between sets)

    while (is) {
        // Pull the next chunk from the file into buf[0..n-1].
        // gcount() tells us how many bytes were actually delivered, which may
        // be less than CHUNK at the end of the file or on a short read.
        is.read(buf.data(), CHUNK);
        std::streamsize n = is.gcount();
        if (n <= 0) break;

        // Scan every byte in the chunk.  This is the hot loop: for a 2 MB file
        // we visit ~2 million bytes total, but spread over ~32 iterations of
        // the outer while loop.  The inner loop body is a handful of cheap
        // comparisons; the branch predictor quickly learns the dominant path
        // (most bytes are digits or commas, not braces).
        for (std::streamsize i = 0; i < n; ++i) {
            const char ch = buf[i];

            if (ch == SET_OPEN) {
                // Opening brace '{':
                //   If we are at depth 0 this is the start of a new source set.
                //   Record the file byte offset of this '{' — that is exactly
                //   the position we will need to seekg() to later when a caller
                //   wants to read this particular source set.
                if (depth == 0) {
                    base_positions_.push_back(
                        static_cast<std::streampos>(file_offset + i));
                }
                ++depth;   // Enter one level of nesting.

            } else if (ch == SET_CLOSE) {
                // Closing brace '}': exit one level of nesting.
                // When depth returns to 0 we have finished one complete source
                // set.  We do not need to act here; the next '{' at depth 0 will
                // begin the next set.
                --depth;

            } else if (depth == 0 && !std::isspace(static_cast<unsigned char>(ch))) {
                // Any non-whitespace character outside a brace pair is malformed.
                // Whitespace between sets (e.g. a trailing newline at end of file)
                // is tolerated silently; anything else is a format violation.
                throw std::runtime_error(
                    "Unexpected character in sEDS file: " + std::string(1, ch));
            }
            // Characters inside a set (digits, commas) are deliberately ignored
            // here — we are only building an index of where each set *starts*,
            // not parsing the set contents.  Parsing happens lazily in
            // read_from_seds() when a caller actually needs the path IDs.
        }

        // Advance the running file offset by the number of bytes we processed.
        file_offset += n;
    }

    // ── Validate / set cardinality ───────────────────────────────────────────
    // base_positions_.size() is now the total number of source sets found.
    // This must match cardinality_ (the expected number of strings) if it was
    // known in advance; otherwise we use the parsed count to initialise it.
    const size_t string_count = base_positions_.size();

    if (cardinality_ == 0) {
        // Auto-detect mode: cardinality was left as 0 when the Sources object
        // was constructed (typically during Sources::load()).  Set it now.
        cardinality_ = string_count;
    } else if (string_count != cardinality_) {
        // Mismatch: the caller told us to expect N sets but we found M.
        // This usually means a stale .seds file was paired with the wrong .eds.
        throw std::runtime_error("sEDS: Source count (" +
            std::to_string(string_count) + ") does not match cardinality (" +
            std::to_string(cardinality_) + ")");
    }
}

// ================================================================================
// EDZ BINARY FORMAT HELPERS
// ================================================================================
//
// EDZ file layout (all integers little-endian):
//
//   Header (16 bytes):
//     [0..3]  magic: 'E','D','Z','\0'
//     [4..5]  flags: uint16_t  — bit 0 = compressed (0 here for EDZ)
//     [6..7]  reserved: uint16_t = 0
//     [8..15] cardinality: uint64_t  — number of source sets
//
//   Index section (cardinality * 12 bytes):
//     For each i: data_offset:uint64_t + data_size:uint32_t
//     data_offset is an absolute byte offset into the file.
//
//   Data section (variable):
//     For each i: varint(path_count) followed by path_count varints(path_id).
//     Varints use unsigned LEB128 (ULEB128) encoding.
//
// The index section immediately follows the header, and the data section
// immediately follows the index. Entry i starts at binary_index_[i].first.

static constexpr uint64_t EDZ_HEADER_SIZE      = 16;
static constexpr uint64_t EDZ_INDEX_ENTRY_SIZE = 12;  // uint64_t + uint32_t

// Write a value of type T as little-endian bytes.
template<typename T>
static void write_le(std::ostream& os, T value) {
    static_assert(std::is_unsigned<T>::value, "write_le requires unsigned integer");
    uint8_t buf[sizeof(T)];
    for (size_t i = 0; i < sizeof(T); ++i) {
        buf[i] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }
    os.write(reinterpret_cast<const char*>(buf), sizeof(T));
}

// Read a little-endian value of type T from a binary stream.
template<typename T>
static T read_le(std::istream& is) {
    static_assert(std::is_unsigned<T>::value, "read_le requires unsigned integer");
    uint8_t buf[sizeof(T)];
    is.read(reinterpret_cast<char*>(buf), sizeof(T));
    if (static_cast<size_t>(is.gcount()) != sizeof(T)) {
        throw std::runtime_error("EDZ: unexpected EOF reading header or index");
    }
    T value = T{};
    for (size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(buf[i]) << (i * 8);
    }
    return value;
}

// Append the ULEB128 encoding of value to out.
static void varint_encode(uint64_t value, std::vector<uint8_t>& out) {
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value != 0) byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
}

// Decode one ULEB128 value from data[0..data_size), advancing offset past it.
static uint64_t varint_decode(const uint8_t* data, size_t data_size, size_t& offset) {
    uint64_t result = 0;
    int shift = 0;
    while (offset < data_size) {
        uint8_t byte = data[offset++];
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        shift += 7;
        if (!(byte & 0x80)) return result;
        if (shift >= 64) throw std::runtime_error("EDZ: varint overflow");
    }
    throw std::runtime_error("EDZ: truncated varint");
}

// ================================================================================
// PARSING - BINARY FORMATS
// ================================================================================

void Sources::parse_edz(std::istream& is) {
    char magic[4];
    is.read(magic, 4);
    if (is.gcount() != 4 || magic[0] != 'E' || magic[1] != 'D' ||
            magic[2] != 'Z' || magic[3] != '\0') {
        throw std::runtime_error("EDZ: invalid magic bytes (not an EDZ file)");
    }

    uint16_t flags = read_le<uint16_t>(is);
    if (flags & 0x0001) {
        throw std::runtime_error(
            "EDZ: compression flag is set — use EDZ_COMPRESSED format instead");
    }
    read_le<uint16_t>(is);  // reserved

    uint64_t card = read_le<uint64_t>(is);

    if (flags & 0x0002) {
        // ── Bitset EDZ format ────────────────────────────────────────────────
        // Header layout (24 bytes):
        //   [0..3]   'E','D','Z','\0'
        //   [4..5]   flags = 0x0002
        //   [6..7]   reserved
        //   [8..15]  cardinality (may be 0 if written by a streaming writer that
        //            could not seek back; compute from file size in that case)
        //   [16..23] num_paths
        uint64_t np = read_le<uint64_t>(is);
        if (np == 0) throw std::runtime_error("EDZ bitset: num_paths is 0 in header");
        num_paths_ = static_cast<size_t>(np);
        is_bitset_ = true;

        // If cardinality was not written (placeholder 0), compute from file size.
        if (card == 0) {
            auto cur = is.tellg();
            is.seekg(0, std::ios::end);
            auto sz = is.tellg();
            is.seekg(cur);
            if (sz <= 24) throw std::runtime_error("EDZ bitset: file too small");
            card = static_cast<uint64_t>((static_cast<size_t>(sz) - 24) / edz_bpe(num_paths_));
        }

        // Validate or set cardinality_
        if (cardinality_ == 0) {
            cardinality_ = static_cast<size_t>(card);
        } else if (cardinality_ != static_cast<size_t>(card)) {
            throw std::runtime_error("EDZ bitset: cardinality mismatch (expected " +
                std::to_string(cardinality_) + ", got " + std::to_string(card) + ")");
        }
        // No index section; entries start immediately at byte 24.
        return;
    }

    // ── Legacy varint EDZ format ─────────────────────────────────────────────
    if (card == 0) throw std::runtime_error("EDZ: file declares cardinality 0");

    binary_index_.resize(static_cast<size_t>(card));
    for (uint64_t i = 0; i < card; ++i) {
        uint64_t offset = read_le<uint64_t>(is);
        uint32_t size   = read_le<uint32_t>(is);
        binary_index_[static_cast<size_t>(i)] = {offset, size};
    }

    if (cardinality_ == 0) {
        cardinality_ = static_cast<size_t>(card);
    } else if (cardinality_ != static_cast<size_t>(card)) {
        throw std::runtime_error("EDZ: cardinality mismatch (expected " +
            std::to_string(cardinality_) + ", got " + std::to_string(card) + ")");
    }
}

void Sources::parse_edz_compressed(std::istream& is) {
    throw std::runtime_error("EDZ_COMPRESSED format parsing not yet implemented");
}

// ================================================================================
// STREAMING ACCESS
// ================================================================================

PathSet Sources::read_from_seds(size_t string_id) const {
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
    PathSet result;
    char ch;

    // Expect '{'
    if (!stream_.get(ch) || ch != SET_OPEN) {
        throw std::runtime_error("Expected '{' for source set " +
                                std::to_string(string_id));
    }

    // Parse path IDs. Supports:
    //   {1,3,7}        — explicit list (legacy and current format)
    //   {1-3,7}        — range notation: 1-3 expands to 1,2,3
    //   {0}            — universal set (all paths); 0 is the complement sentinel
    //   {0,5-10,20}    — complement set: all paths EXCEPT 5,6,7,8,9,10,20
    int current_number = -1;
    int range_start    = -1;  // set when we've seen 'a-' waiting for range end

    auto flush_token = [&]() {
        if (current_number < 0) return;
        if (range_start >= 0) {
            // Expand range [range_start, current_number]
            for (int p = range_start; p <= current_number; ++p)
                result.push_back(p);
            range_start = -1;
        } else {
            result.push_back(current_number);
        }
        current_number = -1;
    };

    while (stream_.get(ch) && ch != SET_CLOSE) {
        if (ch == SET_SEPARATOR) {
            flush_token();
        } else if (ch == '-' && current_number >= 0) {
            // Range separator: current_number is the start of the range
            range_start    = current_number;
            current_number = -1;
        } else if (std::isdigit(static_cast<unsigned char>(ch))) {
            current_number = (current_number < 0 ? 0 : current_number * 10) + (ch - '0');
        } else if (!std::isspace(static_cast<unsigned char>(ch))) {
            throw std::runtime_error("Invalid character in source set: " +
                                   std::string(1, ch));
        }
    }
    flush_token();  // final token before '}'

    // Maintain PathSet invariant: sorted (0 first if present), deduplicated.
    // sort() naturally places 0 at front, which is the complement-flag convention.
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    return result;
}

// ── EDZ bitset encode / decode ────────────────────────────────────────────────

void Sources::pathset_to_bitset(uint8_t* buf, size_t bpe, size_t num_paths,
                                const PathSet& ps) {
    if (!ps.empty() && ps[0] == 0) {
        // Complement / universal: start all-ones, clear exception bits
        std::fill(buf, buf + bpe, uint8_t{0xFF});
        // Clear any padding bits beyond num_paths
        if (num_paths % 8 != 0) {
            buf[bpe - 1] = static_cast<uint8_t>((1u << (num_paths % 8)) - 1u);
        }
        // Clear exception bits (ps[1..])
        for (size_t i = 1; i < ps.size(); ++i) {
            int id = ps[i];
            if (id >= 1 && static_cast<size_t>(id) <= num_paths) {
                size_t bit = static_cast<size_t>(id - 1);
                buf[bit / 8] &= ~(uint8_t{1} << (bit % 8));
            }
        }
    } else {
        std::fill(buf, buf + bpe, uint8_t{0});
        for (int id : ps) {
            if (id >= 1 && static_cast<size_t>(id) <= num_paths) {
                size_t bit = static_cast<size_t>(id - 1);
                buf[bit / 8] |= (uint8_t{1} << (bit % 8));
            }
        }
    }
}

PathSet Sources::bitset_to_pathset(const uint8_t* buf, size_t bpe, size_t num_paths) {
    // Check for all-ones (universal set → PathSet{0})
    bool all_ones = true;
    for (size_t i = 0; i < bpe && all_ones; ++i) {
        uint8_t expected = (i + 1 < bpe) ? 0xFF
                         : (num_paths % 8 == 0) ? 0xFF
                         : static_cast<uint8_t>((1u << (num_paths % 8)) - 1u);
        if (buf[i] != expected) all_ones = false;
    }
    if (all_ones) return {0};

    PathSet result;
    result.reserve(num_paths / 4);  // rough estimate
    for (size_t bit = 0; bit < num_paths; ++bit) {
        if (buf[bit / 8] & (uint8_t{1} << (bit % 8)))
            result.push_back(static_cast<int>(bit + 1));
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────

PathSet Sources::read_from_edz(size_t string_id) const {
    if (!stream_.is_open()) {
        throw std::runtime_error("EDZ stream not open");
    }

    if (is_bitset_) {
        // Bitset format: fixed-size entry at a known byte offset (no index lookup)
        size_t bpe    = edz_bpe(num_paths_);
        size_t offset = 24 + string_id * bpe;
        auto target   = static_cast<std::streampos>(offset);
        if (!stream_.good() || stream_.tellg() != target) {
            stream_.clear();
            stream_.seekg(target);
        }
        std::vector<uint8_t> buf(bpe);
        stream_.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(bpe));
        if (static_cast<size_t>(stream_.gcount()) != bpe) {
            throw std::runtime_error("EDZ bitset: short read for source set " +
                                     std::to_string(string_id));
        }
        return bitset_to_pathset(buf.data(), bpe, num_paths_);
    }

    // Legacy varint format
    const uint64_t offset = binary_index_[string_id].first;
    const uint32_t size   = binary_index_[string_id].second;

    auto target = static_cast<std::streampos>(offset);
    if (!stream_.good() || stream_.tellg() != target) {
        stream_.clear();
        stream_.seekg(target);
    }

    std::vector<uint8_t> buf(size);
    stream_.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    if (static_cast<uint32_t>(stream_.gcount()) != size) {
        throw std::runtime_error("EDZ: short read for source set " +
                                 std::to_string(string_id));
    }

    size_t pos = 0;
    uint64_t count = varint_decode(buf.data(), size, pos);
    PathSet result;
    result.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        result.push_back(static_cast<int>(varint_decode(buf.data(), size, pos)));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

PathSet Sources::read_from_edz_compressed(size_t string_id) const {
    throw std::runtime_error("EDZ_COMPRESSED streaming not yet implemented (Phase 3)");
}

// ================================================================================
// PUBLIC ACCESS
// ================================================================================

// ─────────────────────────────────────────────────────────────────────────────
// copy_range_to_stream — bulk-copy a contiguous slice of source sets to output
// ─────────────────────────────────────────────────────────────────────────────
//
// PURPOSE
//
//   During eds2leds --linear, every EDS symbol that is passed through unchanged
//   (not involved in a merge) must have its source sets copied verbatim from
//   the input SEDS file to the output SEDS file.  This function performs that
//   copy for a contiguous range of string indices [start_idx, start_idx+count).
//
//   Example: if the EDS symbol at position 42 has 4 string alternatives (paths
//   1–4, assigned round-robin), its SEDS entries might be:
//     {1}{2}{3}{4}
//   and copy_range_to_stream(global_idx_of_symbol_42, 4, output) writes exactly
//   those 12 bytes to the output SEDS stream.
//
// PERFORMANCE HISTORY — why this function is performance-critical
//
//   The original naive implementation called read_source() per string, parsed
//   each source set into a std::set<int>, then re-serialised it character by
//   character.  For 200 000 symbols × 4 strings = 800 000 calls, with each
//   call incurring a seekg() (lseek syscall + buffer invalidation) and a parse,
//   this dominated the wall time of eds2leds --linear.
//
//   An intermediate version used 64-KB block reads with brace counting to copy
//   raw bytes instead of parse-and-reserialise.  This was correct but still
//   called seekg() per symbol, and after each 64-KB read the stream sat 64 KB
//   ahead of where it needed to be for the next symbol — forcing another seekg()
//   to jump backward.
//
//   The current implementation (described below) eliminates both problems.
//
// THE TWO PATHS
//
// ── Fast path (almost always taken) ─────────────────────────────────────────
//
//   PRECONDITION: start_idx + count < base_positions_.size()
//   (i.e. we are NOT copying the very last batch of strings in the file)
//
//   Because base_positions_[start_idx + count] is the byte offset of the first
//   character of the entry *after* our range, the exact number of bytes to copy
//   is simply:
//
//     byte_count = base_positions_[start_idx + count] - base_positions_[start_idx]
//
//   This calculation is free — it is arithmetic on two values already in RAM.
//   We then:
//     1. Position the stream at base_positions_[start_idx]   (see seek guard below)
//     2. Read exactly byte_count bytes in 64-KB chunks
//     3. Write each chunk to `out`
//
//   After the read, the stream's internal file pointer is sitting exactly at
//   base_positions_[start_idx + count] — the start of the next entry.  If the
//   next call to copy_range_to_stream() is for start_idx' = start_idx + count
//   (i.e. sequential access, which is the common case), the seek guard fires
//   and the seekg() is skipped entirely.
//
//   SEEK GUARD:
//     The condition  `!stream_.good() || stream_.tellg() != target`  is the same
//     "skip redundant seek" guard used in read_symbol_from_stream().
//     stream_.tellg() does NOT cause a syscall on a healthy buffered stream; the
//     C++ library returns the position from its internal buffer state.
//     If the stream is healthy and already at target, we skip seekg() — saving
//     one lseek(2) syscall and avoiding buffer invalidation.
//
//   WHY NO OVER-READ MATTERS:
//     The original brace-counting version read exactly CHUNK (64 KB) bytes per
//     call regardless of how many bytes were actually needed.  For a typical
//     SEDS batch covering one symbol (≈ 12 bytes) we were reading 64 KB and
//     using 12 bytes — the stream ended up 64 000 bytes past the target, forcing
//     a backward seek on the next call.  With the exact byte_count approach we
//     read only what we need; the stream ends up at the right place and no
//     backward seek is ever required.
//
// ── Fallback path (last batch only) ─────────────────────────────────────────
//
//   PRECONDITION: start_idx + count == base_positions_.size()
//   (i.e. this range extends to the very last string in the file)
//
//   We do not have a base_positions_[start_idx + count] sentinel because
//   parse_seds() records only the *start* positions of entries, not a trailing
//   end position.  The byte count is therefore unknown without scanning.
//
//   We fall back to the original brace-counting approach: read in 64-KB chunks,
//   count '{' and '}' to track nesting depth, and stop when the last expected
//   '}' at depth 0 is found.  The cutoff variable records how many bytes of the
//   final chunk are part of our range, so we do not write trailing garbage.
//
//   This path fires at most once per call to eds_to_leds_linear() (the very
//   last flush_seds_batch() at the end of stream_merged_symbols_to_file()).
//   Its performance does not matter in practice.
//
// THREAD SAFETY
//
//   stream_ and base_positions_ are both protected by io_mutex_.  This function
//   acquires the mutex for its entire duration, which is safe because
//   copy_range_to_stream() is called only from the single streaming thread
//   inside stream_merged_symbols_to_file(), never from parallel workers.
//
// ─────────────────────────────────────────────────────────────────────────────
void Sources::copy_range_to_stream(size_t start_idx, size_t count, std::ostream& out) const {
    if (count == 0) return;

    if (format_ == Format::EDZ && is_bitset_) {
        // EDZ bitset: entries are fixed-size; copy raw bytes then re-serialise
        // each one as SEDS text (range+complement) for the SEDS temp-file output.
        // A future optimisation could write EDZ directly when the output is also EDZ.
        std::lock_guard<std::mutex> lock(io_mutex_);
        const size_t bpe = edz_bpe(num_paths_);
        const size_t byte_start = 24 + start_idx * bpe;
        auto target = static_cast<std::streampos>(byte_start);
        if (!stream_.good() || stream_.tellg() != target) {
            stream_.clear();
            stream_.seekg(target);
        }
        std::vector<uint8_t> buf(bpe);
        for (size_t i = 0; i < count; ++i) {
            stream_.read(reinterpret_cast<char*>(buf.data()),
                         static_cast<std::streamsize>(bpe));
            if (static_cast<size_t>(stream_.gcount()) != bpe)
                throw std::runtime_error("EDZ bitset copy_range: short read");
            PathSet ps = bitset_to_pathset(buf.data(), bpe, num_paths_);
            // Re-serialise to SEDS using range+complement encoding
            out << '{';
            bool is_compl = !ps.empty() && ps[0] == 0;
            if (is_compl) {
                out << '0';
                // Exceptions: ps[1..]
                int prev_end = -1;
                for (size_t j = 1; j < ps.size(); ++j) {
                    out << ',';
                    int lo = ps[j], hi = lo;
                    while (j + 1 < ps.size() && ps[j+1] == hi + 1) { ++j; ++hi; }
                    if (hi > lo + 1) out << lo << '-' << hi;
                    else if (hi == lo + 1) out << lo << ',' << hi;
                    else out << lo;
                    (void)prev_end; prev_end = hi;
                }
            } else {
                for (size_t j = 0; j < ps.size(); ) {
                    if (j > 0) out << ',';
                    int lo = ps[j], hi = lo;
                    while (j + 1 < ps.size() && ps[j+1] == hi + 1) { ++j; ++hi; }
                    if (hi > lo + 1) out << lo << '-' << hi;
                    else if (hi == lo + 1) out << lo << ',' << hi;
                    else out << lo;
                    ++j;
                }
            }
            out << '}';
        }
        return;
    }

    if (format_ != Format::SEDS) {
        // Legacy varint EDZ or EDZ_COMPRESSED: read through LRU cache and
        // re-serialise to SEDS text.
        for (size_t i = 0; i < count; ++i) {
            const PathSet src = read_source(start_idx + i);
            out << '{';
            bool first = true;
            for (int p : src) {
                if (!first) out << ',';
                out << p;
                first = false;
            }
            out << '}';
        }
        return;
    }

    // Acquire the I/O mutex for the entire copy.  stream_ is shared state; we
    // must not interleave reads from different threads.
    std::lock_guard<std::mutex> lock(io_mutex_);

    if (start_idx >= base_positions_.size()) {
        throw std::out_of_range("copy_range_to_stream: start_idx out of range");
    }

    // The byte offset in the SEDS file where our range begins.
    auto target = base_positions_[start_idx];

    // ════════════════════════════════════════════════════════════════════════
    // FAST PATH — exact byte range known from index
    // ════════════════════════════════════════════════════════════════════════
    if (start_idx + count < base_positions_.size()) {
        // base_positions_[start_idx + count] is the start of the *next* source
        // set after our range, so the byte count we need to copy is the
        // difference between these two file positions.
        auto end_pos    = base_positions_[start_idx + count];
        auto byte_count = static_cast<std::streamoff>(end_pos)
                        - static_cast<std::streamoff>(target);

        if (byte_count > 0) {
            // ── Seek guard ───────────────────────────────────────────────────
            // Only call seekg() if the stream is not already at `target`.
            // In the common sequential pattern (each call covers the strings
            // immediately following the previous call's range) the prior call
            // left the stream exactly at base_positions_[start_idx] = target,
            // so this guard fires and we skip the seek + buffer invalidation.
            if (!stream_.good() || stream_.tellg() != target) {
                stream_.clear();
                stream_.seekg(target);
            }

            // ── Read exactly byte_count bytes and forward to output ──────────
            // We process in 64-KB chunks to bound stack usage and keep each
            // read() call at a sensible size.  For small batches (< 64 KB)
            // this loop executes only once.
            constexpr std::streamsize CHUNK = 64 * 1024;
            char buf[CHUNK];
            std::streamoff remaining = byte_count;

            while (remaining > 0 && stream_.good()) {
                // Do not request more than `remaining` bytes; that would
                // advance the stream past the end of our range and break
                // the sequential-seek optimisation for the next call.
                std::streamsize to_read = static_cast<std::streamsize>(
                    std::min(remaining, static_cast<std::streamoff>(CHUNK)));
                stream_.read(buf, to_read);
                std::streamsize n = stream_.gcount();
                if (n <= 0) break;
                out.write(buf, n);
                remaining -= n;
            }
            // After this loop the stream's file pointer is at
            // base_positions_[start_idx + count] — the start of the next entry.
            // The next sequential call will find tellg() == its target and
            // skip its own seekg().
        }
        return;
    }

    // ════════════════════════════════════════════════════════════════════════
    // FALLBACK PATH — last batch, end position not in index
    // ════════════════════════════════════════════════════════════════════════
    //
    // This path handles the case where start_idx + count == base_positions_.size(),
    // meaning our range reaches the very end of the SEDS file.  We do not have
    // a sentinel entry for the position after the last set, so we cannot compute
    // an exact byte count.  Instead, we read in 64-KB chunks and count braces
    // to detect when we have consumed exactly `count` complete source sets.
    //
    // `depth` tracks brace nesting (0 = outside a set, 1 = inside the outer
    // '{…}').  Each time depth returns to 0 we have finished one set; when the
    // sets_remaining counter hits 0 we compute `cutoff` — the index of the
    // last byte we should write — and exit.  Bytes after cutoff in the final
    // chunk are not written (they might be trailing newline/whitespace or
    // garbage from a short read at EOF).
    //
    // This path fires at most once per transformation (the final flush at the
    // end of stream_merged_symbols_to_file()) so its performance is irrelevant.

    if (!stream_.good() || stream_.tellg() != target) {
        stream_.clear();
        stream_.seekg(target);
    }

    constexpr std::streamsize CHUNK = 64 * 1024;
    char buf[CHUNK];
    int sets_remaining = static_cast<int>(count);  // how many sets we still need to write
    int depth = 0;                                  // current brace nesting depth

    while (sets_remaining > 0 && stream_.good()) {
        stream_.read(buf, CHUNK);
        std::streamsize n = stream_.gcount();
        if (n <= 0) break;

        // Scan the buffer for brace characters and count complete sets.
        // cutoff starts at n (write the whole chunk) and is shortened if we
        // find the last required closing brace before the end of the chunk.
        std::streamsize cutoff = n;
        for (std::streamsize i = 0; i < n; ++i) {
            if (buf[i] == SET_OPEN) {
                ++depth;
            } else if (buf[i] == SET_CLOSE) {
                --depth;
                if (depth == 0) {
                    // We just closed one complete source set.
                    --sets_remaining;
                    if (sets_remaining == 0) {
                        // This '}' is the last byte we need.  Set cutoff to
                        // include this character and exit the scan loop.
                        cutoff = i + 1;
                        break;
                    }
                }
            }
        }
        out.write(buf, cutoff);
    }
}

PathSet Sources::read_source(size_t string_id) const {
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
    PathSet paths;
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

const PathSet& Sources::read_source_ref(size_t string_id) const {
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
    PathSet paths;
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

PathSet Sources::intersect_sources(
    const PathSet& sources1,
    const PathSet& sources2
) {
    // Convention: a PathSet whose first element is 0 is a complement set.
    //   {0}          = universal (all paths) — complement of {}
    //   {0,5,10}     = all paths EXCEPT 5 and 10
    //   {3,7}        = exactly paths 3 and 7 (normal explicit set)
    //
    // 0 is never a valid path ID (path IDs are 1-indexed), so this is unambiguous.
    //
    // Intersection cases:
    //   explicit  ∩ explicit    → standard std::set_intersection
    //   compl(E1) ∩ compl(E2)  → complement of (E1 ∪ E2)   i.e. {0} + union of exceptions
    //   compl(E)  ∩ explicit S → S \ E
    //   explicit S ∩ compl(E)  → S \ E

    bool c1 = !sources1.empty() && sources1.front() == 0;
    bool c2 = !sources2.empty() && sources2.front() == 0;

    if (!c1 && !c2) {
        PathSet result;
        std::set_intersection(sources1.begin(), sources1.end(),
                              sources2.begin(), sources2.end(),
                              std::back_inserter(result));
        return result;
    }

    if (c1 && c2) {
        // compl(E1) ∩ compl(E2) = compl(E1 ∪ E2)
        PathSet exceptions;
        std::set_union(sources1.begin() + 1, sources1.end(),
                       sources2.begin() + 1, sources2.end(),
                       std::back_inserter(exceptions));
        PathSet result = {0};
        result.insert(result.end(), exceptions.begin(), exceptions.end());
        return result;
    }

    // One complement, one explicit: result = explicit \ exceptions
    const PathSet& compl_set = c1 ? sources1 : sources2;
    const PathSet& expl_set  = c1 ? sources2 : sources1;
    PathSet result;
    std::set_difference(expl_set.begin(), expl_set.end(),
                        compl_set.begin() + 1, compl_set.end(),
                        std::back_inserter(result));
    return result;
}

std::vector<PathSet> Sources::merge_adjacent_sources(
    size_t symbol1_start, size_t symbol1_size,
    size_t symbol2_start, size_t symbol2_size
) const {
    // Preload both symbols' sources once — eliminates repeated read_source() mutex/cache
    // overhead that would otherwise occur symbol1_size times per symbol2 entry.
    std::vector<PathSet> src1(symbol1_size), src2(symbol2_size);
    for (size_t i = 0; i < symbol1_size; ++i)
        src1[i] = read_source(symbol1_start + i);
    for (size_t j = 0; j < symbol2_size; ++j)
        src2[j] = read_source(symbol2_start + j);

    // Bitset fast path: if all path IDs fit in [1, 63], represent each source set as
    // a uint64_t bitmask (bit k-1 = path k); universal marker {0} → ~0ULL.
    // Sorted vector: max element is at back(), so the check is O(1) per set.
    bool use_bits = true;
    for (size_t i = 0; i < symbol1_size && use_bits; ++i)
        if (!src1[i].empty() && src1[i].back() > 63) use_bits = false;
    for (size_t j = 0; j < symbol2_size && use_bits; ++j)
        if (!src2[j].empty() && src2[j].back() > 63) use_bits = false;

    auto to_bits = [](const PathSet& s) -> uint64_t {
        if (!s.empty() && s[0] == 0) {
            // Complement set: start with all-ones and clear exception bits
            uint64_t b = ~0ULL;
            for (size_t i = 1; i < s.size(); ++i)
                b &= ~(1ULL << (s[i] - 1));
            return b;
        }
        uint64_t b = 0;
        for (int id : s) b |= (1ULL << (id - 1));
        return b;
    };
    // bits_to_set produces a sorted PathSet (k increases 0..62 → IDs 1..63 in order).
    auto bits_to_set = [](uint64_t b) -> PathSet {
        if (b == ~0ULL) return {0};
        PathSet s;
        for (int k = 0; k < 63; ++k)
            if (b & (1ULL << k)) s.push_back(k + 1);
        return s;
    };

    std::vector<uint64_t> bits1, bits2;
    if (use_bits) {
        bits1.resize(symbol1_size); bits2.resize(symbol2_size);
        for (size_t i = 0; i < symbol1_size; ++i) bits1[i] = to_bits(src1[i]);
        for (size_t j = 0; j < symbol2_size; ++j) bits2[j] = to_bits(src2[j]);
    }

    std::vector<PathSet> merged_sources;
    merged_sources.reserve(symbol1_size * symbol2_size);

    for (size_t i = 0; i < symbol1_size; ++i) {
        for (size_t j = 0; j < symbol2_size; ++j) {
            if (use_bits) {
                uint64_t a = bits1[i], b = bits2[j];
                uint64_t isect = (a == ~0ULL) ? b : (b == ~0ULL) ? a : a & b;
                if (isect == 0) continue;
                merged_sources.push_back(bits_to_set(isect));
            } else {
                PathSet isect = intersect_sources(src1[i], src2[j]);
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
        const PathSet& paths = read_source_ref(i);
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
    // If num_paths_ was not explicitly set (e.g. loaded from old varint-EDZ that
    // didn't store num_paths), derive it by scanning all entries for the max path ID.
    size_t np = num_paths_;
    if (np == 0) {
        for (size_t i = 0; i < cardinality_; ++i) {
            PathSet ps = read_source(i);
            for (int id : ps) {
                if (id > 0 && static_cast<size_t>(id) > np)
                    np = static_cast<size_t>(id);
            }
        }
        if (np == 0) np = 1;  // degenerate fallback
    }

    std::ofstream os(path, std::ios::binary);
    if (!os) throw std::runtime_error("Failed to open for writing: " + path.string());

    // Write header (placeholder cardinality; filled in at the end via seekp)
    write_edz_header(os, np);

    // Write one bitset entry per source set
    const size_t bpe = edz_bpe(np);
    std::vector<uint8_t> buf(bpe);
    for (size_t i = 0; i < cardinality_; ++i) {
        const PathSet ps = read_source(i);
        pathset_to_bitset(buf.data(), bpe, np, ps);
        os.write(reinterpret_cast<const char*>(buf.data()),
                 static_cast<std::streamsize>(bpe));
    }

    write_edz_finalize(os, cardinality_);
}

// ── Static streaming write API ────────────────────────────────────────────────
// Allows vcf2eds / msa2eds to write an EDZ file entry-by-entry during streaming
// without building a Sources object in memory.

void Sources::write_edz_header(std::ostream& os, size_t num_paths) {
    os.write("EDZ\0", 4);
    write_le<uint16_t>(os, uint16_t{0x0002});   // flags: bitset format
    write_le<uint16_t>(os, uint16_t{0});         // reserved
    write_le<uint64_t>(os, uint64_t{0});         // cardinality placeholder
    write_le<uint64_t>(os, static_cast<uint64_t>(num_paths));
}

void Sources::write_edz_entry(std::ostream& os, const PathSet& paths, size_t num_paths) {
    const size_t bpe = edz_bpe(num_paths);
    std::vector<uint8_t> buf(bpe);
    pathset_to_bitset(buf.data(), bpe, num_paths, paths);
    os.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(bpe));
}

void Sources::write_edz_finalize(std::ostream& os, size_t cardinality) {
    // Seek back to bytes [8..15] and overwrite the cardinality placeholder.
    os.seekp(8);
    write_le<uint64_t>(os, static_cast<uint64_t>(cardinality));
    os.seekp(0, std::ios::end);
}

void Sources::save_edz_compressed(const std::filesystem::path& path) const {
    throw std::runtime_error("EDZ_COMPRESSED format saving not yet implemented (Phase 3)");
}

// ================================================================================
// CACHE MANAGEMENT
// ================================================================================

void Sources::add_to_cache(size_t string_id, PathSet paths) const {
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
