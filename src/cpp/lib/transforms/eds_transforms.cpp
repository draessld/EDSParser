#include "eds_transforms.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <stdexcept>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace edsparser {

namespace {
    /**
     * Represents a pair of adjacent positions to merge
     */
    struct MergePair {
        size_t pos1;
        size_t pos2;

        MergePair(size_t p1, size_t p2) : pos1(p1), pos2(p2) {}
    };

    /**
     * Result of merging a pair of positions (OLD: for backward compatibility)
     */
    struct MergeResult {
        size_t original_pos1;
        size_t original_pos2;
        StringSet merged_set;
        std::vector<std::set<int>> merged_sources;  // Empty if no sources
    };

    /**
     * Metadata-only result of merging a pair of positions
     * This struct contains NO string data - only metadata for memory-efficient processing
     */
    struct MergeMetadata {
        size_t original_pos1;
        size_t original_pos2;
        size_t merged_size;  // Number of strings in merged symbol
        std::vector<Length> merged_string_lengths;  // Length of each string (no actual strings)
        std::vector<std::set<int>> merged_sources;  // Empty if no sources
    };

    /**
     * Select independent pairs of adjacent positions to merge in parallel.
     *
     * Strategy: Greedy left-to-right selection, choosing pairs where merging
     * would help satisfy the l-EDS property. Ensures no overlapping positions
     * so parallel merging is safe.
     *
     * @param eds The EDS to analyze
     * @param context_length Minimum context length l
     * @return Vector of non-overlapping merge pairs
     */
    std::vector<MergePair> select_independent_merge_pairs(
        const EDS& eds,
        Length context_length
    ) {
        std::vector<MergePair> pairs;

        if (eds.length() < 2) {
            return pairs;  // Need at least 2 positions to merge
        }

        // Get degenerate flags from metadata
        const auto& is_degenerate = eds.get_is_degenerate();

        // Track which positions are already included in pairs
        std::vector<bool> used(eds.length(), false);

        // Greedy left-to-right selection
        for (size_t i = 0; i + 1 < eds.length(); ++i) {
            if (used[i] || used[i + 1]) {
                continue;  // Position already in a pair
            }

            // Only merge if it would fix an l-EDS violation
            // Cases to merge:
            // 1. Internal common block with length < l
            // 2. Two adjacent degenerate symbols (implicit empty common block)
            bool should_merge = false;

            // Check if position i is an internal common block that's too short
            if (!is_degenerate[i] && i > 0 && i < eds.length() - 1) {
                size_t global_idx1 = eds.get_metadata().cum_set_sizes[i];
                Length len1 = eds.get_string_length(global_idx1);
                if (len1 < context_length) {
                    should_merge = true;
                }
            }

            // Check if position i+1 is an internal common block that's too short
            if (!is_degenerate[i + 1] && (i + 1) > 0 && (i + 1) < eds.length() - 1) {
                size_t global_idx2 = eds.get_metadata().cum_set_sizes[i + 1];
                Length len2 = eds.get_string_length(global_idx2);
                if (len2 < context_length) {
                    should_merge = true;
                }
            }

            // Check if both positions are degenerate (implicit empty common)
            // This represents an implicit {} between them, which has length 0 < context_length
            // Note: Edge case exemption applies only to common blocks, not degenerate symbols
            if (is_degenerate[i] && is_degenerate[i + 1]) {
                should_merge = true;
            }

            if (should_merge) {
                pairs.emplace_back(i, i + 1);
                used[i] = true;
                used[i + 1] = true;
            }
        }

        return pairs;
    }

    /**
     * Compute merge metadata for multiple pairs WITHOUT building string data.
     * This is the memory-efficient version that works with METADATA_ONLY mode.
     *
     * Strategy: Calculate merged sizes, lengths, and sources using only metadata.
     * No actual string concatenation occurs - strings are read on-demand during output streaming.
     *
     * @param eds The original EDS (can be METADATA_ONLY mode)
     * @param pairs Vector of non-overlapping merge pairs
     * @param num_threads Number of threads to use (1 = sequential)
     * @return Vector of metadata-only merge results
     */
    std::vector<MergeMetadata> compute_merge_metadata(
        const EDS& eds,
        const std::vector<MergePair>& pairs,
        size_t num_threads
    ) {
        std::vector<MergeMetadata> results(pairs.size());

        // Lambda function to compute metadata for a single pair
        auto compute_pair_metadata = [&eds](const MergePair& pair) -> MergeMetadata {
            MergeMetadata result;
            result.original_pos1 = pair.pos1;
            result.original_pos2 = pair.pos2;

            // Get metadata for both positions
            const auto& metadata = eds.get_metadata();
            size_t global_string_idx1 = metadata.cum_set_sizes[pair.pos1];
            size_t global_string_idx2 = metadata.cum_set_sizes[pair.pos2];
            size_t set1_size = metadata.symbol_sizes[pair.pos1];
            size_t set2_size = metadata.symbol_sizes[pair.pos2];

            // Determine if we're doing LINEAR or CARTESIAN merge
            bool has_sources = eds.has_sources();

            if (!has_sources) {
                // CARTESIAN merge: size is product of set sizes
                result.merged_size = set1_size * set2_size;

                // Calculate string lengths for all combinations
                result.merged_string_lengths.reserve(result.merged_size);
                for (size_t i = 0; i < set1_size; ++i) {
                    Length len1 = metadata.string_lengths[global_string_idx1 + i];
                    for (size_t j = 0; j < set2_size; ++j) {
                        Length len2 = metadata.string_lengths[global_string_idx2 + j];
                        result.merged_string_lengths.push_back(len1 + len2);
                    }
                }
            } else {
                // LINEAR merge: only count valid combinations (non-empty source intersection)
                for (size_t i = 0; i < set1_size; ++i) {
                    const std::set<int> sources1 = eds.read_source(global_string_idx1 + i);
                    Length len1 = metadata.string_lengths[global_string_idx1 + i];

                    for (size_t j = 0; j < set2_size; ++j) {
                        const std::set<int> sources2 = eds.read_source(global_string_idx2 + j);
                        Length len2 = metadata.string_lengths[global_string_idx2 + j];

                        // Compute intersection with special handling for {0} (universal marker)
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

                        // Only keep if intersection is non-empty
                        if (!intersection.empty()) {
                            result.merged_sources.push_back(intersection);
                            result.merged_string_lengths.push_back(len1 + len2);
                        }
                    }
                }

                result.merged_size = result.merged_sources.size();

                // Validation: merged set must not be empty
                if (result.merged_size == 0) {
                    throw std::runtime_error(
                        "Merging positions " + std::to_string(pair.pos1) + " and " +
                        std::to_string(pair.pos2) + " results in empty set "
                        "(no valid source intersections)"
                    );
                }
            }

            return result;
        };

        // Process pairs in parallel or sequentially
        if (num_threads <= 1 || pairs.empty()) {
            // Sequential execution
            for (size_t i = 0; i < pairs.size(); ++i) {
                results[i] = compute_pair_metadata(pairs[i]);
            }
        } else {
            // Parallel execution with OpenMP
#ifdef _OPENMP
            #pragma omp parallel for num_threads(num_threads)
            for (size_t i = 0; i < pairs.size(); ++i) {
                results[i] = compute_pair_metadata(pairs[i]);
            }
#else
            // OpenMP not available, fall back to sequential
            for (size_t i = 0; i < pairs.size(); ++i) {
                results[i] = compute_pair_metadata(pairs[i]);
            }
#endif
        }

        return results;
    }

    /**
     * Merge multiple pairs of positions in parallel.
     *
     * Each pair is processed independently, then results are combined
     * to construct a new EDS.
     *
     * @param eds The original EDS
     * @param pairs Vector of non-overlapping merge pairs
     * @param num_threads Number of threads to use (1 = sequential)
     * @return Vector of merge results
     */
    std::vector<MergeResult> merge_multiple_pairs(
        const EDS& eds,
        const std::vector<MergePair>& pairs,
        size_t num_threads
    ) {
        std::vector<MergeResult> results(pairs.size());

        if (num_threads <= 1 || pairs.empty()) {
            // Sequential execution
            for (size_t i = 0; i < pairs.size(); ++i) {
                const auto& pair = pairs[i];
                EDS merged = eds.merge_adjacent(pair.pos1, pair.pos2);

                results[i].original_pos1 = pair.pos1;
                results[i].original_pos2 = pair.pos2;
                // merge_adjacent returns full EDS with positions merged at pos1
                results[i].merged_set = merged.read_symbol(pair.pos1);

                // Extract sources if present
                if (eds.has_sources()) {
                    size_t merged_size = merged.get_symbol_size(pair.pos1);
                    const auto& all_sources = merged.get_sources();
                    size_t global_idx = merged.get_metadata().cum_set_sizes[pair.pos1];
                    results[i].merged_sources.resize(merged_size);
                    for (size_t j = 0; j < merged_size; ++j) {
                        results[i].merged_sources[j] = all_sources[global_idx + j];
                    }
                }
            }
        } else {
            // Parallel execution with OpenMP
#ifdef _OPENMP
            #pragma omp parallel for num_threads(num_threads)
            for (size_t i = 0; i < pairs.size(); ++i) {
                const auto& pair = pairs[i];
                EDS merged = eds.merge_adjacent(pair.pos1, pair.pos2);

                results[i].original_pos1 = pair.pos1;
                results[i].original_pos2 = pair.pos2;
                results[i].merged_set = merged.read_symbol(pair.pos1);

                // Extract sources if present
                if (eds.has_sources()) {
                    size_t merged_size = merged.get_symbol_size(pair.pos1);
                    const auto& all_sources = merged.get_sources();
                    size_t global_idx = merged.get_metadata().cum_set_sizes[pair.pos1];
                    results[i].merged_sources.resize(merged_size);
                    for (size_t j = 0; j < merged_size; ++j) {
                        results[i].merged_sources[j] = all_sources[global_idx + j];
                    }
                }
            }
#else
            // OpenMP not available, fall back to sequential
            for (size_t i = 0; i < pairs.size(); ++i) {
                const auto& pair = pairs[i];
                EDS merged = eds.merge_adjacent(pair.pos1, pair.pos2);

                results[i].original_pos1 = pair.pos1;
                results[i].original_pos2 = pair.pos2;
                results[i].merged_set = merged.read_symbol(pair.pos1);

                if (eds.has_sources()) {
                    size_t merged_size = merged.get_symbol_size(pair.pos1);
                    const auto& all_sources = merged.get_sources();
                    size_t global_idx = merged.get_metadata().cum_set_sizes[pair.pos1];
                    results[i].merged_sources.resize(merged_size);
                    for (size_t j = 0; j < merged_size; ++j) {
                        results[i].merged_sources[j] = all_sources[global_idx + j];
                    }
                }
            }
#endif
        }

        return results;
    }

    /**
     * Reconstruct EDS from original and merge results.
     *
     * Builds a new EDS by combining unmodified positions with merged results.
     *
     * @param original The original EDS
     * @param merge_results Results from parallel merging
     * @return New EDS with merges applied
     */
    EDS reconstruct_eds(
        const EDS& original,
        const std::vector<MergeResult>& merge_results
    ) {
        // Build mapping: position -> merge result index (or -1 if not merged)
        std::vector<int> merge_map(original.length(), -1);
        std::vector<bool> skip(original.length(), false);

        for (size_t i = 0; i < merge_results.size(); ++i) {
            merge_map[merge_results[i].original_pos1] = static_cast<int>(i);
            skip[merge_results[i].original_pos2] = true;  // Second position consumed by merge
        }

        // Build new EDS string and sources
        std::ostringstream eds_stream;
        std::ostringstream sources_stream;
        bool has_sources = original.has_sources();
        const auto& all_sources = has_sources ? original.get_sources() : std::vector<std::set<int>>();

        for (size_t pos = 0; pos < original.length(); ++pos) {
            if (skip[pos]) {
                continue;  // Position was merged into previous
            }

            if (merge_map[pos] >= 0) {
                // Use merged result
                const auto& result = merge_results[merge_map[pos]];
                const auto& merged_set = result.merged_set;

                // Write merged symbol
                eds_stream << '{';
                for (size_t i = 0; i < merged_set.size(); ++i) {
                    if (i > 0) eds_stream << ',';
                    eds_stream << merged_set[i];
                }
                eds_stream << '}';

                // Write merged sources if present
                if (has_sources) {
                    for (size_t i = 0; i < result.merged_sources.size(); ++i) {
                        sources_stream << '{';
                        bool first = true;
                        for (int path_id : result.merged_sources[i]) {
                            if (!first) sources_stream << ',';
                            sources_stream << path_id;
                            first = false;
                        }
                        sources_stream << '}';
                    }
                }
            } else {
                // Copy original symbol
                const auto& symbol = original.read_symbol(pos);
                eds_stream << '{';
                for (size_t i = 0; i < symbol.size(); ++i) {
                    if (i > 0) eds_stream << ',';
                    eds_stream << symbol[i];
                }
                eds_stream << '}';

                // Copy original sources if present
                if (has_sources) {
                    size_t symbol_size = original.get_symbol_size(pos);
                    size_t global_idx = original.get_metadata().cum_set_sizes[pos];
                    for (size_t i = 0; i < symbol_size; ++i) {
                        const auto& src = all_sources[global_idx + i];
                        sources_stream << '{';
                        bool first = true;
                        for (int path_id : src) {
                            if (!first) sources_stream << ',';
                            sources_stream << path_id;
                            first = false;
                        }
                        sources_stream << '}';
                    }
                }
            }
        }

        // Construct new EDS
        std::string eds_str = eds_stream.str();


        if (has_sources) {
            std::string sources_str = sources_stream.str();
            return EDS(eds_str, sources_str);
        } else {
            return EDS(eds_str);
        }
    }

    /**
     * Stream merged EDS symbols directly to file WITHOUT accumulating in memory.
     * This is the memory-efficient version that works with METADATA_ONLY mode.
     *
     * Strategy: Read symbols on-demand, merge strings on-the-fly, write immediately, flush.
     * No ostringstream accumulation - direct file writes prevent memory growth.
     *
     * @param input_eds The original EDS (can be METADATA_ONLY mode)
     * @param merge_metadata Vector of metadata-only merge results
     * @param eds_out Output stream for EDS data
     * @param sources_out Output stream for sources data (nullptr if no sources)
     */
    void stream_merged_symbols_to_file(
        const EDS& input_eds,
        const std::vector<MergeMetadata>& merge_metadata,
        std::ostream& eds_out,
        std::ostream* sources_out
    ) {
        // Build mapping: position -> merge metadata index (or -1 if not merged)
        std::vector<int> merge_map(input_eds.length(), -1);
        std::vector<bool> skip(input_eds.length(), false);

        for (size_t i = 0; i < merge_metadata.size(); ++i) {
            merge_map[merge_metadata[i].original_pos1] = static_cast<int>(i);
            skip[merge_metadata[i].original_pos2] = true;  // Second position consumed by merge
        }

        bool has_sources = input_eds.has_sources();
        const auto& metadata = input_eds.get_metadata();

        // Stream output symbol-by-symbol
        for (size_t pos = 0; pos < input_eds.length(); ++pos) {
            if (skip[pos]) {
                continue;  // Position was merged into previous
            }

            if (merge_map[pos] >= 0) {
                // ===== MERGED POSITION =====
                const auto& merge_meta = merge_metadata[merge_map[pos]];

                // Read BOTH symbols on-demand (METADATA_ONLY compatible)
                StringSet set1 = input_eds.read_symbol(pos);
                StringSet set2 = input_eds.read_symbol(pos + 1);

                // Get source indices for filtering
                size_t global_string_idx1 = metadata.cum_set_sizes[pos];
                size_t global_string_idx2 = metadata.cum_set_sizes[pos + 1];

                // Write merged symbol to output
                eds_out << '{';

                bool first_string = true;
                size_t merged_idx = 0;

                if (!has_sources) {
                    // CARTESIAN merge: all combinations
                    for (size_t i = 0; i < set1.size(); ++i) {
                        for (size_t j = 0; j < set2.size(); ++j) {
                            if (!first_string) eds_out << ',';
                            eds_out << set1[i] << set2[j];  // Concatenate on-the-fly
                            first_string = false;
                        }
                    }
                } else {
                    // LINEAR merge: only valid combinations (source intersection check)
                    for (size_t i = 0; i < set1.size(); ++i) {
                        for (size_t j = 0; j < set2.size(); ++j) {
                            // Check if this combination is in the merged metadata
                            // (it was pre-filtered during compute_merge_metadata)
                            if (merged_idx < merge_meta.merged_size) {
                                const std::set<int> sources1 = input_eds.read_source(global_string_idx1 + i);
                                const std::set<int> sources2 = input_eds.read_source(global_string_idx2 + j);

                                // Compute intersection (same logic as compute_merge_metadata)
                                std::set<int> intersection;
                                bool sources1_has_universal = sources1.count(0) > 0;
                                bool sources2_has_universal = sources2.count(0) > 0;

                                if (sources1_has_universal && sources2_has_universal) {
                                    intersection.insert(0);
                                } else if (sources1_has_universal) {
                                    intersection = sources2;
                                } else if (sources2_has_universal) {
                                    intersection = sources1;
                                } else {
                                    std::set_intersection(
                                        sources1.begin(), sources1.end(),
                                        sources2.begin(), sources2.end(),
                                        std::inserter(intersection, intersection.begin())
                                    );
                                }

                                // Only write if intersection is non-empty
                                if (!intersection.empty()) {
                                    if (!first_string) eds_out << ',';
                                    eds_out << set1[i] << set2[j];  // Concatenate on-the-fly
                                    first_string = false;

                                    // Write sources for this merged string
                                    if (sources_out) {
                                        *sources_out << '{';
                                        bool first_path = true;
                                        for (int path_id : intersection) {
                                            if (!first_path) *sources_out << ',';
                                            *sources_out << path_id;
                                            first_path = false;
                                        }
                                        *sources_out << '}';
                                    }

                                    merged_idx++;
                                }
                            }
                        }
                    }
                }

                eds_out << '}';

                // Flush immediately to prevent buffering
                eds_out.flush();
                if (sources_out) {
                    sources_out->flush();
                }

            } else {
                // ===== UNMODIFIED POSITION =====
                // Read symbol on-demand and pass through
                StringSet symbol = input_eds.read_symbol(pos);

                // Write symbol
                eds_out << '{';
                for (size_t i = 0; i < symbol.size(); ++i) {
                    if (i > 0) eds_out << ',';
                    eds_out << symbol[i];
                }
                eds_out << '}';

                // Write sources if present
                if (has_sources && sources_out) {
                    size_t symbol_size = input_eds.get_symbol_size(pos);
                    size_t global_idx = metadata.cum_set_sizes[pos];
                    for (size_t i = 0; i < symbol_size; ++i) {
                        const std::set<int> src = input_eds.read_source(global_idx + i);
                        *sources_out << '{';
                        bool first = true;
                        for (int path_id : src) {
                            if (!first) *sources_out << ',';
                            *sources_out << path_id;
                            first = false;
                        }
                        *sources_out << '}';
                    }
                }

                // Flush immediately to prevent buffering
                eds_out.flush();
                if (sources_out) {
                    sources_out->flush();
                }
            }
        }
    }

} // anonymous namespace

/**
 * Convert EDS to l-EDS using linear merging with phasing preservation.
 *
 * Iteratively merges adjacent positions until all internal common blocks
 * have length >= context_length. Uses parallel processing when num_threads > 1.
 *
 * @param input EDS input stream
 * @param output l-EDS output stream
 * @param context_length Minimum context length l
 * @param phasing_input Optional phasing information (.seds file)
 * @param phasing_output Optional output for updated phasing
 * @param num_threads Number of threads for parallel processing (default: 1)
 */
void eds_to_leds_linear(
    std::istream& input,
    std::ostream& output,
    Length context_length,
    std::istream* phasing_input,
    std::ostream* phasing_output,
    size_t num_threads,
    bool compact
) {
    if (context_length == 0) {
        throw std::invalid_argument("context_length must be > 0 for l-EDS transformation");
    }

    // ===== STREAMING ARCHITECTURE FOR MEMORY STABILITY =====
    // This implementation uses METADATA_ONLY mode with temp files to handle 100GB+ files
    // with minimal memory footprint (~500MB for 100GB file)

    // Create temp directory for iteration files
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "edsparser_leds";
    std::filesystem::create_directories(temp_dir);

    // Save input stream to temp file (needed for METADATA_ONLY loading)
    std::filesystem::path temp_input = temp_dir / "input.eds";
    {
        std::ofstream temp_out(temp_input);
        if (!temp_out) {
            throw std::runtime_error("Failed to create temp input file: " + temp_input.string());
        }
        temp_out << input.rdbuf();
        temp_out.close();
    }

    // Save sources if provided
    std::filesystem::path temp_sources_input;
    bool has_sources = (phasing_input != nullptr);
    if (has_sources) {
        temp_sources_input = temp_dir / "input.seds";
        std::ofstream temp_seds_out(temp_sources_input);
        if (!temp_seds_out) {
            throw std::runtime_error("Failed to create temp sources file: " + temp_sources_input.string());
        }
        temp_seds_out << phasing_input->rdbuf();
        temp_seds_out.close();
    }

    // Load as METADATA_ONLY (only ~100MB for 100GB file!)
    EDS eds = has_sources
        ? EDS::load(temp_input, temp_sources_input, EDS::StoringMode::METADATA_ONLY)
        : EDS::load(temp_input, EDS::StoringMode::METADATA_ONLY);

    // Iterative merging until convergence
    size_t iteration = 0;
    const size_t MAX_ITERATIONS = 10000;  // Safety limit
    const size_t BATCH_SIZE = 1000;  // Process 1000 pairs per batch to control parallel memory

    std::filesystem::path current_eds_file = temp_input;
    std::filesystem::path current_seds_file = temp_sources_input;

    while (iteration < MAX_ITERATIONS) {
        // Check convergence (metadata-only operation)
        if (is_leds(eds, context_length)) {
            break;  // All internal common blocks satisfy l-EDS property
        }

        // Select independent pairs to merge (metadata-only operation)
        auto pairs = select_independent_merge_pairs(eds, context_length);

        if (pairs.empty()) {
            // No more pairs to merge, but still not l-EDS
            // This can happen if degenerate symbols prevent further merging
            break;
        }

        // Create temp output files for this iteration
        std::filesystem::path temp_eds_out = temp_dir / ("iter_" + std::to_string(iteration) + ".eds");
        std::filesystem::path temp_seds_out = temp_dir / ("iter_" + std::to_string(iteration) + ".seds");

        std::ofstream eds_out_stream(temp_eds_out);
        std::ofstream seds_out_stream;

        if (!eds_out_stream) {
            throw std::runtime_error("Failed to create temp output file: " + temp_eds_out.string());
        }

        if (has_sources) {
            seds_out_stream.open(temp_seds_out);
            if (!seds_out_stream) {
                throw std::runtime_error("Failed to create temp sources output file: " + temp_seds_out.string());
            }
        }

        // Process pairs in batches to control parallel memory usage
        for (size_t batch_start = 0; batch_start < pairs.size(); batch_start += BATCH_SIZE) {
            size_t batch_end = std::min(batch_start + BATCH_SIZE, pairs.size());
            std::vector<MergePair> batch_pairs(
                pairs.begin() + batch_start,
                pairs.begin() + batch_end
            );

            // Compute merge metadata (NO string data, minimal memory)
            auto batch_metadata = compute_merge_metadata(eds, batch_pairs, num_threads);

            // Stream output directly to file (immediate flush, no accumulation)
            stream_merged_symbols_to_file(
                eds,
                batch_metadata,
                eds_out_stream,
                has_sources ? &seds_out_stream : nullptr
            );

            // batch_metadata freed here automatically (RAII)
        }

        eds_out_stream.close();
        if (has_sources) {
            seds_out_stream.close();
        }

        // Replace EDS with temp file (still METADATA_ONLY mode)
        // This keeps memory footprint constant across iterations
        eds = has_sources
            ? EDS::load(temp_eds_out, temp_seds_out, EDS::StoringMode::METADATA_ONLY)
            : EDS::load(temp_eds_out, EDS::StoringMode::METADATA_ONLY);

        // Update file pointers for cleanup
        current_eds_file = temp_eds_out;
        current_seds_file = temp_seds_out;

        iteration++;
    }

    if (iteration >= MAX_ITERATIONS) {
        throw std::runtime_error("Maximum iterations reached without convergence");
    }

    // Copy final result to output streams
    {
        std::ifstream final_eds(current_eds_file);
        if (!final_eds) {
            throw std::runtime_error("Failed to open final EDS file: " + current_eds_file.string());
        }
        output << final_eds.rdbuf();
    }

    if (phasing_output && has_sources) {
        std::ifstream final_seds(current_seds_file);
        if (!final_seds) {
            throw std::runtime_error("Failed to open final sources file: " + current_seds_file.string());
        }
        *phasing_output << final_seds.rdbuf();
    }

    // Cleanup temp directory
    try {
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception& e) {
        // Non-fatal: temp cleanup failed, but transformation succeeded
        std::cerr << "Warning: Failed to cleanup temp directory " << temp_dir << ": " << e.what() << "\n";
    }
}

/**
 * Convert EDS to l-EDS using cartesian merging.
 *
 * Similar to linear merging but uses cartesian product (ignores phasing).
 * Cannot be used with source files.
 */
void eds_to_leds_cartesian(
    std::istream& input,
    std::ostream& output,
    Length context_length,
    size_t num_threads,
    bool compact
) {
    if (context_length == 0) {
        throw std::invalid_argument("context_length must be > 0 for l-EDS transformation");
    }

    // ===== STREAMING ARCHITECTURE FOR MEMORY STABILITY =====
    // This implementation uses METADATA_ONLY mode with temp files to handle 100GB+ files
    // Same architecture as linear mode, but without source handling

    // Create temp directory for iteration files
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "edsparser_leds_cart";
    std::filesystem::create_directories(temp_dir);

    // Save input stream to temp file (needed for METADATA_ONLY loading)
    std::filesystem::path temp_input = temp_dir / "input.eds";
    {
        std::ofstream temp_out(temp_input);
        if (!temp_out) {
            throw std::runtime_error("Failed to create temp input file: " + temp_input.string());
        }
        temp_out << input.rdbuf();
        temp_out.close();
    }

    // Load as METADATA_ONLY (only ~100MB for 100GB file!)
    EDS eds = EDS::load(temp_input, EDS::StoringMode::METADATA_ONLY);

    if (eds.has_sources()) {
        throw std::invalid_argument("Cartesian mode cannot be used with source files");
    }

    // Iterative merging until convergence
    size_t iteration = 0;
    const size_t MAX_ITERATIONS = 10000;  // Safety limit
    const size_t BATCH_SIZE = 1000;  // Process 1000 pairs per batch to control parallel memory

    std::filesystem::path current_eds_file = temp_input;

    while (iteration < MAX_ITERATIONS) {
        // Check convergence (metadata-only operation)
        if (is_leds(eds, context_length)) {
            break;
        }

        // Select independent pairs to merge (metadata-only operation)
        auto pairs = select_independent_merge_pairs(eds, context_length);

        if (pairs.empty()) {
            break;
        }

        // Create temp output file for this iteration
        std::filesystem::path temp_eds_out = temp_dir / ("iter_" + std::to_string(iteration) + ".eds");
        std::ofstream eds_out_stream(temp_eds_out);

        if (!eds_out_stream) {
            throw std::runtime_error("Failed to create temp output file: " + temp_eds_out.string());
        }

        // Process pairs in batches to control parallel memory usage
        for (size_t batch_start = 0; batch_start < pairs.size(); batch_start += BATCH_SIZE) {
            size_t batch_end = std::min(batch_start + BATCH_SIZE, pairs.size());
            std::vector<MergePair> batch_pairs(
                pairs.begin() + batch_start,
                pairs.begin() + batch_end
            );

            // Compute merge metadata (NO string data, minimal memory)
            auto batch_metadata = compute_merge_metadata(eds, batch_pairs, num_threads);

            // Stream output directly to file (immediate flush, no accumulation)
            stream_merged_symbols_to_file(
                eds,
                batch_metadata,
                eds_out_stream,
                nullptr  // No sources in cartesian mode
            );

            // batch_metadata freed here automatically (RAII)
        }

        eds_out_stream.close();

        // Replace EDS with temp file (still METADATA_ONLY mode)
        // This keeps memory footprint constant across iterations
        eds = EDS::load(temp_eds_out, EDS::StoringMode::METADATA_ONLY);

        // Update file pointer for cleanup
        current_eds_file = temp_eds_out;

        iteration++;
    }

    if (iteration >= MAX_ITERATIONS) {
        throw std::runtime_error("Maximum iterations reached without convergence");
    }

    // Copy final result to output stream
    {
        std::ifstream final_eds(current_eds_file);
        if (!final_eds) {
            throw std::runtime_error("Failed to open final EDS file: " + current_eds_file.string());
        }
        output << final_eds.rdbuf();
    }

    // Cleanup temp directory
    try {
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception& e) {
        // Non-fatal: temp cleanup failed, but transformation succeeded
        std::cerr << "Warning: Failed to cleanup temp directory " << temp_dir << ": " << e.what() << "\n";
    }
}

/**
 * Check if EDS satisfies l-EDS property.
 *
 * An EDS is an l-EDS if:
 * 1. All internal common blocks have length >= l
 * 2. No two adjacent degenerate symbols (implicit empty common block)
 *
 * @param eds The EDS to check
 * @param context_length Minimum context length l
 * @return true if eds is an l-EDS
 */
bool is_leds(const EDS& eds, Length context_length) {
    if (context_length == 0) {
        return true;  // Every EDS is a 0-EDS
    }

    const auto& is_degenerate = eds.get_is_degenerate();

    // Check all positions
    for (size_t i = 0; i < eds.length(); ++i) {
        if (!is_degenerate[i]) {
            // This is a common block - get its length
            size_t global_idx = eds.get_metadata().cum_set_sizes[i];
            Length len = eds.get_string_length(global_idx);

            // Internal common blocks must have length >= context_length
            // Exception: First and last positions can be shorter
            if (i > 0 && i < eds.length() - 1 && len < context_length) {
                return false;
            }
        }

        // Check for adjacent degenerate symbols (implicit empty common block)
        // Note: Edge case exemption applies only to common blocks, not degenerate symbols
        if (i + 1 < eds.length() && is_degenerate[i] && is_degenerate[i + 1]) {
            return false;  // Two adjacent degenerate = implicit {} with length 0 < context_length
        }
    }

    return true;
}

} // namespace edsparser
