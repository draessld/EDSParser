#include "eds.hpp"
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <random>

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
    metadata_.min_context_length = UINT32_MAX;
    metadata_.max_context_length = 0;
    size_t total_context_length = 0;
    size_t num_context_blocks = 0;

    // Cumulative arrays built incrementally (push one entry per symbol)
    metadata_.cum_common_positions.clear();
    metadata_.cum_degenerate_counts.clear();
    metadata_.cum_common_positions.push_back(0);  // cum_common_positions[0] = 0
    metadata_.cum_degenerate_counts.push_back(0); // cum_degenerate_counts[0] = 0
    Position cumulative_common = 0;
    int cumulative_degenerate = 0;

    auto process_token = [&](const std::string& token, bool is_bracketed) {
        if (token.empty() && !is_bracketed) return; // Ignore empty non-bracketed tokens

        size_t symbol_size = 0;
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
            metadata_.total_change_size += (symbol_size - 1);
            cumulative_degenerate += static_cast<int>(symbol_size);
        } else {
            // Non-degenerate: this is a context block
            Length ctx_len = metadata_.string_lengths[m_]; // first (and only) string
            metadata_.num_common_chars += ctx_len;
            if (ctx_len < metadata_.min_context_length) metadata_.min_context_length = ctx_len;
            if (ctx_len > metadata_.max_context_length) metadata_.max_context_length = ctx_len;
            total_context_length += ctx_len;
            num_context_blocks++;
            cumulative_common += ctx_len;
        }

        // Cumulative arrays: push value after this symbol
        metadata_.cum_common_positions.push_back(cumulative_common);
        metadata_.cum_degenerate_counts.push_back(cumulative_degenerate);

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

        for (std::streamsize i = 0; i < n; ++i) {
            const char ch = buf[i];

            if (mode == Scan::IN_BRACKET) {
                // Inside a {...} group: everything up to the matching '}' is body.
                if (ch == SET_CLOSE) {
                    process_token(bracketed_content, /*is_bracketed=*/true);
                    bracketed_content.clear();
                    mode = Scan::BETWEEN;
                } else {
                    bracketed_content += ch;
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
                    static_cast<std::streampos>(file_offset + i));   // offset of '{'
                mode = Scan::IN_BRACKET;
            } else if (is_ws(ch)) {
                // Inter-symbol whitespace terminates a bare token (if any).
                if (mode == Scan::IN_BARE) {
                    process_token(current_token, /*is_bracketed=*/false);
                    current_token.clear();
                    mode = Scan::BETWEEN;
                }
                // Otherwise already BETWEEN: skip the whitespace byte.
            } else {
                // Normal character of a bare (non-bracketed) symbol.
                if (mode == Scan::BETWEEN) {
                    metadata_.base_positions.push_back(
                        static_cast<std::streampos>(file_offset + i));  // first char
                    mode = Scan::IN_BARE;
                }
                current_token += ch;
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

    if (n_ == 0) {
        is_empty_ = true;
        metadata_.min_context_length = 0;
        metadata_.max_context_length = 0;
        metadata_.avg_context_length = 0.0;
    } else {
        is_empty_ = false;
        // Finalize statistics
        if (metadata_.min_context_length == UINT32_MAX) {
            metadata_.min_context_length = 0;
        }
        metadata_.avg_context_length = (num_context_blocks > 0)
            ? static_cast<double>(total_context_length) / num_context_blocks
            : 0.0;
    }
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

    // Load sources using Sources class (always streaming)
    auto sources = Sources::load(seds_path, Sources::Format::SEDS);

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

void EDS::calculate_statistics() {
    if (is_empty_) {
        metadata_.min_context_length = 0;
        metadata_.max_context_length = 0;
        metadata_.avg_context_length = 0.0;
        metadata_.num_degenerate_symbols = 0;
        metadata_.num_common_chars = 0;
        metadata_.total_change_size = 0;
        metadata_.num_empty_strings = 0;
        metadata_.cum_common_positions.clear();
        metadata_.cum_degenerate_counts.clear();
        return;
    }

    // Initialize statistics in metadata
    metadata_.min_context_length = UINT32_MAX;
    metadata_.max_context_length = 0;
    metadata_.num_degenerate_symbols = 0;
    metadata_.num_common_chars = 0;
    metadata_.total_change_size = 0;
    metadata_.num_empty_strings = 0;

    size_t total_context_length = 0;
    size_t num_context_blocks = 0;
    size_t string_idx = 0;

    // Iterate through each symbol using metadata only
    for (size_t i = 0; i < n_; i++) {
        size_t symbol_size = metadata_.symbol_sizes[i];
        bool is_degenerate = metadata_.is_degenerate[i];

        // Count degenerate symbols
        if (is_degenerate) {
            metadata_.num_degenerate_symbols++;
            metadata_.total_change_size += (symbol_size - 1);
        } else {
            // Non-degenerate symbols are "context blocks"
            // These are the common parts between degenerate positions
            Length context_len = metadata_.string_lengths[string_idx];

            if (context_len < metadata_.min_context_length) {
                metadata_.min_context_length = context_len;
            }
            if (context_len > metadata_.max_context_length) {
                metadata_.max_context_length = context_len;
            }
            total_context_length += context_len;
            num_context_blocks++;

            metadata_.num_common_chars += context_len;
        }

        // Count empty strings and process all strings in this symbol
        for (size_t j = 0; j < symbol_size; j++) {
            Length str_len = metadata_.string_lengths[string_idx];
            if (str_len == 0) {
                metadata_.num_empty_strings++;
            }
            string_idx++;
        }
    }

    // Calculate average context length
    if (num_context_blocks > 0) {
        metadata_.avg_context_length = static_cast<double>(total_context_length) / num_context_blocks;
    } else {
        metadata_.avg_context_length = 0.0;
    }

    // Handle edge case where all symbols are degenerate (no context blocks)
    if (metadata_.min_context_length == UINT32_MAX) {
        metadata_.min_context_length = 0;
    }

    // Calculate cumulative common positions (for position checking)
    metadata_.cum_common_positions.clear();
    metadata_.cum_common_positions.reserve(n_ + 1);

    Position cumulative_common = 0;
    metadata_.cum_common_positions.push_back(0);

    string_idx = 0;
    for (size_t i = 0; i < n_; i++) {
        if (!metadata_.is_degenerate[i]) {
            // Non-degenerate: add its length to cumulative
            cumulative_common += metadata_.string_lengths[string_idx];
        }
        metadata_.cum_common_positions.push_back(cumulative_common);

        // Move string_idx forward by number of strings in this symbol
        string_idx += metadata_.symbol_sizes[i];
    }

    // Calculate cumulative degenerate counts (for position checking)
    metadata_.cum_degenerate_counts.clear();
    metadata_.cum_degenerate_counts.reserve(n_ + 1);

    int cumulative_degenerate = 0;
    metadata_.cum_degenerate_counts.push_back(0);

    for (size_t i = 0; i < n_; i++) {
        if (metadata_.is_degenerate[i]) {
            cumulative_degenerate += metadata_.symbol_sizes[i];
        }
        metadata_.cum_degenerate_counts.push_back(cumulative_degenerate);
    }
}

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

    for (size_t i = 0; i < n_; i++) {
        // Read symbol on-demand (works in both FULL and METADATA_ONLY modes)
        StringSet set = read_symbol(i);

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
    for (size_t i = 0; i < n_; i++) {
        // Read symbol on-demand (works in both FULL and METADATA_ONLY modes)
        StringSet set = read_symbol(i);

        // Determine if we should use brackets for this set
        bool use_brackets = (format == OutputFormat::FULL) || metadata_.is_degenerate[i];

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

void EDS::generate_patterns(std::ostream& os, size_t count, Length pattern_length) const {
    if (is_empty_ || n_ == 0) {
        throw std::runtime_error("Cannot generate patterns from empty EDS");
    }

    if (pattern_length == 0) {
        throw std::invalid_argument("Pattern length must be greater than 0");
    }

    // Use random number generator for reproducible results
    std::random_device rd;
    std::mt19937 gen(rd());

    // Random position distribution (0 to num_common_chars - 1)
    std::uniform_int_distribution<Position> pos_dist(
        0,
        metadata_.num_common_chars > 0 ? metadata_.num_common_chars - 1 : 0
    );

    for (size_t i = 0; i < count; ++i) {
        String pattern;
        Length remaining_length = pattern_length;

        // Pick random starting position in the EDS
        Position random_common_pos = metadata_.num_common_chars > 0 ? pos_dist(gen) : 0;
        Position offset_in_symbol = 0;
        size_t start_symbol = 0;

        if (metadata_.num_common_chars > 0) {
            start_symbol = find_symbol_at_common_position(random_common_pos, offset_in_symbol);
        }

        Position current_pos = start_symbol;
        bool first_symbol = true;

        // Generate pattern by randomly selecting from sets
        // Works in both FULL and METADATA_ONLY modes via read_symbol()
        while (remaining_length > 0 && current_pos < n_) {
            StringSet set = read_symbol(current_pos);

            if (set.empty()) {
                // Skip empty sets (epsilon)
                current_pos++;
                first_symbol = false;
                continue;
            }

            // Randomly select one string from the set
            std::uniform_int_distribution<size_t> set_dist(0, set.size() - 1);
            size_t string_idx = set_dist(gen);
            const String& selected = set[string_idx];

            // For first symbol, start from offset; for others, start from 0
            Length start_offset = first_symbol ? offset_in_symbol : 0;

            // Take what we need from this string (starting from offset)
            if (start_offset < selected.length()) {
                Length available = selected.length() - start_offset;
                Length to_take = std::min(remaining_length, available);
                pattern.append(selected.substr(start_offset, to_take));
                remaining_length -= to_take;
            }

            first_symbol = false;

            if (remaining_length > 0) {
                current_pos++;
            } else {
                break;
            }
        }

        // If we couldn't generate full pattern length, pad or regenerate
        if (pattern.length() < pattern_length) {
            // Try wrapping around for short EDS
            while (pattern.length() < pattern_length && n_ > 0) {
                Position wrap_pos = pattern.length() % n_;
                StringSet set = read_symbol(wrap_pos);

                if (!set.empty()) {
                    std::uniform_int_distribution<size_t> set_dist(0, set.size() - 1);
                    size_t string_idx = set_dist(gen);
                    const String& selected = set[string_idx];

                    Length to_take = std::min(
                        static_cast<Length>(pattern_length - pattern.length()),
                        static_cast<Length>(selected.length())
                    );
                    pattern.append(selected.substr(0, to_take));
                }
            }
        }

        // Output the pattern
        os << pattern << '\n';
    }
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
//   `metadata_.base_positions` is a vector of std::streampos values, one per
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
    auto target = metadata_.base_positions[pos];
    if (!stream_.good() || stream_.tellg() != target) {
        stream_.clear();   // Heal any prior EOF / error state before seeking.
        stream_.seekg(target);
        if (!stream_) {
            throw std::runtime_error("Failed to seek to position " + std::to_string(pos));
        }
    }

    // ── Parse the symbol ─────────────────────────────────────────────────────
    // We build a StringSet (a std::vector<std::string>) in-place.
    // current_str accumulates characters belonging to one alternative string
    // until we hit a separator ',' or the closing '}'.
    StringSet result;
    char ch;
    std::string current_str;

    // Read the very first character to decide which format we are in.
    if (!stream_.get(ch)) {
        throw std::runtime_error("Unexpected EOF reading symbol at position " + std::to_string(pos));
    }

    if (ch == SET_OPEN) {
        // ── Full-bracket path: {str1,str2,...,strK} ──────────────────────────
        // Consume characters until the matching '}'.  A ',' signals the end of
        // one alternative and the start of the next.  Whitespace is ignored
        // (defensive; well-formed EDS files contain none inside symbols).
        // Note: this handles nested brackets correctly because inner brackets
        // would be DNA characters — the EDS format does not use nesting — but
        // the simple character scan is sufficient for the flat structure.
        while (stream_.get(ch) && ch != SET_CLOSE) {
            if (ch == SET_SEPARATOR) {
                // Finished one alternative; save it and reset the accumulator.
                result.push_back(current_str);
                current_str.clear();
            } else if (!std::isspace(static_cast<unsigned char>(ch))) {
                // Normal DNA character; append to the current alternative.
                current_str += ch;
            }
            // Whitespace inside a bracket group is silently dropped.
        }
        // The last alternative does not end with ','; push it now.
        // If the symbol was "{}" (empty set with one empty string), this pushes
        // an empty string, which is a valid EDS construct (epsilon / deletion).
        result.push_back(current_str);

    } else {
        // ── Compact path: bare DNA string until delimiter ─────────────────────
        // The first character (ch) was already consumed above; seed the
        // accumulator with it.  Keep reading until we hit the opening brace of
        // the next degenerate symbol, a whitespace character (newline between
        // symbols in pretty-printed files), or EOF.
        // We use peek() so the delimiter is *not* consumed; it belongs either
        // to the next symbol or to whitespace the caller should not care about.
        current_str += ch;
        while (stream_.peek() != SET_OPEN && stream_.peek() != EOF &&
               !std::isspace(static_cast<unsigned char>(stream_.peek()))) {
            stream_.get(ch);
            current_str += ch;
        }
        // A compact non-degenerate symbol always has exactly one alternative.
        result.push_back(current_str);
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

// ================================================================================
// POSITION CHECKING & VALIDATION
// ================================================================================

// Check if pattern occurs at position with given degenerate string choices
bool EDS::check_position(Position common_pos,
                        const std::vector<int>& degenerate_strings,
                        const String& pattern) const {
    // Handle empty EDS
    if (is_empty_ || n_ == 0) {
        return false;
    }

    // Handle empty pattern
    if (pattern.empty()) {
        return true;  // Empty pattern always matches
    }

    // Find starting symbol using binary search
    Position offset_in_symbol = 0;
    size_t start_symbol = 0;

    try {
        start_symbol = find_symbol_at_common_position(common_pos, offset_in_symbol);
    } catch (const std::out_of_range&) {
        // Position is beyond EDS range
        return false;
    }

    // Warn if too many degenerate strings provided
    // Count expected number of degenerate symbols we'll traverse
    size_t expected_deg_count = 0;
    Length chars_counted = 0;
    for (size_t i = start_symbol; i < n_ && chars_counted < pattern.length(); i++) {
        if (metadata_.is_degenerate[i]) {
            expected_deg_count++;
        }
        // Estimate how many chars this symbol contributes
        size_t global_string_idx = metadata_.cum_set_sizes[i];
        Length sym_len = metadata_.string_lengths[global_string_idx];
        if (i == start_symbol) {
            sym_len = (sym_len > offset_in_symbol) ? (sym_len - offset_in_symbol) : 0;
        }
        chars_counted += sym_len;
    }

    if (degenerate_strings.size() > expected_deg_count) {
        std::cerr << "Warning: More degenerate strings provided ("
                  << degenerate_strings.size()
                  << ") than needed (" << expected_deg_count
                  << "). Extra strings will be ignored.\n";
    }

    // Source validation: check if path intersection is non-empty
    if (sources_) {
        PathSet path_intersection;
        try {
            path_intersection = calculate_path_intersection(
                start_symbol, offset_in_symbol,
                degenerate_strings, pattern.length()
            );
        } catch (const std::exception&) {
            // If path intersection calculation fails, propagate error
            throw;
        }

        // Empty intersection means no valid biological path exists
        if (path_intersection.empty()) {
            return false;
        }
    }

    // Reconstruct string from file
    String reconstructed;

    try {
        reconstructed = reconstruct_from_file(
            start_symbol, offset_in_symbol,
            degenerate_strings, pattern.length()
        );
    } catch (const std::exception&) {
        // If reconstruction fails (e.g., validation errors),
        // let the exception propagate
        throw;
    }

    // If we couldn't reconstruct enough characters, pattern doesn't match
    if (reconstructed.length() < pattern.length()) {
        return false;
    }

    // Compare reconstructed string with pattern
    return reconstructed == pattern;
}

// Position checking helper: decode absolute degenerate string number
std::pair<size_t, size_t> EDS::decode_degenerate_string_number(int abs_string_num) const {
    if (abs_string_num < 0) {
        throw std::invalid_argument(
            "Degenerate string number must be non-negative, got: " +
            std::to_string(abs_string_num)
        );
    }

    // Binary search to find which symbol this string belongs to
    auto it = std::upper_bound(
        metadata_.cum_degenerate_counts.begin(),
        metadata_.cum_degenerate_counts.end(),
        abs_string_num
    );

    if (it == metadata_.cum_degenerate_counts.begin()) {
        throw std::out_of_range(
            "Invalid degenerate string number: " + std::to_string(abs_string_num)
        );
    }

    size_t symbol_idx = std::distance(metadata_.cum_degenerate_counts.begin(), it) - 1;

    // Check if this symbol is actually degenerate
    if (!metadata_.is_degenerate[symbol_idx]) {
        throw std::runtime_error(
            "Internal error: degenerate string number " +
            std::to_string(abs_string_num) +
            " maps to non-degenerate symbol " + std::to_string(symbol_idx)
        );
    }

    size_t local_idx = abs_string_num - metadata_.cum_degenerate_counts[symbol_idx];

    // Validate local index is within range
    if (local_idx >= metadata_.symbol_sizes[symbol_idx]) {
        throw std::out_of_range(
            "Local index " + std::to_string(local_idx) +
            " out of range for symbol " + std::to_string(symbol_idx) +
            " (size: " + std::to_string(metadata_.symbol_sizes[symbol_idx]) + ")"
        );
    }

    return {symbol_idx, local_idx};
}

// Position checking helper: find symbol containing common position
size_t EDS::find_symbol_at_common_position(Position common_pos, Position& offset_out) const {
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

// Position checking helper: reconstruct string from memory (FULL mode)
String EDS::reconstruct_from_memory(size_t start_symbol,
                                   Position offset_in_symbol,
                                   const std::vector<int>& degenerate_strings,
                                   Length pattern_length) const {
    String result;
    result.reserve(pattern_length);

    size_t deg_idx = 0;
    bool first_symbol = true;

    for (size_t symbol_idx = start_symbol;
         symbol_idx < n_ && result.length() < pattern_length;
         symbol_idx++) {

        String str;

        if (metadata_.is_degenerate[symbol_idx]) {
            // Degenerate symbol: use specified string
            if (deg_idx >= degenerate_strings.size()) {
                throw std::invalid_argument(
                    "Not enough degenerate strings provided (need at least " +
                    std::to_string(deg_idx + 1) + ", got " +
                    std::to_string(degenerate_strings.size()) + ")"
                );
            }

            int abs_string_num = degenerate_strings[deg_idx];
            auto [expected_symbol, local_idx] = decode_degenerate_string_number(abs_string_num);

            // Verify this degenerate string belongs to current symbol
            if (expected_symbol != symbol_idx) {
                throw std::invalid_argument(
                    "Degenerate string " + std::to_string(abs_string_num) +
                    " belongs to symbol " + std::to_string(expected_symbol) +
                    ", but expected for symbol " + std::to_string(symbol_idx)
                );
            }

            str = sets_[symbol_idx][local_idx];
            deg_idx++;

        } else {
            // Common symbol: use the only string
            str = sets_[symbol_idx][0];

            // Apply offset if this is the first symbol
            if (first_symbol && offset_in_symbol > 0) {
                if (offset_in_symbol >= str.length()) {
                    throw std::out_of_range(
                        "Offset " + std::to_string(offset_in_symbol) +
                        " exceeds symbol length " + std::to_string(str.length())
                    );
                }
                str = str.substr(offset_in_symbol);
                first_symbol = false;
            }
        }

        // Take only what we need
        Length chars_to_take = std::min(
            static_cast<Length>(str.length()),
            static_cast<Length>(pattern_length - result.length())
        );

        result += str.substr(0, chars_to_take);
    }

    return result;
}

// Position checking helper: reconstruct string from file (METADATA_ONLY mode)
String EDS::reconstruct_from_file(size_t start_symbol,
                                 Position offset_in_symbol,
                                 const std::vector<int>& degenerate_strings,
                                 Length pattern_length) const {
    String result;
    result.reserve(pattern_length);

    size_t deg_idx = 0;
    bool first_symbol = true;

    for (size_t symbol_idx = start_symbol;
         symbol_idx < n_ && result.length() < pattern_length;
         symbol_idx++) {

        // Read symbol from file using existing method
        StringSet symbol_strings = read_symbol(symbol_idx);

        String str;

        if (metadata_.is_degenerate[symbol_idx]) {
            // Degenerate symbol: use specified string
            if (deg_idx >= degenerate_strings.size()) {
                throw std::invalid_argument(
                    "Not enough degenerate strings provided (need at least " +
                    std::to_string(deg_idx + 1) + ", got " +
                    std::to_string(degenerate_strings.size()) + ")"
                );
            }

            int abs_string_num = degenerate_strings[deg_idx];
            auto [expected_symbol, local_idx] = decode_degenerate_string_number(abs_string_num);

            // Verify this degenerate string belongs to current symbol
            if (expected_symbol != symbol_idx) {
                throw std::invalid_argument(
                    "Degenerate string " + std::to_string(abs_string_num) +
                    " belongs to symbol " + std::to_string(expected_symbol) +
                    ", but expected for symbol " + std::to_string(symbol_idx)
                );
            }

            if (local_idx >= symbol_strings.size()) {
                throw std::runtime_error(
                    "Local index " + std::to_string(local_idx) +
                    " out of range for symbol (size: " +
                    std::to_string(symbol_strings.size()) + ")"
                );
            }

            str = symbol_strings[local_idx];
            deg_idx++;

        } else {
            // Common symbol: use the only string
            if (symbol_strings.empty()) {
                throw std::runtime_error(
                    "Common symbol " + std::to_string(symbol_idx) + " is empty"
                );
            }

            str = symbol_strings[0];

            // Apply offset if this is the first symbol
            if (first_symbol && offset_in_symbol > 0) {
                if (offset_in_symbol >= str.length()) {
                    throw std::out_of_range(
                        "Offset " + std::to_string(offset_in_symbol) +
                        " exceeds symbol length " + std::to_string(str.length())
                    );
                }
                str = str.substr(offset_in_symbol);
                first_symbol = false;
            }
        }

        // Take only what we need
        Length chars_to_take = std::min(
            static_cast<Length>(str.length()),
            static_cast<Length>(pattern_length - result.length())
        );

        result += str.substr(0, chars_to_take);
    }

    return result;
}

// Position checking helper: calculate path intersection for source validation
PathSet EDS::calculate_path_intersection(size_t start_symbol,
                                         Position offset_in_symbol,
                                         const std::vector<int>& degenerate_strings,
                                         Length pattern_length) const {
    // If no sources loaded, return universal set {0}
    if (!sources_) {
        return {0};
    }

    // Start with universal set (all paths)
    PathSet intersection;
    bool first = true;

    size_t deg_idx = 0;
    Length chars_counted = 0;

    for (size_t symbol_idx = start_symbol;
         symbol_idx < n_ && chars_counted < pattern_length;
         symbol_idx++) {

        // Determine which string is used from this symbol
        size_t global_string_idx;

        if (metadata_.is_degenerate[symbol_idx]) {
            // Degenerate symbol: use specified string
            if (deg_idx >= degenerate_strings.size()) {
                throw std::invalid_argument(
                    "Not enough degenerate strings for path intersection calculation"
                );
            }

            int abs_string_num = degenerate_strings[deg_idx];
            auto [expected_symbol, local_idx] = decode_degenerate_string_number(abs_string_num);

            if (expected_symbol != symbol_idx) {
                throw std::invalid_argument(
                    "Degenerate string mismatch in path intersection calculation"
                );
            }

            // Convert to global string ID
            global_string_idx = metadata_.cum_set_sizes[symbol_idx] + local_idx;
            deg_idx++;

        } else {
            // Common symbol: use the only string
            global_string_idx = metadata_.cum_set_sizes[symbol_idx];

            // Apply offset for first symbol
            if (symbol_idx == start_symbol && offset_in_symbol > 0) {
                Length sym_len = metadata_.string_lengths[global_string_idx];
                if (offset_in_symbol >= sym_len) {
                    // Offset exceeds symbol length - invalid
                    return {};
                }
                sym_len -= offset_in_symbol;
                chars_counted += std::min(sym_len, static_cast<Length>(pattern_length - chars_counted));
            } else {
                Length sym_len = metadata_.string_lengths[global_string_idx];
                chars_counted += std::min(sym_len, static_cast<Length>(pattern_length - chars_counted));
            }
        }

        // Get source set for this string
        if (global_string_idx >= sources_->cardinality()) {
            throw std::runtime_error(
                "String ID " + std::to_string(global_string_idx) +
                " out of range for sources (size: " + std::to_string(sources_->cardinality()) + ")"
            );
        }

        const PathSet current_sources = sources_->read_source(global_string_idx);

        // Compute intersection
        if (first) {
            intersection = current_sources;
            first = false;
        } else {
            // Intersection with special handling for universal marker {0}
            bool current_has_universal = !current_sources.empty() && current_sources.front() == 0;
            bool accum_has_universal   = !intersection.empty()     && intersection.front()     == 0;

            if (current_has_universal && accum_has_universal) {
                intersection = {0};
            } else if (current_has_universal) {
                // intersection unchanged
            } else if (accum_has_universal) {
                intersection = current_sources;
            } else {
                PathSet new_intersection;
                std::set_intersection(
                    intersection.begin(), intersection.end(),
                    current_sources.begin(), current_sources.end(),
                    std::back_inserter(new_intersection)
                );
                intersection = std::move(new_intersection);
            }
        }

        // Early termination if intersection becomes empty
        if (intersection.empty()) {
            return {};
        }

        // Update chars_counted for degenerate symbols
        if (metadata_.is_degenerate[symbol_idx]) {
            Length sym_len = metadata_.string_lengths[global_string_idx];
            chars_counted += std::min(sym_len, static_cast<Length>(pattern_length - chars_counted));
        }
    }

    return intersection;
}

} // namespace edsparser
