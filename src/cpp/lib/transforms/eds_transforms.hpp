#ifndef EDSPARSER_TRANSFORMS_EDS_TRANSFORMS_HPP
#define EDSPARSER_TRANSFORMS_EDS_TRANSFORMS_HPP

#include "../common.hpp"
#include "../formats/eds.hpp"
#include <iostream>
#include <string>
#include <filesystem>

namespace edsparser {

/**
 * EDS Transformation Functions
 *
 * This module provides transformations for Elastic-Degenerate Strings:
 * - EDS → l-EDS (length-constrained merging)
 * - Both LINEAR (phasing-aware) and CARTESIAN (all combinations) strategies
 */

/**
 * Estimate transformation complexity for EDS → l-EDS
 * Returns estimated iteration count and warnings about potential exponential growth
 */
struct TransformComplexity {
    // Diagnostic metrics
    size_t adjacent_degenerate_pairs;   // Count of adjacent degenerate symbols
    size_t short_contexts;              // Count of contexts shorter than threshold
    double avg_degenerate_cluster_size; // Average consecutive degenerate symbols

    // Risk warnings (simple categories: fast/slow/exponential)
    bool warn_slow;                     // True if transformation likely to be slow
    bool warn_exponential;              // True if potential exponential growth detected
    std::string recommendation;         // User-facing recommendation with actionable advice
};

/**
 * Estimate complexity of EDS → l-EDS transformation before running it
 * Use this to warn users about potentially slow transformations
 */
TransformComplexity estimate_leds_complexity(
    const EDS& eds,
    size_t context_length
);

/**
 * Worst-case peak-RAM estimate for the l-EDS merge, computed from metadata only
 * (no merging performed). Lets a caller refuse a transform up front instead of
 * discovering the blow-up mid-run.
 *
 * For each merge group the merge produces `merged_size` output strings and holds
 * `merged_size * (group_count * sizeof(size_t) + per-string source overhead)`
 * bytes of metadata in RAM. merged_size is bounded by the CARTESIAN product of the
 * group's symbol cardinalities; a LINEAR (phased) merge additionally cannot exceed
 * the number of source paths, so pass `path_cap = num_paths` to get the realistic
 * linear bound (path_cap == 0 assumes pure cartesian, no cap).
 *
 * peak_batch_bytes is the decision metric: the transform computes metadata one
 * batch of `batch_size` groups at a time, so peak RAM ≈ the largest batch's total.
 */
struct MergeMemoryEstimate {
    size_t             num_groups          = 0;
    unsigned long long peak_batch_bytes    = 0;   // max metadata RAM over any batch
    unsigned long long peak_group_bytes    = 0;   // largest single group's metadata
    unsigned long long peak_group_combos   = 0;   // output strings in the peak group
    size_t             peak_group_start    = 0;
    size_t             peak_group_count    = 0;
    unsigned long long total_output_strings = 0;  // Σ merged_size (informational)
    bool               saturated           = false; // a cartesian product overflowed → unbounded
    unsigned long long batch_size          = 1000;
    unsigned long long path_cap            = 0;
};

MergeMemoryEstimate estimate_worst_case_merge_memory(
    const EDS& eds,
    Length context_length,
    unsigned long long path_cap = 0,
    size_t batch_size = 1000
);

/**
 * Convert EDS to l-EDS using linear merging with phasing preservation
 *
 * @param input EDS input stream
 * @param output l-EDS output stream
 * @param context_length Minimum context length
 * @param phasing_input Optional phasing information (.seds file)
 * @param phasing_output Optional output for updated phasing
 * @param num_threads Number of threads for parallel processing (default: 1)
 * @param compact Use compact output format (omit brackets on non-degenerate symbols)
 */
void eds_to_leds_linear(
    std::istream& input,
    std::ostream& output,
    Length context_length,
    std::istream* phasing_input = nullptr,
    std::ostream* phasing_output = nullptr,
    size_t num_threads = 1,
    bool compact = true
);

/**
 * Path-based overload: avoids copying input to a temp file by symlinking
 * the provided paths into the iteration temp directory.  Critical for the
 * VCF→EDS→l-EDS two-stage pipeline where the intermediate SEDS can reach
 * 100+ GB (one {path,...} entry per EDS alternative per sample); a full
 * stream copy would require the same disk space a second time.
 *
 * Behaviour is otherwise identical to the stream-based overload.
 */
void eds_to_leds_linear(
    const std::filesystem::path& input_eds_path,
    std::ostream& output,
    Length context_length,
    const std::filesystem::path* input_seds_path = nullptr,
    std::ostream* phasing_output = nullptr,
    size_t num_threads = 1,
    bool compact = true
);

/**
 * Convert EDS to l-EDS using cartesian merging
 *
 * @param num_threads Number of threads for parallel processing (default: 1)
 * @param compact Use compact output format (omit brackets on non-degenerate symbols)
 */
void eds_to_leds_cartesian(
    std::istream& input,
    std::ostream& output,
    Length context_length,
    size_t num_threads = 1,
    bool compact = true
);

} // namespace edsparser

#endif // EDSPARSER_TRANSFORMS_EDS_TRANSFORMS_HPP
