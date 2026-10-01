// EDS implementation: parsing, serialisation, on-demand symbol reads and
// pattern generation.
//
// Two construction paths share one representation. The stream and string
// constructors fill sets_ and never touch the disk again; EDS::load() leaves
// sets_ empty and keeps stream_ open, serving each symbol from its indexed byte
// span. Every method below must work under both, which is why reads go through
// read_symbol() / symbol_view() rather than touching sets_ directly.
#include "eds.hpp"
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <cctype>
#include <random>
#include <optional>
#include <unordered_set>

namespace edsparser {

// ================================================================================
// CONSTRUCTORS & PARSING
// ================================================================================

// Stream-based constructor (EDS only, no sources)
// Keeps strings in sets_ so read_symbol() works without a file.
EDS::EDS(std::istream& eds_stream) : is_empty_(false), sources_(nullptr) {
    parse(eds_stream, /*with_strings=*/true);
}

// String-based constructor (EDS only, no sources)
// Keeps strings in sets_ so read_symbol() works without a file.
EDS::EDS(const std::string& eds_string) : is_empty_(false), sources_(nullptr) {
    std::string normalized = normalize_eds_format(eds_string);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
        [](unsigned char c) { return std::isspace(c); }), normalized.end());
    std::istringstream iss(normalized);
    parse(iss, /*with_strings=*/true);
}

void EDS::parse(std::istream& is, bool with_strings) {
    // Streaming parser: builds metadata index.
    // When with_strings=true also populates sets_ for in-memory read_symbol() access.

    // Clear all data structures
    sets_.clear();
    metadata_.base_positions.clear();
    metadata_.symbol_sizes.clear();
    metadata_.string_lengths.clear();
    metadata_.cum_set_sizes.clear();
    metadata_.is_degenerate.clear();

    n_ = 0;
    N_ = 0;
    m_ = 0;

    // Running statistics accumulated inline (avoids a separate calculate_statistics() pass)
    metadata_.num_degenerate_symbols = 0;
    metadata_.num_common_chars = 0;
    metadata_.total_change_size = 0;
    metadata_.num_empty_strings = 0;

    // Lazy position-lookup array: cleared here, built on first use by
    // ensure_position_index() (see EDS::Metadata).
    metadata_.cum_common_positions.clear();

    auto process_token = [&](const std::string& token, bool is_bracketed) {
        if (token.empty() && !is_bracketed) return; // Ignore empty non-bracketed tokens

        size_t symbol_size = 0;
        size_t symbol_chars = 0;   // characters in this symbol, across all its strings
        StringSet sym;  // populated only when with_strings=true

        if (is_bracketed) {
            // Manual comma scan — no heap allocation per segment
            if (token.empty()) {
                // Empty set {} means one empty string ""
                metadata_.string_lengths.push_back(0);
                symbol_size = 1;
                metadata_.num_empty_strings++;
                if (with_strings) sym.push_back("");
            } else {
                size_t start = 0;
                for (size_t pos = 0; pos <= token.size(); ++pos) {
                    if (pos == token.size() || token[pos] == SET_SEPARATOR) {
                        size_t len = pos - start;
                        metadata_.string_lengths.push_back(len);
                        N_ += len;
                        symbol_chars += len;
                        if (len == 0) metadata_.num_empty_strings++;
                        if (with_strings) sym.push_back(token.substr(start, len));
                        symbol_size++;
                        start = pos + 1;
                    }
                }
            }
        } else {
            size_t len = token.length();
            metadata_.string_lengths.push_back(len);
            N_ += len;
            symbol_chars = len;
            if (len == 0) metadata_.num_empty_strings++;
            symbol_size = 1;
            if (with_strings) sym.push_back(token);
        }

        if (with_strings) sets_.push_back(std::move(sym));

        bool is_deg = (symbol_size > 1);
        metadata_.symbol_sizes.push_back(symbol_size);
        metadata_.cum_set_sizes.push_back(m_);
        metadata_.is_degenerate.push_back(is_deg);

        // Update running statistics
        if (is_deg) {
            metadata_.num_degenerate_symbols++;
            // Characters, not alternatives: total_change_size is the counterpart
            // of num_common_chars, and the two partition N. It used to add
            // (symbol_size - 1), which counts alternatives beyond the first and
            // sums to exactly m - n — a quantity cardinality() and length()
            // already give — while being printed next to a character count as
            // though the two were comparable.
            metadata_.total_change_size += symbol_chars;
        } else {
            // Non-degenerate: part of a context segment. Segment statistics
            // need the run it belongs to, so finalize_context_statistics()
            // computes them once the whole index is known.
            metadata_.num_common_chars += metadata_.string_lengths[m_]; // first (and only) string
        }

        // cum_common_positions is NOT built here — it is lazy (see EDS::Metadata).
        // ensure_position_index() materialises it from these same running counts
        // on the first position lookup.

        m_ += symbol_size;
        n_++;
    };

    // ── Bulk-buffered scan ───────────────────────────────────────────────────
    // Read the stream in 64 KB chunks and scan each chunk in memory, tracking the
    // absolute byte offset ourselves.  This avoids a virtual streambuf round-trip
    // per byte (the old is.get()/is.peek() loop), mirroring the bulk-read index
    // build documented in Sources::parse_seds().  Per-symbol semantics are
    // identical to the previous char-at-a-time implementation.
    //
    // State machine (the cursor is always in exactly one of these modes):
    //   BETWEEN    — skipping inter-symbol whitespace; no token in progress.
    //   IN_BARE    — accumulating a bare (non-bracketed) symbol in current_token.
    //   IN_BRACKET — accumulating a bracket body in bracketed_content until '}'.
    //
    // base_positions[symbol] is recorded at the symbol's first byte: the '{' for a
    // bracketed symbol, or the first character for a bare one.
    enum class Scan { BETWEEN, IN_BARE, IN_BRACKET };
    Scan mode = Scan::BETWEEN;
    std::string current_token;      // bare-token accumulator (spans chunks)
    std::string bracketed_content;  // bracket-body accumulator (spans chunks)

    // std::isspace matches ' ', '\t', '\n', '\v', '\f', '\r' under the default
    // "C" locale; inline the check to avoid a libc call per byte in the hot loop.
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' ||
               c == '\v' || c == '\f' || c == '\r';
    };

    constexpr std::streamsize CHUNK = 64 * 1024;   // 64 KB per I/O call
    std::vector<char> buf(CHUNK);
    std::streamoff file_offset = 0;   // byte offset of buf[0] in the stream

    while (is) {
        is.read(buf.data(), CHUNK);
        std::streamsize n = is.gcount();
        if (n <= 0) break;

        for (std::streamsize i = 0; i < n; /* advanced inside */) {
            const char ch = buf[i];

            if (mode == Scan::IN_BRACKET) {
                // Inside a {...} group: everything up to the matching '}' is body.
                // Bulk-scan the run of body bytes in this chunk and append it in
                // one go rather than one char at a time.
                std::streamsize j = i;
                while (j < n && buf[j] != SET_CLOSE) ++j;
                bracketed_content.append(buf.data() + i, static_cast<size_t>(j - i));
                if (j < n) {
                    // buf[j] == '}': close the symbol.
                    process_token(bracketed_content, /*is_bracketed=*/true);
                    bracketed_content.clear();
                    mode = Scan::BETWEEN;
                    i = j + 1;
                } else {
                    i = j;  // chunk exhausted mid-bracket; resume on next chunk
                }
                continue;
            }

            if (ch == SET_OPEN) {
                // Start of a bracketed symbol. Flush any bare token in progress.
                if (mode == Scan::IN_BARE) {
                    process_token(current_token, /*is_bracketed=*/false);
                    current_token.clear();
                }
                metadata_.base_positions.push_back(
                    static_cast<uint64_t>(file_offset + i));   // offset of '{'
                mode = Scan::IN_BRACKET;
                ++i;
            } else if (ch == SET_CLOSE) {
                // A '}' with no '{' open. Without this the byte falls through to
                // the bare-token branch below and is absorbed as a sequence
                // character, so a truncated or corrupt file parses "successfully"
                // with '}' sitting inside its strings — silent corruption, and
                // asymmetric with the unmatched-'{' check at the end of the scan.
                throw std::runtime_error(
                    "Unmatched '}' in EDS stream at byte " +
                    std::to_string(static_cast<long long>(file_offset + i)) + ".");
            } else if (is_ws(ch)) {
                // Inter-symbol whitespace terminates a bare token (if any).
                if (mode == Scan::IN_BARE) {
                    process_token(current_token, /*is_bracketed=*/false);
                    current_token.clear();
                    mode = Scan::BETWEEN;
                }
                // Otherwise already BETWEEN: skip the whitespace byte.
                ++i;
            } else {
                // Normal character(s) of a bare (non-bracketed) symbol.  Bulk-scan
                // the run of characters until the next '{' / whitespace / chunk end
                // and append it in one call.
                if (mode == Scan::BETWEEN) {
                    metadata_.base_positions.push_back(
                        static_cast<uint64_t>(file_offset + i));  // first char
                    mode = Scan::IN_BARE;
                }
                std::streamsize j = i;
                while (j < n && buf[j] != SET_OPEN && buf[j] != SET_CLOSE && !is_ws(buf[j])) ++j;
                current_token.append(buf.data() + i, static_cast<size_t>(j - i));
                i = j;
            }
        }

        file_offset += n;
    }

    // Drain trailing state at end of stream.
    if (mode == Scan::IN_BRACKET) {
        throw std::runtime_error("Unmatched '{' in EDS stream.");
    }
    if (mode == Scan::IN_BARE) {
        process_token(current_token, /*is_bracketed=*/false);
    }

    is_empty_ = (n_ == 0);
    finalize_context_statistics(metadata_);

    // The index arrays were grown by push_back, so each holds up to 2× the bytes
    // it needs. That slack is not transient for a METADATA_ONLY EDS: it stays
    // resident for the object's whole life, and the l-EDS merge keeps an input
    // and an output metadata alive at once. Hand the excess back now — one
    // realloc per array, paid once at load.
    metadata_.base_positions.shrink_to_fit();
    metadata_.symbol_sizes.shrink_to_fit();
    metadata_.string_lengths.shrink_to_fit();
    metadata_.cum_set_sizes.shrink_to_fit();
}

void EDS::finalize_context_statistics(Metadata& meta) {
    meta.min_context_length = 0;
    meta.max_context_length = 0;
    meta.avg_context_length = 0.0;
    meta.min_internal_context_length = 0;
    meta.num_internal_context_segments = 0;
    meta.num_context_segments = 0;
    meta.num_split_regular_symbols = 0;
    meta.num_adjacent_degenerate = 0;

    const size_t n = meta.is_degenerate.size();
    Length min_all = std::numeric_limits<Length>::max();
    Length min_internal = std::numeric_limits<Length>::max();
    size_t total = 0;
    bool seen_degenerate = false;   // a degenerate symbol lies before the current run

    size_t i = 0;
    while (i < n) {
        if (meta.is_degenerate[i]) {
            if (i > 0 && meta.is_degenerate[i - 1]) meta.num_adjacent_degenerate++;
            seen_degenerate = true;
            ++i;
            continue;
        }
        // Maximal run [i, j) of non-degenerate symbols: one segment.
        size_t len = 0;
        size_t j = i;
        while (j < n && !meta.is_degenerate[j]) {
            len += meta.string_lengths[meta.cum_set_sizes[j]];
            ++j;
        }
        const Length seg = static_cast<Length>(len);
        meta.num_context_segments++;
        meta.num_split_regular_symbols += (j - i) - 1;
        total += len;
        min_all = std::min(min_all, seg);
        meta.max_context_length = std::max(meta.max_context_length, seg);
        if (seen_degenerate && j < n) {             // degenerate on both sides
            meta.num_internal_context_segments++;
            min_internal = std::min(min_internal, seg);
        }
        i = j;
    }

    if (meta.num_context_segments > 0) {
        meta.min_context_length = min_all;
        meta.avg_context_length = static_cast<double>(total) / meta.num_context_segments;
    }
    if (meta.num_internal_context_segments > 0)
        meta.min_internal_context_length = min_internal;
}

// ================================================================================
// FACTORY METHODS
// ================================================================================

// Convenience factory for string-based construction
EDS EDS::from_string(const std::string& eds_string) {
    return EDS(eds_string);
}

// Factory: construct a METADATA_ONLY EDS from pre-built metadata + file path.
// The file must already exist and contain the EDS data described by the metadata.
EDS EDS::from_metadata(Metadata&& metadata,
                       size_t n, size_t m, size_t N,
                       const std::filesystem::path& file_path) {
    EDS eds;
    eds.metadata_  = std::move(metadata);
    eds.n_         = n;
    eds.m_         = m;
    eds.N_         = N;
    eds.is_empty_  = (n == 0);
    eds.file_path_ = file_path;
    // Open stream for on-demand symbol reads (METADATA_ONLY mode)
    eds.stream_.open(file_path);
    if (!eds.stream_) {
        throw std::runtime_error("EDS::from_metadata: cannot open file: " + file_path.string());
    }
    // sets_ left empty → METADATA_ONLY mode (inferred by stream_.is_open())
    return eds;
}

// ================================================================================
// FILE LOADERS
// ================================================================================

// Load EDS from file (uses streaming for memory efficiency)
EDS EDS::load(const std::filesystem::path& path) {
    EDS eds;
    eds.is_empty_ = false;
    eds.sources_ = nullptr;  // No sources by default
    eds.file_path_ = path;

    std::ifstream ifs(path);
    if (!ifs) {
        throw std::runtime_error("Failed to open file: " + path.string());
    }
    eds.parse(ifs);

    // Reuse the already-open stream (seek to beginning) instead of reopening
    ifs.clear();
    ifs.seekg(0);
    eds.stream_ = std::move(ifs);

    return eds;
}

// Load EDS from file with sources from file (uses streaming for memory efficiency)
EDS EDS::load(const std::filesystem::path& eds_path, const std::filesystem::path& seds_path) {
    EDS eds;
    eds.is_empty_ = false;
    eds.file_path_ = eds_path;

    // Load EDS
    std::ifstream eds_ifs(eds_path);
    if (!eds_ifs) {
        throw std::runtime_error("Failed to open EDS file: " + eds_path.string());
    }
    eds.parse(eds_ifs);

    // Load sources using Sources class — auto-detect format from file content
    auto sources = Sources::load(seds_path);

    // Validate cardinality matches
    if (!eds.is_empty_ && sources->cardinality() != eds.m_) {
        throw std::invalid_argument("Sources cardinality (" + std::to_string(sources->cardinality()) +
                                  ") does not match EDS cardinality (" + std::to_string(eds.m_) + ")");
    }
    eds.sources_ = std::move(sources);

    // Reuse the already-open stream (seek to beginning) instead of reopening
    eds_ifs.clear();
    eds_ifs.seekg(0);
    eds.stream_ = std::move(eds_ifs);

    return eds;
}

// ================================================================================
// SOURCE MANAGEMENT (Deleted - now delegated to Sources class)
// ================================================================================
// Note: load_sources(), save_sources(), parse_sources() methods have been removed.
// Sources are now managed via the Sources class.
// Use Sources::load() to create a Sources object, then set_sources_object() to attach it.

// ================================================================================
// (Deleted: parse_sources() and parse_sources_metadata_only() moved to Sources class)
// ================================================================================

// (Deleted: read_source_from_stream() - now in Sources class)

// Read source set (delegates to Sources object)
PathSet EDS::read_source(size_t string_id) const {
    if (!sources_) {
        throw std::runtime_error("No sources loaded");
    }
    return sources_->read_source(string_id);
}

// Set sources object
void EDS::set_sources_object(std::shared_ptr<Sources> sources) {
    // Validate cardinality matches if both EDS and sources are non-empty
    if (sources && !is_empty_ && sources->cardinality() != m_) {
        throw std::invalid_argument("Sources cardinality (" + std::to_string(sources->cardinality()) +
                                  ") does not match EDS cardinality (" + std::to_string(m_) + ")");
    }
    sources_ = sources;
}

// ================================================================================
// STATISTICS & METADATA
// ================================================================================

// ================================================================================
// OUTPUT METHODS
// ================================================================================

void EDS::print(std::ostream& os) const {
    // Now works with both FULL and METADATA_ONLY modes via read_symbol()
    if (is_empty_) {
        os << "(empty EDS)\n";
        return;
    }

    os << "EDS with " << n_ << " sets, " << m_ << " total strings:\n";

    StringSet scratch;
    for (size_t i = 0; i < n_; i++) {
        // Read symbol on-demand (works in both FULL and METADATA_ONLY modes);
        // symbol_view avoids copying in FULL mode.
        const StringSet& set = symbol_view(i, scratch);

        os << "Set " << i << ": {";

        for (size_t j = 0; j < set.size(); j++) {
            if (j > 0) os << ", ";

            const auto& str = set[j];
            if (str.empty()) {
                os << "ε";  // Epsilon for empty string
            } else {
                os << "\"" << str << "\"";
            }
        }

        os << "}";

        if (metadata_.is_degenerate[i]) {
            os << " [degenerate]";
        }

        os << "\n";
    }
}

void EDS::save(std::ostream& os, OutputFormat format) const {
    // Now works with both FULL and METADATA_ONLY modes via read_symbol()
    // Output EDS format
    StringSet scratch;
    for (size_t i = 0; i < n_; i++) {
        // Read symbol on-demand (works in both FULL and METADATA_ONLY modes);
        // symbol_view avoids copying in FULL mode.
        const StringSet& set = symbol_view(i, scratch);

        // Determine if we should use brackets for this set
        // An empty regular symbol has no bare spelling — without brackets it
        // would vanish on reparse — so it keeps "{}" even in compact output.
        bool use_brackets = (format == OutputFormat::FULL) || metadata_.is_degenerate[i] ||
                            (set.size() == 1 && set[0].empty());

        if (use_brackets) {
            os << "{";
        }

        bool first = true;
        for (const auto& str : set) {
            if (!first) os << ",";
            os << str;
            first = false;
        }

        if (use_brackets) {
            os << "}";
        }
    }
    os << "\n";
}

void EDS::save(const std::filesystem::path& path, OutputFormat format) const {
    std::ofstream ofs(path);
    if (!ofs) {
        throw std::runtime_error("Failed to open file for writing: " + path.string());
    }
    save(ofs, format);
}

// (Deleted: save_sources() methods - now in Sources class via sources_->save())

// ================================================================================
// PATTERN GENERATION & EXTRACTION
// ================================================================================

namespace {

// A PathSet is complement-encoded when it starts with 0: {0} means every path,
// {0,e1,e2} means every path except e1 and e2. Anything else is an explicit
// member list. Both spellings are produced by vcf2eds and msa2eds (the reference
// allele is written as a complement), so membership must handle them.
bool path_in_set(const PathSet& s, int path) {
    if (s.empty()) return false;
    if (s.front() == 0) {
        return std::find(s.begin() + 1, s.end(), path) == s.end();
    }
    return std::find(s.begin(), s.end(), path) != s.end();
}

constexpr size_t NO_ALTERNATIVE = static_cast<size_t>(-1);

}  // namespace

EDS::PatternGenStats EDS::generate_patterns(std::ostream& os, size_t count,
                                            Length pattern_length,
                                            std::optional<uint64_t> seed,
                                            bool source_aware,
                                            bool unique) const {
    if (is_empty_ || n_ == 0) {
        throw std::runtime_error("Cannot generate patterns from empty EDS");
    }

    if (pattern_length == 0) {
        throw std::invalid_argument("Pattern length must be greater than 0");
    }

    PatternGenStats stats;
    stats.requested = count;
    stats.unique = unique;

    uint64_t effective_seed;
    if (seed.has_value()) {
        effective_seed = *seed;
    } else {
        std::random_device rd;
        effective_seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    }
    std::mt19937_64 gen(effective_seed);

    // Walking a path needs both a Sources object and a known path universe.
    const bool walk_path =
        source_aware && has_sources() && sources_ && sources_->num_paths() > 0;
    const size_t num_paths = walk_path ? sources_->num_paths() : 0;
    stats.source_aware = walk_path;
    stats.num_paths = num_paths;

    std::uniform_int_distribution<Position> pos_dist(
        0,
        metadata_.num_common_chars > 0 ? metadata_.num_common_chars - 1 : 0
    );

    // Pick which alternative of `symbol_pos` to follow. In path mode only
    // alternatives carrying `path` are eligible; a non-degenerate symbol has a
    // single alternative that every path shares, so it skips the source lookup.
    auto choose_alternative = [&](const StringSet& set, Position symbol_pos,
                                  int path) -> size_t {
        if (path == 0 || set.size() == 1) {
            std::uniform_int_distribution<size_t> d(0, set.size() - 1);
            return set.size() == 1 ? 0 : d(gen);
        }

        const size_t base = metadata_.cum_set_sizes[symbol_pos];
        std::vector<size_t> candidates;
        candidates.reserve(set.size());
        for (size_t k = 0; k < set.size(); ++k) {
            if (path_in_set(read_source(base + k), path)) candidates.push_back(k);
        }
        if (candidates.empty()) return NO_ALTERNATIVE;

        std::uniform_int_distribution<size_t> d(0, candidates.size() - 1);
        return candidates[candidates.size() == 1 ? 0 : d(gen)];
    };

    // One attempt from a given start. Returns an empty optional if the walk ran
    // off the end of the EDS or hit a symbol this path has no alternative at.
    auto try_build = [&](Position start_symbol, Length offset_in_symbol,
                         int path) -> std::optional<String> {
        String pattern;
        pattern.reserve(pattern_length);
        Length remaining = pattern_length;
        Position current_pos = start_symbol;
        bool first_symbol = true;

        while (remaining > 0 && current_pos < n_) {
            StringSet set = read_symbol(current_pos);

            if (set.empty()) {  // epsilon symbol — nothing to contribute
                current_pos++;
                first_symbol = false;
                continue;
            }

            size_t idx = choose_alternative(set, current_pos, path);
            if (idx == NO_ALTERNATIVE) return std::nullopt;

            const String& selected = set[idx];
            Length start_offset = first_symbol ? offset_in_symbol : 0;

            // An empty alternative (a deletion on this path) contributes nothing
            // but is a legitimate step, not a failure.
            if (start_offset < selected.length()) {
                Length available = selected.length() - start_offset;
                Length to_take = std::min(remaining, available);
                pattern.append(selected.substr(start_offset, to_take));
                remaining -= to_take;
            }

            first_symbol = false;
            current_pos++;
        }

        if (remaining > 0) return std::nullopt;  // ran out of EDS
        return pattern;
    };

    // Retry from a fresh path and start rather than padding a short walk by
    // wrapping around to symbol 0, which would splice together sequence that is
    // not contiguous in any genome.  The same retry loop absorbs deduplication:
    // a repeat is discarded exactly like a walk that ran off the end.
    constexpr int MAX_ATTEMPTS = 64;

    // Only populated when deduplicating, so the allow-duplicates path costs
    // nothing. Patterns are fixed-length, so this is count x pattern_length
    // bytes at worst.
    std::unordered_set<String> seen;
    if (unique) seen.reserve(count * 2);

    for (size_t i = 0; i < count; ++i) {
        std::optional<String> pattern;

        for (int attempt = 0; attempt < MAX_ATTEMPTS && !pattern; ++attempt) {
            int path = 0;
            if (walk_path) {
                std::uniform_int_distribution<int> path_dist(1, static_cast<int>(num_paths));
                path = path_dist(gen);
            }

            Position random_common_pos =
                metadata_.num_common_chars > 0 ? pos_dist(gen) : 0;
            Position offset_in_symbol = 0;
            size_t start_symbol = 0;
            if (metadata_.num_common_chars > 0) {
                start_symbol = find_symbol_at_common_position(random_common_pos, offset_in_symbol);
            }

            pattern = try_build(start_symbol, static_cast<Length>(offset_in_symbol), path);

            if (pattern && unique && !seen.insert(*pattern).second) {
                stats.duplicates_discarded++;
                pattern.reset();   // try again from a fresh path and start
            }
        }

        if (pattern) {
            os << *pattern << '\n';
            stats.generated++;
        }
    }

    return stats;
}

String EDS::extract(Position pos, Length len, const std::vector<int>& changes) const {
    // Now works with both FULL and METADATA_ONLY modes via read_symbol()
    if (is_empty_ || n_ == 0) {
        throw std::runtime_error("Cannot extract from empty EDS");
    }

    if (pos >= n_) {
        throw std::out_of_range("Start position exceeds EDS length");
    }

    if (len == 0) {
        return "";
    }

    // Validate changes vector size
    Position end_pos = std::min(pos + len, n_);
    size_t expected_changes = end_pos - pos;

    if (changes.size() != expected_changes) {
        throw std::invalid_argument(
            "changes vector size (" + std::to_string(changes.size()) +
            ") must match range length (" + std::to_string(expected_changes) + ")"
        );
    }

    // Extract substring by selecting alternatives according to changes vector
    String result;
    for (size_t i = 0; i < expected_changes; ++i) {
        Position current_pos = pos + i;
        int change_idx = changes[i];

        // Read symbol on-demand (works in both FULL and METADATA_ONLY modes)
        StringSet set = read_symbol(current_pos);

        // Validate change index
        if (change_idx < 0 || static_cast<size_t>(change_idx) >= set.size()) {
            throw std::out_of_range(
                "Change index " + std::to_string(change_idx) +
                " at position " + std::to_string(current_pos) +
                " is out of range (set size: " + std::to_string(set.size()) + ")"
            );
        }

        // Append the selected string
        result.append(set[change_idx]);
    }

    return result;
}

// ================================================================================
// STREAMING & DATA ACCESS
// ================================================================================

std::string EDS::normalize_eds_format(const std::string& input) const {
    /*
     * Normalize compact EDS format to full bracketed format
     * Examples:
     *   "ACGT{A,ACA}CGT" -> "{ACGT}{A,ACA}{CGT}"
     *   "{ACGT}{A,ACA}{CGT}" -> "{ACGT}{A,ACA}{CGT}" (no change)
     *   "A{C,G}T" -> "{A}{C,G}{T}"
     */

    std::string result;
    std::string current_string;
    size_t i = 0;
    int brace_depth = 0;

    while (i < input.length()) {
        char ch = input[i];

        if (ch == SET_OPEN) {
            // If we have accumulated non-bracketed characters, wrap them
            if (!current_string.empty() && brace_depth == 0) {
                result += "{" + current_string + "}";
                current_string.clear();
            }
            result += ch;
            brace_depth++;
            i++;
        }
        else if (ch == SET_CLOSE) {
            result += ch;
            brace_depth--;
            i++;
        }
        else if (brace_depth > 0) {
            // Inside brackets, pass through as-is
            result += ch;
            i++;
        }
        else {
            // Outside brackets, accumulate characters
            current_string += ch;
            i++;
        }
    }

    // If there are remaining non-bracketed characters at the end, wrap them
    if (!current_string.empty() && brace_depth == 0) {
        result += "{" + current_string + "}";
    }

    return result;
}

// Read symbol from stream (on-demand reading)
// ─────────────────────────────────────────────────────────────────────────────
// read_symbol_from_stream — on-demand EDS symbol deserialisation
// ─────────────────────────────────────────────────────────────────────────────
//
// CONTEXT — what is a "symbol" and why does it live on disk?
//
//   An EDS is a sequence of *symbols*.  Each symbol is either:
//     • a non-degenerate context block: a single DNA string, e.g. "ACGTCG"
//     • a degenerate set: multiple alternative strings, e.g. "{ACC,A,TTTGC}"
//
//   In FULL storage mode every symbol is pre-loaded into the in-memory
//   `sets_` vector and this function is never called.  In METADATA_ONLY mode
//   only the *index* (base_positions, string_lengths, …) is kept in RAM; the
//   actual character data remains on disk and is fetched here on demand.
//   METADATA_ONLY is mandatory for files that don't fit in RAM (100 GB+ EDS).
//
// THE INDEX — how we know where to look
//
//   `metadata_.base_positions` is a vector of uint64_t byte offsets, one per
//   symbol.  base_positions[pos] is the byte offset of the first character of
//   symbol pos in the file (the opening '{' in full-bracket format, or the
//   first DNA character in compact format).  This index is built once during
//   load() by scanning the file, and never changes afterwards.
//
// THE KEY OPTIMISATION — skip seekg() when the stream is already there
//
//   Every call to seekg() has two expensive side-effects:
//     1. An lseek(2) syscall — a kernel round-trip even for tiny movements.
//     2. Buffer invalidation — the C++ stream library maintains an 8-KB read
//        buffer; seekg() declares it stale, so the very next get() or read()
//        must fill it again with a new read(2) syscall.
//   Together that is at minimum two syscalls per symbol read, regardless of
//   whether the disk head has to move at all.
//
//   In stream_merged_symbols_to_file() the dominant caller, symbols are
//   processed in strictly increasing order (pos = 0, 1, 2, …).  After reading
//   symbol pos the stream lands at base_positions[pos+1] — exactly the target
//   for the next call.  The check below detects this situation and skips the
//   seekg() entirely, keeping the buffer hot and avoiding the syscall pair.
//
//   Measured effect (1 MB EDS, 10% variability, 2 iterations):
//     • Without this guard: ~400 000 lseek + 400 000 read syscalls per run.
//     • With this guard:    ~5 000 lseek + 5 000 read syscalls per run.
//   (The ~5 000 remaining seeks cover the ~2 727 merge pairs where the second
//   symbol must be read immediately after the first without the loop advancing,
//   and one seek per merged pair's next neighbour after the skip gap.)
//
// WHEN DOES stream_.good() FAIL?
//
//   std::ifstream sets failbit or badbit if a previous read hit the end of the
//   file or encountered an I/O error.  EOF does not self-heal; the stream stays
//   in a failed state until clear() is called.  We therefore check good() first:
//   if the stream is unhealthy we must clear() and seek regardless of where the
//   file pointer happens to sit.  This prevents spurious "stream not good after
//   seek" errors on the first symbol read following an EOF-terminated preceding
//   symbol.
//
// FORMAT SUPPORT
//
//   Two on-disk layouts are recognised:
//
//   Full-bracket format  (used for all intermediate temp files):
//     {str1,str2,...,strK}
//     Even a non-degenerate symbol with a single string is wrapped: {ACGT}.
//     This format is required for METADATA_ONLY because every symbol starts
//     with '{', making the index simple and the per-symbol byte boundaries easy
//     to identify (see parse() in this file).
//
//   Compact format  (used for final user-facing output and as input):
//     {str1,str2,...}   for multi-alternative (degenerate) symbols   ← same
//     ACGT              for single-alternative (non-degenerate) symbols
//     Non-degenerate symbols are written without brackets; their end is
//     delimited by the '{' that opens the next degenerate symbol, by
//     whitespace, or by EOF.
//
// ─────────────────────────────────────────────────────────────────────────────
// Size (in bytes) of the backing file, derived from the open stream rather than
// the path.  Temp files are frequently unlinked while the stream stays open
// (valid on POSIX), so std::filesystem::file_size(file_path_) would throw; the
// stream's own end position is always available.  Cached after first use; leaves
// the get pointer at end-of-file (callers re-seek as needed).
std::streamoff EDS::stream_file_size() const {
    if (file_size_ < 0) {
        stream_.clear();
        stream_.seekg(0, std::ios::end);
        file_size_ = static_cast<std::streamoff>(stream_.tellg());
    }
    return file_size_;
}

StringSet EDS::read_symbol_from_stream(Position pos) const {
    if (!stream_.is_open()) {
        throw std::runtime_error("File stream not available for reading symbol");
    }

    // ── Position the stream ──────────────────────────────────────────────────
    // base_positions[pos] is the byte offset of the first character of this
    // symbol (either '{' or a DNA letter for compact non-degenerate symbols).
    // We only call seekg() when the stream is not already sitting there.
    // See the long comment above for the rationale: seekg() costs two syscalls
    // (lseek + buffer-refill read) and in the common sequential-access pattern
    // the stream lands exactly at base_positions[pos+1] after reading pos, so
    // the guard fires and we skip both syscalls.
    const auto target = static_cast<std::streamoff>(metadata_.base_positions[pos]);
    if (!stream_.good() || stream_.tellg() != std::streampos(target)) {
        stream_.clear();   // Heal any prior EOF / error state before seeking.
        stream_.seekg(target);
        if (!stream_) {
            throw std::runtime_error("Failed to seek to position " + std::to_string(pos));
        }
    }

    // ── Bulk-read the symbol's byte span ─────────────────────────────────────
    // The metadata already knows the exact byte layout, so instead of pulling
    // the symbol one get()/peek() at a time (the compact path evaluated
    // stream_.peek() three times per byte in its loop condition), read the whole
    // span in a single stream_.read() and scan it in memory.  The span is
    // [base_positions[pos], base_positions[pos+1]) for every symbol but the last,
    // and [base_positions[pos], file_size) for the final symbol.  Any trailing
    // inter-symbol whitespace captured inside the span is ignored by the scan.
    std::streamoff span_end;
    if (pos + 1 < n_) {
        span_end = static_cast<std::streamoff>(metadata_.base_positions[pos + 1]);
    } else {
        span_end = stream_file_size();   // last symbol: span runs to EOF
        // Re-seek: querying the size moved the get pointer to end-of-file.
        stream_.clear();
        stream_.seekg(target);
    }
    std::streamoff span = span_end - target;
    if (span <= 0) {
        throw std::runtime_error("Unexpected EOF reading symbol at position " + std::to_string(pos));
    }

    std::string buf(static_cast<size_t>(span), '\0');
    stream_.read(&buf[0], span);
    buf.resize(static_cast<size_t>(stream_.gcount()));
    if (buf.empty()) {
        throw std::runtime_error("Unexpected EOF reading symbol at position " + std::to_string(pos));
    }

    // is_ws: inline whitespace test (matches std::isspace under the "C" locale)
    // without a libc call per byte.
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' ||
               c == '\v' || c == '\f' || c == '\r';
    };

    // ── Parse the buffer into a StringSet ────────────────────────────────────
    // emit_token appends buf[s, e) as one alternative.  The fast path (no
    // interior whitespace, the only case well-formed EDS files produce) is a
    // single bulk substring construction; the slow path strips whitespace
    // defensively to preserve the old char-scan's behaviour.
    StringSet result;
    auto emit_token = [&](size_t s, size_t e) {
        size_t k = s;
        while (k < e && !is_ws(buf[k])) ++k;
        if (k == e) {
            result.emplace_back(buf.data() + s, e - s);
            return;
        }
        std::string t;
        t.reserve(e - s);
        for (size_t j = s; j < e; ++j)
            if (!is_ws(buf[j])) t += buf[j];
        result.push_back(std::move(t));
    };

    if (buf[0] == SET_OPEN) {
        // ── Full-bracket path: {str1,str2,...,strK} ──────────────────────────
        // Split the body on ',' up to the matching '}'.  A "{}" symbol yields a
        // single empty string (epsilon / deletion), a valid EDS construct.
        size_t tok_start = 1;
        bool closed = false;
        for (size_t i = 1; i < buf.size(); ++i) {
            const char c = buf[i];
            if (c == SET_SEPARATOR) {
                emit_token(tok_start, i);
                tok_start = i + 1;
            } else if (c == SET_CLOSE) {
                emit_token(tok_start, i);
                closed = true;
                break;
            }
        }
        // Truncated symbol (EOF before '}'): emit the trailing token, matching
        // the old loop which pushed whatever it had accumulated.
        if (!closed) emit_token(tok_start, buf.size());
    } else {
        // ── Compact path: bare DNA string until the next symbol/whitespace ────
        size_t i = 0;
        while (i < buf.size() && buf[i] != SET_OPEN && !is_ws(buf[i])) ++i;
        result.emplace_back(buf.data(), i);
    }

    return result;
}

// Public accessor for read_symbol (works in both modes)
StringSet EDS::read_symbol(Position pos) const {
    if (pos >= n_) {
        throw std::out_of_range("Position " + std::to_string(pos) + " out of range");
    }
    // In-memory mode: EDS was constructed from a stream/string, sets_ is populated
    if (!sets_.empty()) return sets_[pos];
    // File-backed mode: EDS was loaded from a file via EDS::load()
    return read_symbol_from_stream(pos);
}

// Return symbol `pos` without copying where possible: in-memory EDS references
// sets_ directly, file-backed EDS reads into the caller's scratch buffer and
// references that (one move, no extra copy).
const StringSet& EDS::symbol_view(Position pos, StringSet& scratch) const {
    if (!sets_.empty()) return sets_[pos];
    scratch = read_symbol_from_stream(pos);
    return scratch;
}

// ── Raw byte-copy of a run of symbols ────────────────────────────────────────
// Mirrors Sources::copy_range_to_stream for the EDS side.  When a run of
// symbols is stored in full-bracket format with no inter-symbol padding, their
// output bytes are byte-identical to their input bytes, so we can copy the whole
// contiguous span [base_positions[start], base_positions[start+count]) directly
// — no read_symbol()/StringSet allocation and no char-by-char re-serialisation.
// The caller (stream_merged_symbols_to_file BRANCH B) verifies the
// full-bracket-contiguity precondition per symbol before batching, so this
// method assumes it holds for the whole range.  Copies in 64 KB chunks so a
// large unmodified run never pulls the entire span into RAM.
void EDS::copy_symbol_range_to_stream(Position start, size_t count, std::ostream& out) const {
    if (count == 0) return;
    if (!stream_.is_open())
        throw std::runtime_error("copy_symbol_range_to_stream: file stream not available");
    if (start + count > n_)
        throw std::out_of_range("copy_symbol_range_to_stream: range out of bounds");

    auto a = static_cast<std::streamoff>(metadata_.base_positions[start]);
    std::streamoff b;
    if (start + count < n_) {
        b = static_cast<std::streamoff>(metadata_.base_positions[start + count]);
    } else {
        b = stream_file_size();   // range reaches EOF
    }
    std::streamoff byte_count = b - a;
    if (byte_count <= 0) return;

    // Seek guard: sequential calls land the stream exactly at `a`, so the seek
    // (and its buffer refill) is skipped in the common in-order pattern.
    auto target = static_cast<std::streampos>(a);
    if (!stream_.good() || stream_.tellg() != target) {
        stream_.clear();
        stream_.seekg(target);
    }

    constexpr std::streamsize CHUNK = 64 * 1024;
    char chunk[CHUNK];
    std::streamoff remaining = byte_count;
    while (remaining > 0 && stream_.good()) {
        std::streamsize to_read = static_cast<std::streamsize>(
            std::min(remaining, static_cast<std::streamoff>(CHUNK)));
        stream_.read(chunk, to_read);
        std::streamsize got = stream_.gcount();
        if (got <= 0) break;
        out.write(chunk, got);
        remaining -= got;
    }
}

// ================================================================================
// COMMON-POSITION LOOKUP
// ================================================================================

// Materialise the lazy common-position prefix sums.  cum_common_positions is a
// pure function of string_lengths / is_degenerate, so parsing skips it (8 bytes
// per symbol) and only callers that actually look up positions pay for it — the
// l-EDS merge never does.
void EDS::ensure_position_index() const {
    if (metadata_.cum_common_positions.size() == n_ + 1) return;  // already built

    metadata_.cum_common_positions.assign(1, 0);
    metadata_.cum_common_positions.reserve(n_ + 1);

    Position cumulative_common = 0;
    for (size_t i = 0; i < n_; ++i) {
        if (!metadata_.is_degenerate[i]) {
            cumulative_common += metadata_.string_lengths[metadata_.cum_set_sizes[i]];
        }
        metadata_.cum_common_positions.push_back(cumulative_common);
    }
}

// Find the symbol containing a given common position (used by generate_patterns)
size_t EDS::find_symbol_at_common_position(Position common_pos, Position& offset_out) const {
    ensure_position_index();

    // cum_common_positions has n+1 entries, so a position at or past the total
    // common-character count sends upper_bound to end() and makes symbol_idx ==
    // n below — one past the end of is_degenerate, cum_set_sizes and every other
    // per-symbol array. That read produced a garbage string index and a segfault
    // roughly two runs in three, so the range is rejected here instead.
    const Position total_common = metadata_.cum_common_positions.empty()
        ? 0 : metadata_.cum_common_positions.back();
    if (common_pos >= total_common) {
        throw std::out_of_range(
            "Common position " + std::to_string(common_pos) +
            " is past the end: EDS has " + std::to_string(total_common) +
            " common characters"
        );
    }

    // Binary search in cum_common_positions
    auto it = std::upper_bound(
        metadata_.cum_common_positions.begin(),
        metadata_.cum_common_positions.end(),
        common_pos
    );

    if (it == metadata_.cum_common_positions.begin()) {
        throw std::out_of_range(
            "Common position " + std::to_string(common_pos) + " is before EDS start"
        );
    }

    size_t symbol_idx = std::distance(metadata_.cum_common_positions.begin(), it) - 1;

    // This symbol must be non-degenerate (common)
    if (metadata_.is_degenerate[symbol_idx]) {
        throw std::out_of_range(
            "Common position " + std::to_string(common_pos) +
            " points to degenerate symbol " + std::to_string(symbol_idx)
        );
    }

    // Calculate offset within the symbol
    offset_out = common_pos - metadata_.cum_common_positions[symbol_idx];

    // Validate offset is within the symbol's length
    size_t global_string_idx = metadata_.cum_set_sizes[symbol_idx];
    Length symbol_length = metadata_.string_lengths[global_string_idx];

    if (offset_out >= symbol_length) {
        throw std::out_of_range(
            "Offset " + std::to_string(offset_out) +
            " exceeds symbol " + std::to_string(symbol_idx) +
            " length " + std::to_string(symbol_length)
        );
    }

    return symbol_idx;
}

} // namespace edsparser
