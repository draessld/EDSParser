// Types, EDS syntax constants and small utilities shared by every translation
// unit in the library. Anything here is depended on by all of formats/ and
// transforms/, so it must stay free of dependencies on either.
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

// EDS syntax. A symbol is `{alt1,alt2,...}`; a non-degenerate symbol may drop
// its braces in compact format. CHANGE_SEPARATOR delimits alternatives in the
// flat "changes" sequence a downstream index builds from an EDS, and is chosen
// outside the DNA alphabet so a query can never match across it.
constexpr char SET_OPEN = '{';
constexpr char SET_CLOSE = '}';
constexpr char SET_SEPARATOR = ',';
constexpr char CHANGE_SEPARATOR = '#';

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
 * Wall-clock timer for the runtime figure every tool prints on exit.
 * elapsed_seconds() may be read while running; it then measures up to now.
 */
class Timer {
public:
    Timer();
    ~Timer();

    void start();
    void stop();
    double elapsed_seconds() const;

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
