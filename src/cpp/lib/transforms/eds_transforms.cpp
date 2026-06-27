#include "eds_transforms.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <unordered_map>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace edsparser {

namespace {
    /**
     * RAII cleanup for the per-process temp directory used by the l-EDS merge
     * loop. Removes the directory (and all iteration files inside it) on every
     * scope exit — normal return or exception. Without this, a mid-transform
     * throw (e.g. an empty source intersection, or an I/O error) would strand
     * the iteration temp files, which can total hundreds of GB for 100GB+ inputs.
     * Uses the std::error_code overload so the destructor never throws.
     */
    struct TempDirGuard {
        std::filesystem::path dir;
        ~TempDirGuard() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };

    /**
     * Reason why a pair of adjacent positions needs to be merged
     *
     * ADJACENT_DEGENERATE : both symbols are degenerate (implicit empty common block)
     * SHORT_COMMON_LEFT   : pos1 is a short common block being absorbed into pos2 (degen on right)
     * SHORT_COMMON_RIGHT  : pos2 is a short common block being absorbed into pos1 (degen on left)
     * SHORT_COMMON_BOTH   : both pos1 and pos2 are short common blocks
     * ADJACENT_COMMON     : both symbols are non-degenerate (regardless of length).
     *                       Two adjacent common blocks are a non-canonical form:
     *                       {AB}{CD} is semantically identical to {ABCD}, and in
     *                       compact output both lose their brackets and serialise
     *                       as the single run "ABCD" — re-parsing then yields one
     *                       symbol, not two, so the EDS cardinality silently drops
     *                       below the SEDS cardinality. Coalescing them here keeps
     *                       EDS/SEDS consistent and makes compact output lossless.
     */
    enum class MergeReason {
        ADJACENT_DEGENERATE,
        SHORT_COMMON_LEFT,
        SHORT_COMMON_RIGHT,
        SHORT_COMMON_BOTH,
        ADJACENT_COMMON,
    };

    /**
     * Represents a pair of adjacent positions to merge
     */
    struct MergePair {
        size_t pos1;
        size_t pos2;
        MergeReason reason;

        MergePair(size_t p1, size_t p2, MergeReason r)
            : pos1(p1), pos2(p2), reason(r) {}
    };

    /**
     * Result of merging a pair of positions (OLD: for backward compatibility)
     */
    struct MergeResult {
        size_t original_pos1;
        size_t original_pos2;
        StringSet merged_set;
        std::vector<PathSet> merged_sources;  // Empty if no sources
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
        std::vector<PathSet> merged_sources;  // Empty if no sources
        std::vector<std::pair<size_t, size_t>> valid_indices;  // (i,j) pairs for LINEAR merge
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
        const auto& is_degenerate = eds.get_metadata().is_degenerate;

        // Track which positions are already included in pairs
        std::vector<bool> used(eds.length(), false);

        // Greedy left-to-right selection
        for (size_t i = 0; i + 1 < eds.length(); ++i) {
            if (used[i] || used[i + 1]) {
                continue;  // Position already in a pair
            }

            // Merge when it fixes an l-EDS violation OR removes a non-canonical
            // adjacent-common pair. Cases to merge:
            // 1. Internal common block with length < l
            // 2. Two adjacent degenerate symbols (implicit empty common block)
            // 3. Two adjacent common blocks (non-canonical; lossy in compact form)
            bool should_merge = false;

            bool left_short = false;   // pos i is a short common block
            bool right_short = false;  // pos i+1 is a short common block
            bool adj_degen = false;    // both are degenerate
            // Both non-degenerate: coalesce regardless of length. An EDS in
            // canonical form never has two adjacent common blocks; leaving them
            // separate corrupts compact serialisation (see MergeReason docs).
            bool adj_common = (!is_degenerate[i] && !is_degenerate[i + 1]);

            // Check if position i is an internal common block that's too short
            if (!is_degenerate[i] && i > 0 && i < eds.length() - 1) {
                size_t global_idx1 = eds.get_metadata().cum_set_sizes[i];
                Length len1 = eds.get_string_length(global_idx1);
                if (len1 < context_length) {
                    left_short = true;
                }
            }

            // Check if position i+1 is an internal common block that's too short
            if (!is_degenerate[i + 1] && (i + 1) > 0 && (i + 1) < eds.length() - 1) {
                size_t global_idx2 = eds.get_metadata().cum_set_sizes[i + 1];
                Length len2 = eds.get_string_length(global_idx2);
                if (len2 < context_length) {
                    right_short = true;
                }
            }

            // Check if both positions are degenerate (implicit empty common)
            if (is_degenerate[i] && is_degenerate[i + 1]) {
                adj_degen = true;
            }

            should_merge = left_short || right_short || adj_degen || adj_common;

            if (should_merge) {
                MergeReason reason;
                if (adj_degen)
                    reason = MergeReason::ADJACENT_DEGENERATE;
                else if (adj_common)
                    // Both common — subsumes the both-short case; merge regardless
                    // of length to keep the canonical (no-adjacent-commons) form.
                    reason = MergeReason::ADJACENT_COMMON;
                else if (left_short)
                    // i is a short common, i+1 is degenerate.
                    reason = MergeReason::SHORT_COMMON_LEFT;
                else
                    // i is degenerate, i+1 is a short common.
                    reason = MergeReason::SHORT_COMMON_RIGHT;

                pairs.emplace_back(i, i + 1, reason);
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

        // Hoist invariants out of the per-pair lambda.
        const bool has_sources = eds.has_sources();
        const auto& metadata   = eds.get_metadata();

        // --- Source preloading (LINEAR mode + parallel only) ---
        //
        // When num_threads > 1, every read_source() call inside the parallel
        // region acquires Sources::io_mutex_, serialising all workers onto a
        // single lock — making --threads effectively useless for LINEAR mode.
        //
        // Fix: read every source set we need in a single-threaded pass *before*
        // the parallel region and store the results in a flat hash map keyed by
        // global string index.  Workers then read from preloaded (no mutex).
        // unordered_map read-only access is safe under concurrent reads.
        std::unordered_map<size_t, PathSet> preloaded;
#ifdef _OPENMP
        if (has_sources && num_threads > 1 && !pairs.empty()) {
            // Reserve to avoid rehashing during sequential population.
            size_t total_strings = 0;
            for (const auto& p : pairs)
                total_strings += metadata.symbol_sizes[p.pos1] + metadata.symbol_sizes[p.pos2];
            preloaded.reserve(total_strings);

            for (const auto& p : pairs) {
                size_t idx1 = metadata.cum_set_sizes[p.pos1], sz1 = metadata.symbol_sizes[p.pos1];
                size_t idx2 = metadata.cum_set_sizes[p.pos2], sz2 = metadata.symbol_sizes[p.pos2];
                for (size_t i = 0; i < sz1; ++i) preloaded.emplace(idx1 + i, eds.read_source(idx1 + i));
                for (size_t j = 0; j < sz2; ++j) preloaded.emplace(idx2 + j, eds.read_source(idx2 + j));
            }
        }
#endif

        // Lambda: uses preloaded map when populated (parallel path, lock-free),
        // or falls back to eds.read_source() (sequential path, LRU cache + mutex).
        auto compute_pair_metadata = [&](const MergePair& pair) -> MergeMetadata {
            MergeMetadata result;
            result.original_pos1 = pair.pos1;
            result.original_pos2 = pair.pos2;

            size_t global_string_idx1 = metadata.cum_set_sizes[pair.pos1];
            size_t global_string_idx2 = metadata.cum_set_sizes[pair.pos2];
            size_t set1_size = metadata.symbol_sizes[pair.pos1];
            size_t set2_size = metadata.symbol_sizes[pair.pos2];

            if (!has_sources) {
                // CARTESIAN merge: size is product of set sizes
                result.merged_size = set1_size * set2_size;

                result.merged_string_lengths.reserve(result.merged_size);
                result.valid_indices.reserve(result.merged_size);
                for (size_t i = 0; i < set1_size; ++i) {
                    Length len1 = metadata.string_lengths[global_string_idx1 + i];
                    for (size_t j = 0; j < set2_size; ++j) {
                        Length len2 = metadata.string_lengths[global_string_idx2 + j];
                        result.merged_string_lengths.push_back(len1 + len2);
                        result.valid_indices.push_back({i, j});
                    }
                }
            } else {
                // LINEAR merge: only keep valid combinations (non-empty source intersection).

                // Read sources from preloaded (no mutex) when available, otherwise
                // fall back to eds.read_source() which holds Sources::io_mutex_.
                auto get_src = [&](size_t idx) -> PathSet {
                    if (!preloaded.empty()) return preloaded.at(idx);
                    return eds.read_source(idx);
                };

                std::vector<PathSet> src1(set1_size), src2(set2_size);
                for (size_t i = 0; i < set1_size; ++i) src1[i] = get_src(global_string_idx1 + i);
                for (size_t j = 0; j < set2_size; ++j) src2[j] = get_src(global_string_idx2 + j);

                // Bitset fast path: if all path IDs fit in [1, 63], represent each
                // source set as a uint64_t bitmask.  Intersection = bitwise AND: O(1).
                // PathSet is sorted, so max element is at back() — O(1) check per set.
                bool use_bits = true;
                for (size_t i = 0; i < set1_size && use_bits; ++i)
                    if (!src1[i].empty() && src1[i].back() > 63) use_bits = false;
                for (size_t j = 0; j < set2_size && use_bits; ++j)
                    if (!src2[j].empty() && src2[j].back() > 63) use_bits = false;

                auto to_bits = [](const PathSet& s) -> uint64_t {
                    if (!s.empty() && s[0] == 0) return ~0ULL;
                    uint64_t b = 0;
                    for (int id : s) b |= (1ULL << (id - 1));
                    return b;
                };
                // Produces a sorted PathSet (k increases 0..62 → IDs 1..63 in order).
                auto bits_to_set = [](uint64_t b) -> PathSet {
                    if (b == ~0ULL) return {0};
                    PathSet s;
                    for (int k = 0; k < 63; ++k)
                        if (b & (1ULL << k)) s.push_back(k + 1);
                    return s;
                };

                std::vector<uint64_t> bits1, bits2;
                if (use_bits) {
                    bits1.resize(set1_size); bits2.resize(set2_size);
                    for (size_t i = 0; i < set1_size; ++i) bits1[i] = to_bits(src1[i]);
                    for (size_t j = 0; j < set2_size; ++j) bits2[j] = to_bits(src2[j]);
                }

                result.merged_sources.reserve(set1_size * set2_size);
                result.merged_string_lengths.reserve(set1_size * set2_size);
                result.valid_indices.reserve(set1_size * set2_size);

                for (size_t i = 0; i < set1_size; ++i) {
                    Length len1 = metadata.string_lengths[global_string_idx1 + i];
                    for (size_t j = 0; j < set2_size; ++j) {
                        Length len2 = metadata.string_lengths[global_string_idx2 + j];
                        if (use_bits) {
                            uint64_t a = bits1[i], b = bits2[j];
                            uint64_t isect = (a == ~0ULL) ? b : (b == ~0ULL) ? a : a & b;
                            if (isect == 0) continue;
                            result.merged_sources.push_back(bits_to_set(isect));
                        } else {
                            PathSet isect = Sources::intersect_sources(src1[i], src2[j]);
                            if (isect.empty()) continue;
                            result.merged_sources.push_back(std::move(isect));
                        }
                        result.merged_string_lengths.push_back(len1 + len2);
                        result.valid_indices.push_back({i, j});
                    }
                }

                result.merged_size = result.merged_sources.size();

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
            for (size_t i = 0; i < pairs.size(); ++i) {
                results[i] = compute_pair_metadata(pairs[i]);
            }
        } else {
#ifdef _OPENMP
            // Exceptions must not propagate out of a parallel region (UB in OpenMP).
            // Each thread catches locally; the first exception is re-thrown after join.
            std::exception_ptr first_exception;
            std::atomic<bool> exception_occurred{false};
            #pragma omp parallel for num_threads(num_threads)
            for (size_t i = 0; i < pairs.size(); ++i) {
                if (exception_occurred.load(std::memory_order_relaxed)) continue;
                try {
                    results[i] = compute_pair_metadata(pairs[i]);
                } catch (...) {
                    #pragma omp critical
                    if (!first_exception) {
                        first_exception = std::current_exception();
                        exception_occurred.store(true, std::memory_order_relaxed);
                    }
                }
            }
            if (first_exception) std::rethrow_exception(first_exception);
#else
            for (size_t i = 0; i < pairs.size(); ++i) {
                results[i] = compute_pair_metadata(pairs[i]);
            }
#endif
        }

        return results;
    }

    /**
     * Result of streaming merged symbols to file.
     * Contains the complete EDS::Metadata for the output file, built
     * as a byproduct of the write — so the caller can skip re-reading
     * the file to rebuild metadata for the next iteration.
     *
     * Requires eds_out to be a seekable stream (std::ofstream on a real file).
     */
    struct StreamResult {
        EDS::Metadata metadata;
        size_t n = 0;   // total symbols written
        size_t m = 0;   // total strings written
        size_t N = 0;   // total characters written
    };

    /**
     * Stream merged EDS symbols directly to file WITHOUT accumulating in memory.
     * This is the memory-efficient version that works with METADATA_ONLY mode.
     *
     * Strategy: Read symbols on-demand, merge strings on-the-fly, write immediately.
     * No ostringstream accumulation - direct file writes prevent memory growth.
     *
     * Also builds EDS::Metadata for the output file inline, so the caller can
     * construct the next EDS object without a second parse of the written file.
     *
     * @param input_eds   The original EDS (can be METADATA_ONLY mode)
     * @param merge_metadata Vector of metadata-only merge results
     * @param eds_out     Output stream for EDS data (must be seekable)
     * @param sources_out Output stream for sources data (nullptr if no sources)
     * @return StreamResult containing metadata for the written file
     */
// ─────────────────────────────────────────────────────────────────────────────
// stream_merged_symbols_to_file — write one iteration's EDS (and SEDS) output
// ─────────────────────────────────────────────────────────────────────────────
//
// BIG PICTURE — where this function sits in the overall pipeline
//
//   eds_to_leds_linear() iteratively transforms an EDS into an l-EDS by
//   merging adjacent symbols that violate the l-context rule.  Each iteration
//   looks like this:
//
//     1. select_independent_merge_pairs() — find which pairs to merge (metadata only)
//     2. compute_merge_metadata()         — compute what the merged symbols look like
//     3. THIS FUNCTION                    — write the new EDS (and SEDS if applicable)
//     4. EDS::from_metadata()             — create a new EDS object pointing at the
//                                          temp file just written, using the metadata
//                                          returned by this function (no re-parse)
//
//   This function is the single place where I/O happens for a complete iteration.
//   All symbols (merged and unmodified alike) are written exactly once.
//
// INPUTS
//
//   input_eds       The EDS from the previous iteration (or the original input).
//                   Loaded in METADATA_ONLY mode: only the index is in RAM; the
//                   actual character data is read on demand from the temp file.
//
//   merge_metadata  A vector of MergeMetadata structs, one per merge pair.  Each
//                   struct contains:
//                     • original_pos1, original_pos2 — the two positions to merge
//                     • merged_size                  — number of strings in the result
//                     • merged_string_lengths        — length of each result string
//                     • valid_indices                — list of (i, j) pairs giving
//                       which alternative from symbol1 concatenates with which from
//                       symbol2 (for CARTESIAN: all pairs; for LINEAR: only pairs
//                       whose source sets intersect non-trivially)
//                     • merged_sources               — for each valid (i,j) pair,
//                       the resulting source set (intersection of the two inputs)
//
//   eds_out         Output stream for the new EDS temp file.
//
//   sources_out     Output stream for the new SEDS temp file, or nullptr if this
//                   is a cartesian transform (no source tracking).
//
// OUTPUTS
//
//   Returns a StreamResult containing:
//     • metadata   — a fully populated EDS::Metadata struct describing the output
//                    file, built *inline* as we write.  This lets the caller create
//                    the next EDS object via EDS::from_metadata() without re-reading
//                    the file.
//     • n          — symbol count in the output
//     • m          — total string count (sum of all symbol sizes)
//     • N          — total character count
//
// THE MERGE MAP — how we navigate merged vs unmodified positions
//
//   Before entering the main loop we build two lookup tables over the input
//   positions [0, input_eds.length()):
//
//   merge_map[pos]:  if pos is the *first* position of a merge pair, this holds
//                    the index into merge_metadata[]; otherwise -1.
//
//   skip[pos]:       true if pos is the *second* position of a merge pair (i.e.
//                    it has been absorbed into its predecessor and should produce
//                    no output symbol of its own).
//
//   Every position falls into exactly one of three categories:
//     • skip[pos] == true           → consumed; produce no output, skip.
//     • merge_map[pos] >= 0         → first of a merge pair; produce one merged
//                                    output symbol by concatenating pos and pos+1.
//     • otherwise (both false)      → unmodified; copy symbol verbatim.
//
// THE SEDS BATCHING OPTIMISATION
//
//   When source tracking is enabled, for each output symbol we must write the
//   corresponding source sets to `sources_out`.  For unmodified symbols the
//   source data exists unchanged in the *input* SEDS file; we need only copy
//   the raw bytes.  For merged symbols the new source sets are the computed
//   intersection sets from merge_metadata, and we serialise them character by
//   character.
//
//   The key insight: consecutive unmodified symbols have consecutive source
//   entries in the input SEDS file.  If positions p, p+1, p+2 are all
//   unmodified, their SEDS entries sit at
//     base_positions_[cum_set_sizes[p]]  …  base_positions_[cum_set_sizes[p+3]]
//   — a single contiguous byte range.  Instead of calling copy_range_to_stream()
//   once per symbol (one seek + one tiny read each time), we can accumulate the
//   range across the entire run of consecutive unmodified symbols and issue a
//   single call at the boundary.
//
//   Concretely, the pattern is:
//     for each unmodified pos:
//         extend the pending batch by sym_size string entries
//     when a merged pos is encountered (or the loop ends):
//         flush the batch: copy_range_to_stream(batch_start, batch_count, out)
//         reset batch state
//
//   With 10% variability and 2 727 merge pairs per iteration, the number of
//   copy_range_to_stream() calls drops from ~200 000 (one per symbol) to ~2 727
//   (one per merge boundary).  Combined with copy_range_to_stream()'s own
//   sequential seek elimination, this cuts SEDS I/O from ~400 000 syscalls to
//   ~5 000 syscalls per iteration.
//
// METADATA TRACKING — building the next iteration's index inline
//
//   As we write each output symbol we simultaneously build the EDS::Metadata
//   struct for the output file.  The key fields:
//
//   base_positions[]     byte offset of each symbol's '{' in eds_out.
//                        Captured via eds_out.tellp() just before writing '{'.
//
//   symbol_sizes[]       number of alternatives per output symbol.
//
//   string_lengths[]     character length of each individual string alternative.
//                        For unmodified symbols: copied from input metadata.
//                        For merged symbols: taken from merge_meta.merged_string_lengths.
//
//   cum_set_sizes[]      cumulative count of strings before each symbol (prefix sum
//                        of symbol_sizes); used to find the global string index for
//                        a given symbol position.
//
//   is_degenerate[]      true if sym_size > 1.
//
//   cum_common_positions[], cum_degenerate_counts[]:
//                        per-symbol cumulative position counters used by
//                        locate() in the index.
//
//   min/max/avg_context_length, num_degenerate_symbols, etc.:
//                        aggregate statistics for --verbose output and compliance
//                        checking.
//
//   Because all these values are derived from the symbol we are writing right
//   now (not from re-reading the output file), this inline metadata building
//   avoids a costly re-parse pass after writing, matching the "Sources still
//   require a re-read" note in eds_to_leds_linear().
//
// ─────────────────────────────────────────────────────────────────────────────
    StreamResult stream_merged_symbols_to_file(
        const EDS& input_eds,
        const std::vector<MergeMetadata>& merge_metadata,
        std::ostream& eds_out,
        std::ostream* sources_out
    ) {
        // ── Build the merge/skip lookup tables ───────────────────────────────
        // merge_map[pos] = index into merge_metadata if pos is the leading
        //                  position of a merge pair, else -1.
        // skip[pos]      = true if pos is the trailing position of a merge pair
        //                  (it has been absorbed and produces no output).
        std::vector<int> merge_map(input_eds.length(), -1);
        std::vector<bool> skip(input_eds.length(), false);

        for (size_t i = 0; i < merge_metadata.size(); ++i) {
            merge_map[merge_metadata[i].original_pos1] = static_cast<int>(i);
            skip[merge_metadata[i].original_pos2] = true;
        }

        bool has_sources = input_eds.has_sources();
        const auto& in_meta = input_eds.get_metadata();  // input index, read-only

        // ── Initialise the output metadata accumulator ───────────────────────
        // min_context_length starts at UINT32_MAX so the first real context
        // length (which may be small) correctly replaces it via std::min logic.
        StreamResult result;
        result.metadata.min_context_length = UINT32_MAX;
        result.metadata.max_context_length = 0;
        result.metadata.num_degenerate_symbols = 0;
        result.metadata.num_common_chars = 0;
        result.metadata.total_change_size = 0;
        result.metadata.num_empty_strings = 0;
        size_t total_context_length = 0;   // used to compute avg at the end
        size_t num_context_blocks = 0;     // number of non-degenerate symbols seen
        Position cumulative_common = 0;    // cumulative non-degenerate char count
        int cumulative_degenerate = 0;     // cumulative degenerate string count

        // The cum_* arrays have one extra leading entry at index 0 representing
        // the state *before* any symbol.  The entry for symbol i is pushed at
        // the *end* of processing symbol i, so after the loop these arrays have
        // length n+1 (one per symbol, plus the sentinel).
        result.metadata.cum_common_positions.push_back(0);
        result.metadata.cum_degenerate_counts.push_back(0);

        // ── SEDS batch state ─────────────────────────────────────────────────
        // We accumulate consecutive unmodified symbols' source index ranges here
        // instead of calling copy_range_to_stream() once per symbol.
        // seds_batch_start  = global string index (into the input SEDS file) of
        //                     the first string in the current pending batch.
        // seds_batch_count  = total number of strings accumulated so far.
        //
        // When seds_batch_count > 0 there is a pending batch that has not yet
        // been written to sources_out.  The batch covers string indices
        //   [seds_batch_start, seds_batch_start + seds_batch_count).
        size_t seds_batch_start = 0;
        size_t seds_batch_count = 0;

        // flush_seds_batch() is called:
        //   (a) just before writing a merged symbol — the merged symbol's own
        //       SEDS data is written inline, not via copy_range_to_stream(), so
        //       we must first flush whatever unmodified data preceded it.
        //   (b) at the very end of the loop, after the last symbol, to drain
        //       any trailing batch that was never terminated by a merge.
        //
        // If seds_batch_count is 0 there is nothing to flush; the guard avoids
        // an unnecessary function call.
        auto flush_seds_batch = [&]() {
            if (has_sources && sources_out && seds_batch_count > 0) {
                // Write all accumulated source entries in one I/O call.
                // copy_range_to_stream() uses the exact-byte-range fast path
                // when start_idx + count < base_positions_.size(), reading
                // precisely the needed bytes and leaving the SEDS stream
                // positioned at the start of the next entry.
                input_eds.get_sources_object()->copy_range_to_stream(
                    seds_batch_start, seds_batch_count, *sources_out);
                seds_batch_count = 0;  // reset; seds_batch_start is overwritten on next use
            }
        };

        // ════════════════════════════════════════════════════════════════════
        // MAIN LOOP — iterate over every input position in order
        // ════════════════════════════════════════════════════════════════════
        for (size_t pos = 0; pos < input_eds.length(); ++pos) {

            // ── Skip absorbed positions ──────────────────────────────────────
            // When position pos+1 is the second element of a merge pair that
            // starts at pos, it is marked skip[pos+1]=true.  The merged output
            // for that pair has already been written when we processed pos, so
            // pos+1 must produce no output at all.
            if (skip[pos]) {
                continue;
            }

            // ── Record the output file position for this symbol ──────────────
            // eds_out.tellp() returns the current write position in the output
            // stream, which is the byte offset where the '{' we are about to
            // write will land.  We store this so the EDS object created from the
            // output file (via EDS::from_metadata()) knows where to seekg() to
            // find this symbol.
            auto base_pos = static_cast<std::streampos>(eds_out.tellp());
            result.metadata.base_positions.push_back(base_pos);

            // cum_set_sizes[i] = total number of strings in symbols 0..i-1.
            // We push result.m (the running total so far) before updating it.
            result.metadata.cum_set_sizes.push_back(result.m);

            size_t sym_size;  // will be set in either branch below

            // ════════════════════════════════════════════════════════════════
            // BRANCH A: MERGED POSITION
            // The pre-computed merge plan says to fuse symbols pos and pos+1.
            // ════════════════════════════════════════════════════════════════
            if (merge_map[pos] >= 0) {

                // Before writing this merged symbol's SEDS data, flush whatever
                // unmodified SEDS data has been accumulating in the batch.
                // The output SEDS file must contain entries in the same order as
                // the EDS symbols they describe, so the batch for symbols before
                // this merge must be written before the merge's own entries.
                flush_seds_batch();

                const auto& merge_meta = merge_metadata[merge_map[pos]];
                sym_size = merge_meta.merged_size;  // number of output alternatives

                // ── String lengths: from metadata, no I/O ───────────────────
                // merge_meta.merged_string_lengths holds the pre-computed length
                // of each output string: len(set1[i]) + len(set2[j]) for each
                // valid (i,j) pair.  We record them here for the output metadata
                // and accumulate the total character count result.N.
                for (Length len : merge_meta.merged_string_lengths) {
                    result.metadata.string_lengths.push_back(len);
                    result.N += len;
                    if (len == 0) result.metadata.num_empty_strings++;
                }

                // ── Read both input symbols ──────────────────────────────────
                // read_symbol() calls read_symbol_from_stream() in METADATA_ONLY
                // mode.  The sequential-seek optimisation in that function means:
                //   • The first call (for pos) may need to seek if pos was not
                //     immediately after the previous output symbol.
                //   • The second call (for pos+1) will find the stream already
                //     sitting at base_positions[pos+1] (the stream landed there
                //     after parsing pos), so it skips its own seekg().
                // Two symbols, at most one lseek.
                StringSet set1 = input_eds.read_symbol(pos);
                StringSet set2 = input_eds.read_symbol(pos + 1);

                // ── Write the merged EDS symbol ──────────────────────────────
                // Format: {concat1,concat2,...,concatK}
                // valid_indices contains the (i,j) pairs pre-selected by
                // compute_merge_metadata():
                //   CARTESIAN mode: all (i,j) in [0,|set1|) × [0,|set2|)
                //   LINEAR mode:    only (i,j) where intersect_sources(i,j) ≠ ∅
                eds_out << '{';
                bool first_string = true;

                for (size_t idx = 0; idx < merge_meta.valid_indices.size(); ++idx) {
                    auto [i, j] = merge_meta.valid_indices[idx];

                    if (!first_string) eds_out << ',';
                    // Concatenate the two strings directly into the output stream;
                    // no intermediate buffer needed.
                    eds_out << set1[i] << set2[j];
                    first_string = false;

                    // ── Write the merged SEDS entry for this output string ───
                    // The source set for the merged string is
                    //   intersect_sources(sources_of_set1[i], sources_of_set2[j])
                    // pre-computed in merge_meta.merged_sources[idx].
                    // We serialise it here as {p1,p2,...} because the byte
                    // layout of merged SEDS entries cannot be copied verbatim
                    // from the input — the merged strings are new creations.
                    if (sources_out) {
                        *sources_out << '{';
                        bool first_path = true;
                        const PathSet& merged_source = merge_meta.merged_sources[idx];
                        for (int path_id : merged_source) {
                            if (!first_path) *sources_out << ',';
                            *sources_out << path_id;
                            first_path = false;
                        }
                        *sources_out << '}';
                    }
                }

                eds_out << '}';

            } else {
                // ════════════════════════════════════════════════════════════
                // BRANCH B: UNMODIFIED POSITION
                // This symbol is not involved in any merge; write it verbatim.
                // ════════════════════════════════════════════════════════════

                sym_size = in_meta.symbol_sizes[pos];

                // global_idx is the index of this symbol's first string in the
                // flat string array (and in the SEDS file).  It is the starting
                // point for the SEDS batch we are about to extend.
                size_t global_idx = in_meta.cum_set_sizes[pos];

                // ── String lengths: from input metadata, no I/O ─────────────
                // All string lengths for this symbol are already in in_meta;
                // we copy them into the output metadata and update counters.
                for (size_t k = 0; k < sym_size; ++k) {
                    Length len = in_meta.string_lengths[global_idx + k];
                    result.metadata.string_lengths.push_back(len);
                    result.N += len;
                    if (len == 0) result.metadata.num_empty_strings++;
                }

                // ── Read and re-write the EDS symbol ────────────────────────
                // read_symbol() triggers read_symbol_from_stream() in
                // METADATA_ONLY mode.  Because we process positions in order
                // (0, 1, 2, …) and the input file has symbols in the same order,
                // the sequential-seek guard in read_symbol_from_stream() fires
                // for nearly every call: after reading symbol pos the stream
                // sits at base_positions[pos+1], so the next call for pos+1
                // skips its seekg().
                // Exception: after a merged pair, skip[pos+1] fires and the
                // loop jumps to pos+2; the stream is at base_positions[pos+1]
                // but we want base_positions[pos+2], so one seek is needed.
                StringSet symbol = input_eds.read_symbol(pos);

                // Write in full-bracket format {str1,str2,...}.  Intermediate
                // temp files always use full-bracket format (required for
                // METADATA_ONLY seeking) even for non-degenerate symbols.
                // The compact serialisation (bare string without brackets for
                // non-degenerate symbols) is applied only to the final output
                // in eds_to_leds_linear(), not here.
                eds_out << '{';
                for (size_t i = 0; i < symbol.size(); ++i) {
                    if (i > 0) eds_out << ',';
                    eds_out << symbol[i];
                }
                eds_out << '}';

                // ── Extend the SEDS batch ────────────────────────────────────
                // Rather than calling copy_range_to_stream() immediately (one
                // seek + one tiny read per symbol), we record the range in the
                // batch accumulators.  This batch will be flushed in one call
                // when we either encounter a merged symbol or reach the end of
                // the loop.
                //
                // Invariant: consecutive unmodified symbols have consecutive
                // SEDS entries (cum_set_sizes is strictly increasing and the
                // SEDS file mirrors the string order of the EDS file), so the
                // batch always represents a single contiguous byte range in the
                // input SEDS.  copy_range_to_stream() can therefore copy it
                // with a single seek + a single sequential read.
                if (has_sources && sources_out) {
                    if (seds_batch_count == 0) {
                        // Start of a new batch: record where in the SEDS file
                        // this run of unmodified symbols begins.
                        seds_batch_start = global_idx;
                    }
                    // Extend the batch by this symbol's string count.
                    // After the loop, seds_batch_start to seds_batch_start +
                    // seds_batch_count covers all the accumulated unmodified
                    // strings.
                    seds_batch_count += sym_size;
                }
            }

            // ── Update per-symbol metadata statistics ────────────────────────
            // A symbol is "degenerate" (a variant set) if it has more than one
            // alternative string.  A non-degenerate symbol is a context block
            // with exactly one string.
            bool is_deg = (sym_size > 1);
            result.metadata.symbol_sizes.push_back(static_cast<Length>(sym_size));
            result.metadata.is_degenerate.push_back(is_deg);

            if (is_deg) {
                result.metadata.num_degenerate_symbols++;
                // total_change_size counts the number of *extra* alternatives
                // beyond the first: a symbol with 4 alternatives contributes 3.
                result.metadata.total_change_size += (sym_size - 1);
                cumulative_degenerate += static_cast<int>(sym_size);
            } else {
                // Non-degenerate: update context-length statistics.
                // ctx_len is the length of the single string in this symbol.
                Length ctx_len = result.metadata.string_lengths.back();
                result.metadata.num_common_chars += ctx_len;
                total_context_length += ctx_len;
                num_context_blocks++;
                cumulative_common += static_cast<Position>(ctx_len);
                if (ctx_len < result.metadata.min_context_length)
                    result.metadata.min_context_length = ctx_len;
                if (ctx_len > result.metadata.max_context_length)
                    result.metadata.max_context_length = ctx_len;
            }

            // Push the running cumulative totals — one entry per output symbol.
            // These arrays are used by the locate() implementation to map
            // positions between the EDS index and the common/changes indexes.
            result.metadata.cum_common_positions.push_back(cumulative_common);
            result.metadata.cum_degenerate_counts.push_back(cumulative_degenerate);

            result.m += sym_size;  // advance the total string counter
            result.n++;            // advance the output symbol counter
        }
        // ════════════════════════════════════════════════════════════════════
        // END OF MAIN LOOP
        // ════════════════════════════════════════════════════════════════════

        // Drain any SEDS batch that was still accumulating when the loop ended.
        // This covers the common case where the final symbols of the EDS are all
        // unmodified (no merge at the very end of the file).
        flush_seds_batch();

        // ── Finalise aggregate statistics ────────────────────────────────────
        result.metadata.avg_context_length = (num_context_blocks > 0)
            ? static_cast<double>(total_context_length) / num_context_blocks
            : 0.0;

        // If no non-degenerate symbol was ever seen, min_context_length was
        // never updated from its sentinel value UINT32_MAX.  Reset to 0 so
        // callers see a sensible value rather than an enormous integer.
        if (result.metadata.min_context_length == UINT32_MAX)
            result.metadata.min_context_length = 0;

        return result;
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
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path()
        / ("edsparser_leds_" + std::to_string(getpid()));
    std::filesystem::create_directories(temp_dir);

    // Remove temp_dir on every exit path (normal return or exception thrown
    // anywhere below), so partial iteration files are never left behind.
    TempDirGuard temp_guard{temp_dir};

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
        ? EDS::load(temp_input, temp_sources_input)
        : EDS::load(temp_input);

    // Warn when temp space requirement is non-trivial.
    // Peak usage: input copy + previous iteration + current iteration = ~3× input size.
    {
        auto input_size = std::filesystem::file_size(temp_input);
        if (input_size > 500ULL * 1024 * 1024) {  // > 500 MB
            auto gb = [](uintmax_t b) { return b / double(1ULL << 30); };
            std::cerr << "[l-EDS] Temp disk needed: ~" << std::fixed << std::setprecision(1)
                      << gb(input_size * 3) << " GB in " << temp_dir << "\n";
        }
    }

    // ===== COMPLEXITY ESTIMATION =====
    // Warn users about potentially slow transformations BEFORE starting
    auto complexity = estimate_leds_complexity(eds, context_length);

    if (complexity.warn_exponential || complexity.warn_slow) {
        std::cerr << "\n" << std::string(70, '=') << "\n";
        std::cerr << complexity.recommendation << "\n";
        std::cerr << std::string(70, '=') << "\n\n";
    } else {
        std::cerr << "[l-EDS] " << complexity.recommendation << "\n";
    }

    // Iterative merging until convergence
    size_t iteration = 0;
    const size_t MAX_ITERATIONS = 10000;  // Safety limit
    const size_t BATCH_SIZE = 1000;  // Process 1000 pairs per batch to control parallel memory

    std::filesystem::path current_eds_file = temp_input;
    std::filesystem::path current_seds_file = temp_sources_input;

    while (iteration < MAX_ITERATIONS) {
        // Select independent pairs to merge (metadata-only operation)
        // This checks both l-EDS conditions:
        // 1. Internal common blocks with length < context_length
        // 2. Adjacent degenerate symbols (implicit empty common block)
        auto pairs = select_independent_merge_pairs(eds, context_length);

        if (pairs.empty()) {
            // No violations - EDS satisfies l-EDS property
            std::cerr << "[l-EDS] Converged after " << iteration << " iterations\n";
            break;
        }

        // Progress header for this iteration
        size_t total_symbols = eds.length();
        size_t total_pairs = pairs.size();
        {
            size_t n_adj = 0, n_left = 0, n_right = 0, n_both = 0, n_common = 0;
            for (const auto& p : pairs) {
                switch (p.reason) {
                    case MergeReason::ADJACENT_DEGENERATE: ++n_adj;    break;
                    case MergeReason::SHORT_COMMON_LEFT:   ++n_left;   break;
                    case MergeReason::SHORT_COMMON_RIGHT:  ++n_right;  break;
                    case MergeReason::SHORT_COMMON_BOTH:   ++n_both;   break;
                    case MergeReason::ADJACENT_COMMON:     ++n_common; break;
                }
            }
            std::cerr << "[l-EDS] Iter " << iteration
                      << ": " << total_symbols << " symbols, merging " << total_pairs << " pairs";
            std::cerr << " (";
            bool first = true;
            auto sep = [&]() { if (!first) std::cerr << ", "; first = false; };
            if (n_adj)   { sep(); std::cerr << n_adj   << " adj-degen"; }
            if (n_left)  { sep(); std::cerr << n_left  << " short-ctx←left";  }
            if (n_right) { sep(); std::cerr << n_right << " short-ctx→right"; }
            if (n_both)  { sep(); std::cerr << n_both  << " short-ctx-both";  }
            if (n_common){ sep(); std::cerr << n_common<< " adj-common";      }
            std::cerr << ")\n";
        }

        // Helper: print merge-pairs progress bar in-place using \r.
        // Suppressed when stderr is not a terminal to avoid polluting log files.
        const bool stderr_tty = isatty(STDERR_FILENO);
        auto print_bar = [&](size_t done) {
            if (!stderr_tty) return;
            const int BAR_WIDTH = 40;
            float frac = total_pairs > 0 ? static_cast<float>(done) / total_pairs : 1.0f;
            int filled = static_cast<int>(BAR_WIDTH * frac);
            std::cerr << "\r  [";
            for (int i = 0; i < BAR_WIDTH; i++) {
                if (i < filled)       std::cerr << '#';
                else if (i == filled) std::cerr << '>';
                else                  std::cerr << ' ';
            }
            std::cerr << "] " << std::setw(3) << static_cast<int>(frac * 100) << "%"
                      << " (" << done << "/" << total_pairs << ")    ";
            std::cerr.flush();
        };

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

        // Compute all merge metadata first (batched for parallel memory control),
        // then stream the full result once. stream_merged_symbols_to_file iterates
        // over ALL positions, so calling it once per batch would write every
        // unmodified symbol N-times (once per batch).
        std::vector<MergeMetadata> all_metadata;
        all_metadata.reserve(pairs.size());

        for (size_t batch_start = 0; batch_start < pairs.size(); batch_start += BATCH_SIZE) {
            print_bar(batch_start);

            size_t batch_end = std::min(batch_start + BATCH_SIZE, pairs.size());
            std::vector<MergePair> batch_pairs(
                pairs.begin() + batch_start,
                pairs.begin() + batch_end
            );

            // Compute merge metadata (NO string data, minimal memory)
            auto batch_metadata = compute_merge_metadata(eds, batch_pairs, num_threads);

            // Accumulate metadata across batches
            all_metadata.insert(all_metadata.end(),
                                 std::make_move_iterator(batch_metadata.begin()),
                                 std::make_move_iterator(batch_metadata.end()));
        }
        print_bar(total_pairs);
        if (stderr_tty) std::cerr << "\n";

        // Stream full result to file once (each position written exactly once).
        // Also captures output metadata inline — avoids re-reading the file next iteration.
        auto stream_result = stream_merged_symbols_to_file(
            eds,
            all_metadata,
            eds_out_stream,
            has_sources ? &seds_out_stream : nullptr
        );

        eds_out_stream.close();
        if (has_sources) {
            seds_out_stream.close();
        }

        // Log merge outcome and new metadata state
        {
            const auto& m = stream_result.metadata;
            std::cerr << "[l-EDS]   Merged:   " << total_symbols << " → " << stream_result.n
                      << " symbols (" << (total_symbols - stream_result.n) << " consumed)"
                      << ", " << stream_result.m << " strings"
                      << ", " << stream_result.N << " chars\n";
            std::cerr << "[l-EDS]   Metadata: ctx min=" << m.min_context_length
                      << " max=" << m.max_context_length
                      << std::fixed << std::setprecision(1)
                      << " avg=" << m.avg_context_length
                      << " | degen=" << m.num_degenerate_symbols
                      << " empty=" << m.num_empty_strings
                      << " (built inline, no re-parse)\n";
        }

        // Replace EDS with temp file using pre-built metadata (no re-parse needed).
        // Sources still require a re-read (Sources has its own index-building pass).
        if (has_sources) {
            std::cerr << "[l-EDS]   Sources: re-indexing " << temp_seds_out.filename().string() << "\n";
            auto new_sources = Sources::load(temp_seds_out, Sources::Format::SEDS);
            eds = EDS::from_metadata(std::move(stream_result.metadata),
                                     stream_result.n, stream_result.m, stream_result.N,
                                     temp_eds_out);
            eds.set_sources_object(new_sources);
        } else {
            eds = EDS::from_metadata(std::move(stream_result.metadata),
                                     stream_result.n, stream_result.m, stream_result.N,
                                     temp_eds_out);
        }

        // Delete previous iteration files immediately (Linux: fd valid after unlink)
        std::filesystem::remove(current_eds_file);
        if (has_sources) {
            std::filesystem::remove(current_seds_file);
        }

        // Update file pointers
        current_eds_file = temp_eds_out;
        current_seds_file = temp_seds_out;

        iteration++;
    }

    if (iteration >= MAX_ITERATIONS) {
        throw std::runtime_error("Maximum iterations reached without convergence");
    }

    // Serialise final result to the output stream.
    // Intermediate temp files always use full-bracket format (required for METADATA_ONLY
    // seeks); the requested output format is applied only here, on the final EDS object.
    // Using read_symbol() rather than a raw rdbuf() copy ensures the format flag is
    // honoured even when the input was already l-EDS compliant (zero iterations run).
    for (size_t i = 0; i < eds.length(); ++i) {
        const StringSet sym = eds.read_symbol(i);
        bool use_brackets = !compact || sym.size() > 1;
        if (use_brackets) output << '{';
        for (size_t j = 0; j < sym.size(); ++j) {
            if (j > 0) output << ',';
            output << sym[j];
        }
        if (use_brackets) output << '}';
    }
    output << '\n';

    if (phasing_output && has_sources) {
        std::ifstream final_seds(current_seds_file);
        if (!final_seds) {
            throw std::runtime_error("Failed to open final sources file: " + current_seds_file.string());
        }
        *phasing_output << final_seds.rdbuf();
    }

    // Temp directory is removed by temp_guard (RAII) when this function returns,
    // covering both the normal path here and any exception thrown above.
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
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path()
        / ("edsparser_leds_cart_" + std::to_string(getpid()));
    std::filesystem::create_directories(temp_dir);

    // Remove temp_dir on every exit path (normal return or exception thrown
    // anywhere below), so partial iteration files are never left behind.
    TempDirGuard temp_guard{temp_dir};

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
    EDS eds = EDS::load(temp_input);

    if (eds.has_sources()) {
        throw std::invalid_argument("Cartesian mode cannot be used with source files");
    }

    {
        auto input_size = std::filesystem::file_size(temp_input);
        if (input_size > 500ULL * 1024 * 1024) {
            auto gb = [](uintmax_t b) { return b / double(1ULL << 30); };
            std::cerr << "[l-EDS] Temp disk needed: ~" << std::fixed << std::setprecision(1)
                      << gb(input_size * 3) << " GB in " << temp_dir << "\n";
        }
    }

    // Iterative merging until convergence
    size_t iteration = 0;
    const size_t MAX_ITERATIONS = 10000;  // Safety limit
    const size_t BATCH_SIZE = 1000;  // Process 1000 pairs per batch to control parallel memory

    std::filesystem::path current_eds_file = temp_input;

    while (iteration < MAX_ITERATIONS) {
        // Select independent pairs to merge (metadata-only operation)
        // This checks both l-EDS conditions:
        // 1. Internal common blocks with length < context_length
        // 2. Adjacent degenerate symbols (implicit empty common block)
        auto pairs = select_independent_merge_pairs(eds, context_length);

        if (pairs.empty()) {
            // No violations - EDS satisfies l-EDS property
            std::cerr << "[l-EDS] Converged after " << iteration << " iterations\n";
            break;
        }

        // Progress header for this iteration
        size_t total_symbols = eds.length();
        size_t total_pairs = pairs.size();
        {
            size_t n_adj = 0, n_left = 0, n_right = 0, n_both = 0, n_common = 0;
            for (const auto& p : pairs) {
                switch (p.reason) {
                    case MergeReason::ADJACENT_DEGENERATE: ++n_adj;    break;
                    case MergeReason::SHORT_COMMON_LEFT:   ++n_left;   break;
                    case MergeReason::SHORT_COMMON_RIGHT:  ++n_right;  break;
                    case MergeReason::SHORT_COMMON_BOTH:   ++n_both;   break;
                    case MergeReason::ADJACENT_COMMON:     ++n_common; break;
                }
            }
            std::cerr << "[l-EDS] Iter " << iteration
                      << ": " << total_symbols << " symbols, merging " << total_pairs << " pairs";
            std::cerr << " (";
            bool first = true;
            auto sep = [&]() { if (!first) std::cerr << ", "; first = false; };
            if (n_adj)   { sep(); std::cerr << n_adj   << " adj-degen"; }
            if (n_left)  { sep(); std::cerr << n_left  << " short-ctx←left";  }
            if (n_right) { sep(); std::cerr << n_right << " short-ctx→right"; }
            if (n_both)  { sep(); std::cerr << n_both  << " short-ctx-both";  }
            if (n_common){ sep(); std::cerr << n_common<< " adj-common";      }
            std::cerr << ")\n";
        }

        const bool stderr_tty = isatty(STDERR_FILENO);
        auto print_bar = [&](size_t done) {
            if (!stderr_tty) return;
            const int BAR_WIDTH = 40;
            float frac = total_pairs > 0 ? static_cast<float>(done) / total_pairs : 1.0f;
            int filled = static_cast<int>(BAR_WIDTH * frac);
            std::cerr << "\r  [";
            for (int i = 0; i < BAR_WIDTH; i++) {
                if (i < filled)       std::cerr << '#';
                else if (i == filled) std::cerr << '>';
                else                  std::cerr << ' ';
            }
            std::cerr << "] " << std::setw(3) << static_cast<int>(frac * 100) << "%"
                      << " (" << done << "/" << total_pairs << ")    ";
            std::cerr.flush();
        };

        // Create temp output file for this iteration
        std::filesystem::path temp_eds_out = temp_dir / ("iter_" + std::to_string(iteration) + ".eds");
        std::ofstream eds_out_stream(temp_eds_out);

        if (!eds_out_stream) {
            throw std::runtime_error("Failed to create temp output file: " + temp_eds_out.string());
        }

        // Compute all merge metadata first (batched for parallel memory control),
        // then stream the full result once. stream_merged_symbols_to_file iterates
        // over ALL positions, so calling it once per batch would write every
        // unmodified symbol N-times (once per batch).
        std::vector<MergeMetadata> all_metadata;
        all_metadata.reserve(pairs.size());

        for (size_t batch_start = 0; batch_start < pairs.size(); batch_start += BATCH_SIZE) {
            print_bar(batch_start);

            size_t batch_end = std::min(batch_start + BATCH_SIZE, pairs.size());
            std::vector<MergePair> batch_pairs(
                pairs.begin() + batch_start,
                pairs.begin() + batch_end
            );

            // Compute merge metadata (NO string data, minimal memory)
            auto batch_metadata = compute_merge_metadata(eds, batch_pairs, num_threads);

            // Accumulate metadata across batches
            all_metadata.insert(all_metadata.end(),
                                 std::make_move_iterator(batch_metadata.begin()),
                                 std::make_move_iterator(batch_metadata.end()));
        }
        print_bar(total_pairs);
        if (stderr_tty) std::cerr << "\n";

        // Stream full result to file once (each position written exactly once).
        // Also captures output metadata inline — avoids re-reading the file next iteration.
        auto stream_result = stream_merged_symbols_to_file(
            eds,
            all_metadata,
            eds_out_stream,
            nullptr  // No sources in cartesian mode
        );

        eds_out_stream.close();

        // Log merge outcome and new metadata state
        {
            const auto& m = stream_result.metadata;
            std::cerr << "[l-EDS]   Merged:   " << total_symbols << " → " << stream_result.n
                      << " symbols (" << (total_symbols - stream_result.n) << " consumed)"
                      << ", " << stream_result.m << " strings"
                      << ", " << stream_result.N << " chars\n";
            std::cerr << "[l-EDS]   Metadata: ctx min=" << m.min_context_length
                      << " max=" << m.max_context_length
                      << std::fixed << std::setprecision(1)
                      << " avg=" << m.avg_context_length
                      << " | degen=" << m.num_degenerate_symbols
                      << " empty=" << m.num_empty_strings
                      << " (built inline, no re-parse)\n";
        }

        // Replace EDS with temp file using pre-built metadata (no re-parse needed).
        eds = EDS::from_metadata(std::move(stream_result.metadata),
                                 stream_result.n, stream_result.m, stream_result.N,
                                 temp_eds_out);

        // Delete previous iteration file immediately (Linux: fd valid after unlink)
        std::filesystem::remove(current_eds_file);

        // Update file pointer
        current_eds_file = temp_eds_out;

        iteration++;
    }

    if (iteration >= MAX_ITERATIONS) {
        throw std::runtime_error("Maximum iterations reached without convergence");
    }

    // Serialise final result to the output stream.
    // Intermediate temp files always use full-bracket format (required for METADATA_ONLY
    // seeks); the requested output format is applied only here, on the final EDS object.
    // Using read_symbol() rather than a raw rdbuf() copy ensures the format flag is
    // honoured even when the input was already l-EDS compliant (zero iterations run).
    for (size_t i = 0; i < eds.length(); ++i) {
        const StringSet sym = eds.read_symbol(i);
        bool use_brackets = !compact || sym.size() > 1;
        if (use_brackets) output << '{';
        for (size_t j = 0; j < sym.size(); ++j) {
            if (j > 0) output << ',';
            output << sym[j];
        }
        if (use_brackets) output << '}';
    }
    output << '\n';

    // Temp directory is removed by temp_guard (RAII) when this function returns,
    // covering both the normal path here and any exception thrown above.
}

} // namespace edsparser
