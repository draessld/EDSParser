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
EDS::EDS(std::istream& eds_stream) : is_empty_(false), sources_(nullptr) {
    parse(eds_stream);
}

// String-based constructor (EDS only, no sources)
// Uses std::istringstream to avoid temp file I/O
EDS::EDS(const std::string& eds_string) : is_empty_(false), sources_(nullptr) {
    // Normalize the input string
    std::string normalized = normalize_eds_format(eds_string);
    // Remove whitespace
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), ::isspace), normalized.end());
    
    // Use a string stream for in-memory parsing
    std::istringstream iss(normalized);
    // Parse metadata
    parse(iss);
    // Note: read_symbol() will not work for objects created from a string,
    // as there is no persistent stream. This constructor is for in-memory representation.
}

void EDS::parse(std::istream& is) {
    // Streaming parser: builds an index without loading file content into memory.
    // Handles both compact (ACGT{A,C}) and full ({ACGT}{A,C}) formats.
    // Uses a byte offset counter instead of tellg() to avoid per-character virtual calls.

    // Clear all data structures
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
        if (is_bracketed) {
            // Manual comma scan instead of stringstream — no heap allocation per segment
            if (token.empty()) {
                // Empty set {} means one empty string ""
                metadata_.string_lengths.push_back(0);
                symbol_size = 1;
                metadata_.num_empty_strings++;
            } else {
                size_t start = 0;
                for (size_t pos = 0; pos <= token.size(); ++pos) {
                    if (pos == token.size() || token[pos] == SET_SEPARATOR) {
                        size_t len = pos - start;
                        metadata_.string_lengths.push_back(len);
                        N_ += len;
                        if (len == 0) metadata_.num_empty_strings++;
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
        }

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

    char ch;
    std::string current_token;
    size_t byte_offset = 0;

    // Consume leading whitespace manually (track byte offset)
    while (is.peek() != EOF && std::isspace(static_cast<unsigned char>(is.peek()))) {
        is.get(ch);
        byte_offset++;
    }

    while (is.peek() != EOF) {
        if (is.peek() == SET_OPEN) {
            // Flush any accumulated non-bracketed token
            if (!current_token.empty()) {
                process_token(current_token, false);
                current_token.clear();
            }

            // Record position of '{', then consume it
            metadata_.base_positions.push_back(static_cast<std::streampos>(byte_offset));
            is.get(ch); // consume '{'
            byte_offset++;

            std::string bracketed_content;
            std::getline(is, bracketed_content, SET_CLOSE);
            if (!is) throw std::runtime_error("Unmatched '{' in EDS stream.");
            byte_offset += bracketed_content.size() + 1; // +1 for the consumed '}'

            process_token(bracketed_content, true);
        } else {
            if (current_token.empty()) {
                metadata_.base_positions.push_back(static_cast<std::streampos>(byte_offset));
            }
            is.get(ch);
            byte_offset++;
            current_token += ch;
        }

        if (is.peek() == SET_OPEN || is.peek() == EOF ||
            std::isspace(static_cast<unsigned char>(is.peek()))) {
            if (!current_token.empty()) {
                process_token(current_token, false);
                current_token.clear();
            }
            // Consume whitespace manually
            while (is.peek() != EOF && std::isspace(static_cast<unsigned char>(is.peek()))) {
                is.get(ch);
                byte_offset++;
            }
        }
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
    eds.sources_ = Sources::load(seds_path, Sources::Format::SEDS);

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
std::set<int> EDS::read_source(size_t string_id) const {
    if (!sources_) {
        throw std::runtime_error("No sources loaded");
    }
    return sources_->read_source(string_id);
}

// Configure source cache capacity (delegates to Sources)
void EDS::set_source_cache_capacity(size_t capacity) {
    if (sources_) {
        sources_->set_cache_capacity(capacity);
    }
}

// Clear source cache manually (delegates to Sources)
void EDS::clear_source_cache() const {
    if (sources_) {
        sources_->clear_cache();
    }
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

// (Deleted: calculate_source_statistics() - now in Sources class)

EDS::Statistics EDS::get_statistics() const {
    // Return Statistics struct from Metadata (for backward compatibility)
    Statistics stats;
    stats.min_context_length = metadata_.min_context_length;
    stats.max_context_length = metadata_.max_context_length;
    stats.avg_context_length = metadata_.avg_context_length;
    stats.num_degenerate_symbols = metadata_.num_degenerate_symbols;
    stats.num_common_chars = metadata_.num_common_chars;
    stats.total_change_size = metadata_.total_change_size;
    stats.num_empty_strings = metadata_.num_empty_strings;
    return stats;
}

// ================================================================================
// OUTPUT METHODS
// ================================================================================

void EDS::print_statistics(std::ostream& os) const {
    Statistics stats = get_statistics();

    os << "========================================\n";
    os << "EDS Statistics\n";
    os << "========================================\n";
    os << "Structure:\n";
    os << "  Number of sets (n):           " << n_ << "\n";
    os << "  Total characters (N):         " << N_ << "\n";
    os << "  Total strings (m):            " << m_ << "\n";
    os << "  Degenerate symbols:           " << stats.num_degenerate_symbols << "\n";
    os << "  Regular symbols:              " << (n_ - stats.num_degenerate_symbols) << "\n";
    os << "\n";
    os << "Context Lengths:\n";
    os << "  Minimum:                      " << stats.min_context_length << "\n";
    os << "  Maximum:                      " << stats.max_context_length << "\n";
    os << "  Average:                      " << stats.avg_context_length << "\n";
    os << "\n";
    os << "Variations:\n";
    os << "  Total change size:            " << stats.total_change_size << "\n";
    os << "  Common characters:            " << stats.num_common_chars << "\n";
    os << "  Empty strings:                " << stats.num_empty_strings << "\n";
    os << "\n";
    if (sources_) {
        os << "Sources: Loaded (" << sources_->cardinality() << " strings with source info)\n";
    } else {
        os << "Sources: Not loaded\n";
    }
    os << "========================================\n";
}

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
StringSet EDS::read_symbol_from_stream(Position pos) const {
    // Stream from file
    if (!stream_.is_open()) {
        throw std::runtime_error("File stream not available for reading symbol");
    }

    // Seek to symbol position
    stream_.clear();  // Clear any error flags
    stream_.seekg(metadata_.base_positions[pos]);

    if (!stream_) {
        throw std::runtime_error("Failed to seek to position " + std::to_string(pos));
    }

    // Parse one symbol
    StringSet result;
    char ch;
    std::string current_str;

    // Expect '{'
    if (!stream_.get(ch) || ch != SET_OPEN) {
        throw std::runtime_error("Expected '{' at position " + std::to_string(pos));
    }

    // Parse strings in symbol
    while (stream_.get(ch) && ch != SET_CLOSE) {
        if (ch == SET_SEPARATOR) {
            result.push_back(current_str);
            current_str.clear();
        } else if (!std::isspace(ch)) {  // Skip whitespace
            current_str += ch;
        }
    }

    // Add last string
    result.push_back(current_str);

    return result;
}

// Public accessor for read_symbol (works in both modes)
StringSet EDS::read_symbol(Position pos) const {
    if (pos >= n_) {
        throw std::out_of_range("Position " + std::to_string(pos) + " out of range");
    }
    return read_symbol_from_stream(pos);
}

// ================================================================================
// POSITION CHECKING & VALIDATION
// ================================================================================

// get_sets() - deprecated (streaming mode only)
const std::vector<StringSet>& EDS::get_sets() const {
    throw std::runtime_error(
        "Direct access to sets is not supported in streaming mode. "
        "Use read_symbol(pos) for on-demand access"
    );
}

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
        std::set<int> path_intersection;
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
std::set<int> EDS::calculate_path_intersection(size_t start_symbol,
                                               Position offset_in_symbol,
                                               const std::vector<int>& degenerate_strings,
                                               Length pattern_length) const {
    // If no sources loaded, return universal set {0}
    if (!sources_) {
        return {0};
    }

    // Start with universal set (all paths)
    std::set<int> intersection;
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

        const std::set<int> current_sources = sources_->read_source(global_string_idx);

        // Compute intersection
        if (first) {
            intersection = current_sources;
            first = false;
        } else {
            // Intersection with special handling for universal marker {0}
            std::set<int> new_intersection;

            bool current_has_universal = current_sources.count(0) > 0;
            bool accum_has_universal = intersection.count(0) > 0;

            if (current_has_universal && accum_has_universal) {
                // {0} ∩ {0} = {0}
                new_intersection.insert(0);
            } else if (current_has_universal) {
                // {0} ∩ {x,y,...} = {x,y,...}
                new_intersection = intersection;
            } else if (accum_has_universal) {
                // {x,y,...} ∩ {0} = {x,y,...}
                new_intersection = current_sources;
            } else {
                // Regular set intersection
                std::set_intersection(
                    intersection.begin(), intersection.end(),
                    current_sources.begin(), current_sources.end(),
                    std::inserter(new_intersection, new_intersection.begin())
                );
            }

            intersection = new_intersection;
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
