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

    if (format_ != Format::SEDS) {
        // Non-SEDS formats (EDZ, EDZ_COMPRESSED) do not have a file-backed
        // byte stream for raw copying.  Fall back to the slow path: read each
        // source set through the LRU cache and re-serialise it.  In practice
        // these formats are not yet fully implemented, so this branch is rarely
        // taken in production.
        for (size_t i = 0; i < count; ++i) {
            const std::set<int> src = read_source(start_idx + i);
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
