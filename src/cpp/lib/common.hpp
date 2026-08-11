#ifndef EDSPARSER_COMMON_HPP
#define EDSPARSER_COMMON_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace edsparser {

// Version information
constexpr const char* VERSION = "1.0.0";

/**
 * Build provenance — which commit this binary was actually built from.
 *
 * Exists because a stale install silently shadowing a fresh build is not a
 * hypothetical: a pre-2026-08-04 `eds2leds` produces l-EDS output containing
 * strings no genome carries (the complement-source bug) without erroring, so
 * "which binary ran" is a correctness question, not just bookkeeping.
 * Experiment runners gate on `build_commit_date()`.
 */
const char* build_commit();       // short hash, or "unknown"
const char* build_commit_date();  // ISO-8601 UTC, sortable as a string
bool build_is_dirty();            // uncommitted changes to tracked files

/**
 * Print `--version` as machine-readable KEY=VALUE lines, matching the
 * convention `eds2leds --estimate-memory` already uses.
 */
void print_version(const std::string& tool_name);

// Common types
using String = std::string;
using StringSet = std::vector<String>;
using Position = uint64_t;
using Length = uint32_t;

// EDS format constants
constexpr char SET_OPEN = '{';
constexpr char SET_CLOSE = '}';
constexpr char SET_SEPARATOR = ',';
constexpr char CHANGE_SEPARATOR = '#';
constexpr char EMPTY_STRING_MARKER = '\0';

// File extensions
constexpr const char* EXT_MSA = ".msa"; // Multiple Sequence Alignment
constexpr const char* EXT_VCF = ".vcf"; // Variant Call Format
constexpr const char* EXT_EDS = ".eds"; // Elastic-Degenerate String
constexpr const char* EXT_EDZ = ".edz"; // Sources of Elastic-Degenerate String - binary
constexpr const char* EXT_SEDS = ".seds"; // Sources of Elastic-Degenerate String - simple
constexpr const char* EXT_LEDS = ".leds";   // Context-length limited EDS
constexpr const char* EXT_EDP = ".edp"; // EDS Patterns

// Error codes
enum class ErrorCode {
    SUCCESS = 0,
    FILE_NOT_FOUND = 1,
    INVALID_FORMAT = 2,
    INVALID_PARAMETER = 3,
    BUILD_FAILED = 4,
    QUERY_FAILED = 5,
    UNKNOWN_ERROR = 99
};

/**
 * High-resolution timer for performance measurements
 */
class Timer {
public:
    Timer();
    ~Timer();

    void start();
    void stop();
    double elapsed_seconds() const;
    double elapsed_milliseconds() const;
    double elapsed_microseconds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Get current process peak memory usage in MB
 * Returns 0.0 if unavailable (non-Linux platform or error reading /proc)
 */
double get_peak_memory_mb();

} // namespace edsparser

#endif // EDSPARSER_COMMON_HPP
