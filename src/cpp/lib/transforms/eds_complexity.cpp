#include "eds_transforms.hpp"
#include <algorithm>
#include <sstream>
#include <cmath>
#include <iomanip>

namespace edsparser {

/**
 * Estimate complexity of EDS → l-EDS transformation
 *
 * Detects patterns that lead to exponential growth:
 * - Dense clusters of adjacent degenerate symbols
 * - Many short context blocks between degenerates
 *
 * These patterns cause iterative merging where each iteration
 * creates larger degenerate symbols that must be merged again.
 */
TransformComplexity estimate_leds_complexity(
    const EDS& eds,
    size_t context_length
) {
    TransformComplexity result;
    result.adjacent_degenerate_pairs = 0;
    result.short_contexts = 0;
    result.avg_degenerate_cluster_size = 0.0;
    result.warn_slow = false;
    result.warn_exponential = false;

    if (eds.length() < 2) {
        result.recommendation = "EDS too small to transform";
        return result;
    }

    const auto& is_degenerate = eds.get_metadata().is_degenerate;
    const auto& metadata = eds.get_metadata();

    // Count adjacent degenerate pairs
    size_t cluster_count = 0;
    size_t total_cluster_size = 0;
    size_t current_cluster = 0;

    for (size_t i = 0; i < eds.length(); ++i) {
        if (is_degenerate[i]) {
            current_cluster++;

            // Check if adjacent to next
            if (i + 1 < eds.length() && is_degenerate[i + 1]) {
                result.adjacent_degenerate_pairs++;
            }
        } else {
            // End of cluster
            if (current_cluster > 0) {
                cluster_count++;
                total_cluster_size += current_cluster;
                current_cluster = 0;
            }

            // Check if this is a short internal context
            if (i > 0 && i < eds.length() - 1) {
                size_t global_idx = metadata.cum_set_sizes[i];
                size_t len = metadata.string_lengths[global_idx];
                if (len < context_length) {
                    result.short_contexts++;
                }
            }
        }
    }

    // Handle final cluster
    if (current_cluster > 0) {
        cluster_count++;
        total_cluster_size += current_cluster;
    }

    result.avg_degenerate_cluster_size = cluster_count > 0
        ? static_cast<double>(total_cluster_size) / cluster_count
        : 0.0;

    // Use simple risk categorization instead of unreliable iteration predictions
    size_t violations = result.adjacent_degenerate_pairs + result.short_contexts;
    double degenerate_ratio = static_cast<double>(metadata.num_degenerate_symbols) / eds.length();

    // No violations = already satisfies l-EDS constraints
    if (violations == 0) {
        result.warn_slow = false;
        result.warn_exponential = false;
        result.recommendation = "✓ Already satisfies l-EDS constraints (no merging needed)";
        return result;
    }

    // Risk categorization based on structure patterns
    // EXPONENTIAL RISK: High degenerate density + large clusters
    if (degenerate_ratio > 0.3 && result.avg_degenerate_cluster_size > 5.0) {
        result.warn_exponential = true;
        result.warn_slow = true;
    }
    // SLOW: Many violations
    else if (violations > 1000) {
        result.warn_slow = true;
        result.warn_exponential = false;
    }
    // FAST: Few violations, should complete quickly
    else {
        result.warn_slow = false;
        result.warn_exponential = false;
    }

    // Generate recommendations based on risk category
    std::ostringstream oss;

    if (result.warn_exponential) {
        oss << "⚠ EXPONENTIAL GROWTH RISK!\n";
        oss << "  - High degenerate density (" << std::fixed << std::setprecision(1)
            << (degenerate_ratio * 100) << "% of symbols)\n";
        oss << "  - Large clusters (avg " << result.avg_degenerate_cluster_size << " consecutive)\n";
        oss << "  - " << violations << " violations to resolve (out of " << eds.length() << " symbols)\n";
        oss << "  - Transformation may take VERY long time or not complete\n\n";
        oss << "Recommendations:\n";
        oss << "  1. Decrease context length (current: " << context_length << ") to reduce violations\n";
        oss << "  2. Use CARTESIAN mode if phasing not required\n";
        oss << "  3. Consider if l-EDS is necessary for your use case";
    } else if (result.warn_slow) {
        oss << "⚠ SLOW transformation expected\n";
        oss << "  - " << violations << " violations to resolve (out of " << eds.length() << " symbols)\n";
        oss << "  - May take several minutes\n\n";
        oss << "Recommendation:\n";
        oss << "  Consider decreasing context length (current: " << context_length << ") to reduce violations";
    } else {
        oss << "✓ Transformation should complete quickly\n";
        oss << "  - " << violations << " violations out of " << eds.length() << " symbols (manageable)\n";
        oss << "  - Expected runtime: < 1 minute for moderate files";
    }

    result.recommendation = oss.str();
    return result;
}

} // namespace edsparser
