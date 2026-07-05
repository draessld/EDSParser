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

    // A contiguous run of positions that should all be merged into one output symbol.
    // Chain detection (select_merge_groups) extends greedily: if pair (i,i+1) and
    // pair (i+1,i+2) both need merging, they form one group of count=3, not two
    // separate pairs.  This is the core of the O(chain_length) → O(1) iteration fix.
    struct MergeGroup {
        size_t start;   // first position in the group
        size_t count;   // number of consecutive positions (>= 2)
        MergeReason reason;  // primary reason (from the first pair in the chain)

        MergeGroup(size_t s, size_t c, MergeReason r)
            : start(s), count(c), reason(r) {}
    };

    // Metadata-only result of merging a group of positions.
    // Contains NO string data — strings are read on-demand during streaming.
    //
    // valid_indices_flat encodes, for each output string m and group position k,
    // which alternative to pick:  valid_indices_flat[m * group_count + k] is the
    // index into input_eds.read_symbol(group_start + k).
    // For a 2-position group this is equivalent to the old (i, j) pair encoding.
    struct MergeMetadata {
        size_t group_start;   // first position in the merged group
        size_t group_count;   // number of positions merged (>= 2)
        size_t merged_size;   // number of strings in the merged output symbol
        std::vector<Length> merged_string_lengths;
        std::vector<PathSet> merged_sources;     // empty if no source tracking
        std::vector<size_t>  valid_indices_flat; // size = merged_size * group_count
    };
    

    // Returns {true, reason} if positions i and i+1 need to be merged.
    // Same four conditions as before; extracted so chain detection can reuse them.
    // Takes the metadata reference and symbol count directly so the caller can
    // hoist eds.get_metadata() out of the per-position scan (it was previously
    // re-fetched several times per call).
    std::pair<bool, MergeReason> needs_merge(
        const EDS::Metadata& meta,
        size_t n,
        size_t i,
        Length context_length
    ) {
        const auto& is_degenerate = meta.is_degenerate;

        bool left_short  = false;
        bool right_short = false;
        bool adj_degen   = (is_degenerate[i] && is_degenerate[i + 1]);
        bool adj_common  = (!is_degenerate[i] && !is_degenerate[i + 1]);

        if (!is_degenerate[i] && i > 0 && i < n - 1) {
            size_t g = meta.cum_set_sizes[i];
            if (meta.string_lengths[g] < context_length) left_short = true;
        }
        if (!is_degenerate[i + 1] && (i + 1) > 0 && (i + 1) < n - 1) {
            size_t g = meta.cum_set_sizes[i + 1];
            if (meta.string_lengths[g] < context_length) right_short = true;
        }

        if (!(left_short || right_short || adj_degen || adj_common))
            return {false, MergeReason::ADJACENT_DEGENERATE};

        MergeReason reason;
        if (adj_degen)       reason = MergeReason::ADJACENT_DEGENERATE;
        else if (adj_common) reason = MergeReason::ADJACENT_COMMON;
        else if (left_short) reason = MergeReason::SHORT_COMMON_LEFT;
        else                 reason = MergeReason::SHORT_COMMON_RIGHT;

        return {true, reason};
    }

    // Select merge groups: maximal contiguous chains where every adjacent pair
    // needs merging.  Each chain becomes ONE output symbol in a single pass.
    //
    // Old approach (select_independent_merge_pairs): picked only non-overlapping
    // adjacent PAIRS, so a chain of length L required L-1 iterations, each
    // writing the entire file.  This function instead extends greedily:
    //
    //   i=0, found (0,1) needs merge → scan forward while (j, j+1) needs merge
    //   → emit MergeGroup{0, chain_length, reason}
    //   → jump past the group and continue
    //
    // Result: O(chain_length) → O(1) iterations for typical genomic data.
    std::vector<MergeGroup> select_merge_groups(
        const EDS& eds,
        Length context_length
    ) {
        std::vector<MergeGroup> groups;

        const size_t n = eds.length();
        if (n < 2) return groups;

        // Hoist the metadata reference out of the scan: needs_merge() previously
        // re-fetched eds.get_metadata() several times per call.
        const auto& meta = eds.get_metadata();

        size_t i = 0;
        while (i + 1 < n) {
            auto [merge, reason] = needs_merge(meta, n, i, context_length);
            if (merge) {
                size_t group_start = i;
                // Extend the chain as far as consecutive pairs also need merging.
                while (i + 2 < n) {
                    auto [merge_next, ignored] = needs_merge(meta, n, i + 1, context_length);
                    if (!merge_next) break;
                    ++i;
                }
                size_t group_count = (i + 1) - group_start + 1;  // inclusive [start, i+1]
                groups.emplace_back(group_start, group_count, reason);
                i += 2;  // jump past the last position in the group
            } else {
                ++i;
            }
        }

        return groups;
    }

    // Compute merge metadata for a vector of MergeGroups WITHOUT building string data.
    // Works with METADATA_ONLY mode; strings are read on-demand later during streaming.
    //
    // For each group [p₀, p₁, ..., pₖ] the result is computed by an iterative fold:
    //   step 0: initialise accumulator from p₀
    //   step j: for each (accumulated string m, alternative j in pⱼ), check source
    //           compatibility and keep only valid combinations; accumulate lengths.
    //
    // valid_indices_flat[m * group_count + k] = which alternative from position
    // (group_start + k) contributes to output string m.  The streaming function
    // reads all group positions on-demand and concatenates the indicated alternatives.
    std::vector<MergeMetadata> compute_merge_metadata(
        const EDS& eds,
        const std::vector<MergeGroup>& groups,
        size_t num_threads
    ) {
        std::vector<MergeMetadata> results(groups.size());

        const bool has_sources = eds.has_sources();
        const auto& metadata   = eds.get_metadata();

        // Bitset helpers (used in the per-group lambda below).
        // Path IDs are 1-based; ID 0 is the sentinel "universal" source.
        auto to_bits = [](const PathSet& s) -> uint64_t {
            if (!s.empty() && s[0] == 0) return ~0ULL;
            uint64_t b = 0;
            for (int id : s) b |= (1ULL << (id - 1));
            return b;
        };
        auto bits_to_set = [](uint64_t b) -> PathSet {
            if (b == ~0ULL) return {0};
            PathSet s;
            for (int k = 0; k < 63; ++k)
                if (b & (1ULL << k)) s.push_back(k + 1);
            return s;
        };

        // Preload all needed source sets before the parallel region so that
        // workers can read from the map lock-free instead of going through
        // Sources::io_mutex_ on every read_source() call.
        std::unordered_map<size_t, PathSet> preloaded;
#ifdef _OPENMP
        if (has_sources && num_threads > 1 && !groups.empty()) {
            size_t total_strings = 0;
            for (const auto& g : groups)
                for (size_t k = 0; k < g.count; ++k)
                    total_strings += metadata.symbol_sizes[g.start + k];
            preloaded.reserve(total_strings);

            for (const auto& g : groups)
                for (size_t k = 0; k < g.count; ++k) {
                    size_t pj = g.start + k;
                    size_t gj = metadata.cum_set_sizes[pj];
                    size_t szj = metadata.symbol_sizes[pj];
                    for (size_t j = 0; j < szj; ++j)
                        preloaded.emplace(gj + j, eds.read_source(gj + j));
                }
        }
#endif

        // Returns the source PathSet for global string index idx, by const
        // reference to avoid a copy at the access point.
        //   • Parallel path: `preloaded` is populated and immutable for the whole
        //     region, so a reference into it is safe across threads.
        //   • Fallback path: `preloaded` is empty only when the computation runs
        //     sequentially (num_threads <= 1, or no OpenMP), so read_source_ref()
        //     — whose reference is invalidated by later cache mutations — is safe:
        //     each result is consumed (copied into srcj/src0) before the next call.
        auto get_src = [&](size_t idx) -> const PathSet& {
            if (!preloaded.empty()) return preloaded.at(idx);
            return eds.get_sources_object()->read_source_ref(idx);
        };

        // Per-group computation — runs in parallel when num_threads > 1.
        // Computes the merged metadata by iterative fold over the group positions.
        auto compute_group_metadata = [&](const MergeGroup& group) -> MergeMetadata {
            MergeMetadata result;
            result.group_start = group.start;
            result.group_count = group.count;

            // ── Initialise accumulator from position p₀ ─────────────────────
            size_t p0 = group.start;
            size_t g0 = metadata.cum_set_sizes[p0];
            size_t sz0 = metadata.symbol_sizes[p0];

            size_t cur_size = sz0;
            std::vector<Length> cur_lengths(sz0);
            // cur_flat: flat index array with stride = current fold step.
            // cur_flat[m * step + k] = which alternative from position (p0+k)
            // contributes to accumulated string m.  Stride grows by 1 each fold.
            std::vector<size_t> cur_flat(sz0);
            for (size_t m = 0; m < sz0; ++m) {
                cur_lengths[m] = metadata.string_lengths[g0 + m];
                cur_flat[m]    = m;  // step=1: only position p0, index = m itself
            }

            // Source accumulator for LINEAR mode.
            // use_bits stays true as long as all path IDs fit in [1,63].
            // If a position breaks the invariant we switch cur_bits → cur_src_sets.
            bool use_bits = has_sources;
            std::vector<uint64_t> cur_bits;
            std::vector<PathSet>  cur_src_sets;

            if (has_sources) {
                std::vector<PathSet> src0(sz0);
                for (size_t m = 0; m < sz0; ++m) {
                    src0[m] = get_src(g0 + m);
                    if (use_bits && !src0[m].empty() && src0[m][0] != 0 && src0[m].back() > 63)
                        use_bits = false;
                }
                if (use_bits) {
                    cur_bits.resize(sz0);
                    for (size_t m = 0; m < sz0; ++m) cur_bits[m] = to_bits(src0[m]);
                } else {
                    cur_src_sets = std::move(src0);
                }
            }

            // ── Fold in positions p₁, p₂, … ─────────────────────────────────
            for (size_t step = 1; step < group.count; ++step) {
                size_t pj  = group.start + step;
                size_t gj  = metadata.cum_set_sizes[pj];
                size_t szj = metadata.symbol_sizes[pj];

                // Load sources for this position and check bitset applicability.
                std::vector<PathSet>  srcj;
                std::vector<uint64_t> bitsj;
                if (has_sources) {
                    srcj.resize(szj);
                    for (size_t j = 0; j < szj; ++j) {
                        srcj[j] = get_src(gj + j);
                        if (use_bits && !srcj[j].empty() && srcj[j][0] != 0 && srcj[j].back() > 63)
                            use_bits = false;
                    }
                    // If this position broke use_bits, convert cur_bits → cur_src_sets now.
                    if (!use_bits && !cur_bits.empty()) {
                        cur_src_sets.resize(cur_size);
                        for (size_t m = 0; m < cur_size; ++m)
                            cur_src_sets[m] = bits_to_set(cur_bits[m]);
                        cur_bits.clear();
                    }
                    if (use_bits) {
                        bitsj.resize(szj);
                        for (size_t j = 0; j < szj; ++j) bitsj[j] = to_bits(srcj[j]);
                    }
                }

                size_t new_stride   = step + 1;
                size_t new_max_size = cur_size * szj;

                std::vector<Length>   new_lengths;
                std::vector<size_t>   new_flat;
                std::vector<uint64_t> new_bits;
                std::vector<PathSet>  new_src;

                new_lengths.reserve(new_max_size);
                new_flat.reserve(new_max_size * new_stride);
                if (has_sources) {
                    if (use_bits) new_bits.reserve(new_max_size);
                    else          new_src.reserve(new_max_size);
                }

                for (size_t m = 0; m < cur_size; ++m) {
                    Length base_len = cur_lengths[m];
                    for (size_t j = 0; j < szj; ++j) {
                        if (has_sources) {
                            if (use_bits) {
                                uint64_t a = cur_bits[m], b = bitsj[j];
                                uint64_t isect = (a == ~0ULL) ? b
                                               : (b == ~0ULL) ? a
                                               : a & b;
                                if (isect == 0) continue;
                                new_bits.push_back(isect);
                            } else {
                                PathSet isect = Sources::intersect_sources(cur_src_sets[m], srcj[j]);
                                if (isect.empty()) continue;
                                new_src.push_back(std::move(isect));
                            }
                        }
                        new_lengths.push_back(base_len + metadata.string_lengths[gj + j]);
                        // Copy the prev-step index tuple for m, then append j.
                        for (size_t k = 0; k < step; ++k)
                            new_flat.push_back(cur_flat[m * step + k]);
                        new_flat.push_back(j);
                    }
                }

                cur_size     = new_lengths.size();
                cur_lengths  = std::move(new_lengths);
                cur_flat     = std::move(new_flat);
                if (has_sources) {
                    cur_bits     = std::move(new_bits);
                    cur_src_sets = std::move(new_src);
                }

                if (cur_size == 0) {
                    throw std::runtime_error(
                        "Merging group at position " + std::to_string(group.start) +
                        " (count=" + std::to_string(group.count) +
                        ") produced an empty set after folding position " +
                        std::to_string(pj) + " (no valid source intersections)"
                    );
                }
            }

            // ── Build final result ──────────────────────────────────────────
            result.merged_size           = cur_size;
            result.merged_string_lengths = std::move(cur_lengths);
            result.valid_indices_flat    = std::move(cur_flat);

            if (has_sources) {
                result.merged_sources.resize(cur_size);
                if (use_bits) {
                    for (size_t m = 0; m < cur_size; ++m)
                        result.merged_sources[m] = bits_to_set(cur_bits[m]);
                } else {
                    result.merged_sources = std::move(cur_src_sets);
                }
            }

            return result;
        };

        // Process groups in parallel or sequentially.
        if (num_threads <= 1 || groups.empty()) {
            for (size_t i = 0; i < groups.size(); ++i)
                results[i] = compute_group_metadata(groups[i]);
        } else {
#ifdef _OPENMP
            std::exception_ptr first_exception;
            std::atomic<bool> exception_occurred{false};
            #pragma omp parallel for num_threads(num_threads)
            for (size_t i = 0; i < groups.size(); ++i) {
                if (exception_occurred.load(std::memory_order_relaxed)) continue;
                try {
                    results[i] = compute_group_metadata(groups[i]);
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
            for (size_t i = 0; i < groups.size(); ++i)
                results[i] = compute_group_metadata(groups[i]);
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

// ─────────────────────────────────────────────────────────────────────────────
// MergeStreamWriter — write one iteration's EDS (and SEDS) output
// ─────────────────────────────────────────────────────────────────────────────
//
// BIG PICTURE — where this writer sits in the overall pipeline
//
//   eds_to_leds_linear() iteratively transforms an EDS into an l-EDS by
//   merging adjacent symbols that violate the l-context rule.  Each iteration
//   looks like this:
//
//     1. select_merge_groups()   — find maximal chains to merge (metadata only)
//     2. compute_merge_metadata() — compute what each merged group looks like,
//                                   one BATCH_SIZE window at a time
//     3. writer.add_batch(...)    — write each window's EDS (and SEDS) output as
//                                   soon as it is computed, then discard it
//     4. writer.finish()          — drain the trailing unmodified tail
//     5. EDS::from_metadata()     — create the next EDS object pointing at the
//                                   temp file just written, using the metadata
//                                   the writer returns (no re-parse)
//
//   The writer is the single place where output I/O happens for a complete
//   iteration.  All symbols (merged and unmodified alike) are written once.
//
// INPUTS
//
//   input_eds       The EDS from the previous iteration (or the original input).
//                   Loaded in METADATA_ONLY mode: only the index is in RAM; the
//                   actual character data is read on demand from the temp file.
//
//   Each add_batch() call receives a vector of MergeMetadata structs (one per
//   merged group, in ascending position order).  Each struct contains:
//     • group_start, group_count    — the run of positions fused into one symbol
//     • merged_size                 — number of strings in the result symbol
//     • merged_string_lengths       — length of each result string
//     • valid_indices_flat          — valid_indices_flat[m*group_count + k] is the
//                                     alternative from position (group_start+k) that
//                                     contributes to output string m (CARTESIAN: all
//                                     tuples; LINEAR: only source-compatible tuples)
//     • merged_sources              — the intersected source set for each output
//                                     string m (empty when no source tracking)
//
//   eds_out         Output stream for the new EDS temp file.
//   sources_out     Output stream for the new SEDS temp file, or nullptr for a
//                   cartesian transform (no source tracking).
//
// OUTPUTS  (from finish())
//
//   A StreamResult containing:
//     • metadata   — a fully populated EDS::Metadata struct describing the output
//                    file, built *inline* as we write.  This lets the caller create
//                    the next EDS object via EDS::from_metadata() without re-reading
//                    the file.
//     • n / m / N  — symbol / string / character counts in the output.
//
// HOW WE NAVIGATE MERGED vs UNMODIFIED POSITIONS
//
//   The writer keeps a monotonically increasing `cursor_` over input positions.
//   add_batch() walks each group in the batch: it emits every position in
//   [cursor_, group_start) as an unmodified symbol, then emits the merged group
//   as one output symbol, then advances cursor_ to group_start + group_count
//   (jumping over the absorbed positions, which produce no output of their own).
//   finish() emits [cursor_, length()) as the trailing unmodified run.  Because
//   groups arrive in position order and never overlap, every input position is
//   visited exactly once across all add_batch() calls plus finish().
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
//   base_positions[]     byte offset of each symbol's '{' in eds_out.  Tracked
//                        via the manual out_pos_ counter (not eds_out.tellp(),
//                        which the deferred raw-copy batch would leave stale).
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
// WHY STREAM PER BATCH (memory)
//
//   An earlier design accumulated every group's MergeMetadata in one
//   `all_metadata` vector held for the whole iteration.  For CARTESIAN merges
//   each entry stores merged_string_lengths + valid_indices_flat sized
//   merged_size × group product, so in a dense region that vector — not the
//   BATCH_SIZE compute working set — was the dominant allocation and grew with
//   the number of groups.  Consuming one batch at a time via add_batch() and
//   discarding it afterwards caps retained MergeMetadata to a single BATCH_SIZE
//   window (measured ~23% lower peak RSS on a 40k-group cartesian run).  Output
//   is byte-identical to the old accumulate-then-stream version: the manual
//   out_pos_ tracking, the EDS raw-copy batch, and the SEDS copy batch all carry
//   across batch boundaries in member state.
// ─────────────────────────────────────────────────────────────────────────────
    class MergeStreamWriter {
    public:
        MergeStreamWriter(const EDS& input_eds,
                          std::ostream& eds_out,
                          std::ostream* sources_out)
            : input_eds_(input_eds),
              eds_out_(eds_out),
              sources_out_(sources_out),
              has_sources_(input_eds.has_sources()),
              in_meta_(input_eds.get_metadata()) {
            // min_context_length starts at UINT32_MAX so the first real context
            // length (which may be small) correctly replaces it via std::min logic.
            result_.metadata.min_context_length = UINT32_MAX;
            result_.metadata.max_context_length = 0;
            result_.metadata.num_degenerate_symbols = 0;
            result_.metadata.num_common_chars = 0;
            result_.metadata.total_change_size = 0;
            result_.metadata.num_empty_strings = 0;
            // The cum_* arrays carry a leading index-0 entry for the state
            // *before* any symbol; per-symbol entries are appended as symbols
            // are written, giving final length n+1.
            result_.metadata.cum_common_positions.push_back(0);
            result_.metadata.cum_degenerate_counts.push_back(0);
        }

        // Consume one position-ordered batch of merge metadata.  Emits every
        // unmodified position up to each group's start, then the merged group.
        void add_batch(const std::vector<MergeMetadata>& batch) {
            for (const auto& mm : batch) {
                while (cursor_ < mm.group_start) {
                    write_unmodified(cursor_);
                    ++cursor_;
                }
                write_merged(mm);
                cursor_ = mm.group_start + mm.group_count;  // skip absorbed positions
            }
        }

        // Emit the trailing run of unmodified positions, drain the pending
        // batches, finalise statistics, and hand back the StreamResult.
        StreamResult finish() {
            while (cursor_ < input_eds_.length()) {
                write_unmodified(cursor_);
                ++cursor_;
            }
            flush_eds_batch();
            flush_seds_batch();

            result_.metadata.avg_context_length = (num_context_blocks_ > 0)
                ? static_cast<double>(total_context_length_) / num_context_blocks_
                : 0.0;
            if (result_.metadata.min_context_length == UINT32_MAX)
                result_.metadata.min_context_length = 0;

            return std::move(result_);
        }

    private:
        const EDS& input_eds_;
        std::ostream& eds_out_;
        std::ostream* sources_out_;
        bool has_sources_;
        const EDS::Metadata& in_meta_;

        StreamResult result_;
        size_t total_context_length_ = 0;   // used to compute avg at the end
        size_t num_context_blocks_ = 0;     // number of non-degenerate symbols seen
        Position cumulative_common_ = 0;    // cumulative non-degenerate char count
        int cumulative_degenerate_ = 0;     // cumulative degenerate string count

        // SEDS copy-batch state (see the header comment above): the pending
        // run of consecutive unmodified symbols' source entries, flushed in a
        // single copy_range_to_stream() call at each merge boundary / at finish.
        size_t seds_batch_start_ = 0;
        size_t seds_batch_count_ = 0;

        // EDS raw-copy batch state: the pending run of consecutive raw-copyable
        // unmodified symbols, flushed in one copy_symbol_range_to_stream() call.
        size_t eds_batch_start_ = 0;
        size_t eds_batch_count_ = 0;

        // Manual output byte-offset tracker.  Raw-copy batches defer physical
        // writes past the point where each symbol's base_position is recorded,
        // so eds_out_.tellp() would be stale; every symbol advances out_pos_ by
        // its full-bracket byte length regardless of when bytes are flushed.
        std::streamoff out_pos_ = 0;

        // Cursor over input positions; monotonically increasing across batches.
        size_t cursor_ = 0;

        void flush_eds_batch() {
            if (eds_batch_count_ > 0) {
                input_eds_.copy_symbol_range_to_stream(eds_batch_start_, eds_batch_count_, eds_out_);
                eds_batch_count_ = 0;
            }
        }

        void flush_seds_batch() {
            if (has_sources_ && sources_out_ && seds_batch_count_ > 0) {
                input_eds_.get_sources_object()->copy_range_to_stream(
                    seds_batch_start_, seds_batch_count_, *sources_out_);
                seds_batch_count_ = 0;
            }
        }

        // Common per-symbol metadata tail (identical for merged and unmodified).
        void record_symbol(size_t sym_size) {
            bool is_deg = (sym_size > 1);
            result_.metadata.symbol_sizes.push_back(static_cast<Length>(sym_size));
            result_.metadata.is_degenerate.push_back(is_deg);

            if (is_deg) {
                result_.metadata.num_degenerate_symbols++;
                result_.metadata.total_change_size += (sym_size - 1);
                cumulative_degenerate_ += static_cast<int>(sym_size);
            } else {
                Length ctx_len = result_.metadata.string_lengths.back();
                result_.metadata.num_common_chars += ctx_len;
                total_context_length_ += ctx_len;
                num_context_blocks_++;
                cumulative_common_ += static_cast<Position>(ctx_len);
                if (ctx_len < result_.metadata.min_context_length)
                    result_.metadata.min_context_length = ctx_len;
                if (ctx_len > result_.metadata.max_context_length)
                    result_.metadata.max_context_length = ctx_len;
            }

            result_.metadata.cum_common_positions.push_back(cumulative_common_);
            result_.metadata.cum_degenerate_counts.push_back(cumulative_degenerate_);

            result_.m += sym_size;
            result_.n++;
        }

        // BRANCH A: write a merged group as one output symbol.
        void write_merged(const MergeMetadata& merge_meta) {
            // Flush unmodified EDS/SEDS data accumulated before this group; the
            // merged symbol's bytes must follow them in the output.
            flush_eds_batch();
            flush_seds_batch();

            result_.metadata.base_positions.push_back(static_cast<std::streampos>(out_pos_));
            result_.metadata.cum_set_sizes.push_back(result_.m);

            const size_t sym_size = merge_meta.merged_size;
            const size_t gc = merge_meta.group_count;
            const size_t pos = merge_meta.group_start;

            size_t sum_len = 0;
            for (Length len : merge_meta.merged_string_lengths) {
                result_.metadata.string_lengths.push_back(len);
                result_.N += len;
                sum_len += len;
                if (len == 0) result_.metadata.num_empty_strings++;
            }
            // Full-bracket bytes: '{' + strings + (sym_size−1) commas + '}'.
            out_pos_ += static_cast<std::streamoff>(sum_len + sym_size + 1);

            // Read all gc symbols in the group sequentially (the sequential-seek
            // optimisation fires after the first read → at most one seek/group).
            std::vector<StringSet> syms(gc);
            for (size_t k = 0; k < gc; ++k)
                syms[k] = input_eds_.read_symbol(pos + k);

            eds_out_ << '{';
            bool first_string = true;
            for (size_t m = 0; m < sym_size; ++m) {
                if (!first_string) eds_out_ << ',';
                for (size_t k = 0; k < gc; ++k)
                    eds_out_ << syms[k][merge_meta.valid_indices_flat[m * gc + k]];
                first_string = false;

                if (sources_out_) {
                    *sources_out_ << '{';
                    bool first_path = true;
                    for (int path_id : merge_meta.merged_sources[m]) {
                        if (!first_path) *sources_out_ << ',';
                        *sources_out_ << path_id;
                        first_path = false;
                    }
                    *sources_out_ << '}';
                }
            }
            eds_out_ << '}';

            record_symbol(sym_size);
        }

        // BRANCH B: write an unmodified position verbatim.
        void write_unmodified(size_t pos) {
            result_.metadata.base_positions.push_back(static_cast<std::streampos>(out_pos_));
            result_.metadata.cum_set_sizes.push_back(result_.m);

            const size_t sym_size = in_meta_.symbol_sizes[pos];
            const size_t global_idx = in_meta_.cum_set_sizes[pos];

            size_t sum_len = 0;
            for (size_t k = 0; k < sym_size; ++k) {
                Length len = in_meta_.string_lengths[global_idx + k];
                result_.metadata.string_lengths.push_back(len);
                result_.N += len;
                sum_len += len;
                if (len == 0) result_.metadata.num_empty_strings++;
            }

            // Raw-copy fast path: an unmodified full-bracket symbol with no
            // trailing padding (input span == full-bracket byte length) is
            // byte-identical in the output, so batch it for a single
            // copy_symbol_range_to_stream() call.  Otherwise (compact input,
            // interior whitespace, or the last symbol whose next base_position
            // is unknown) fall back to parse-and-reserialise.
            const size_t full_len = sum_len + sym_size + 1;
            bool raw_ok = false;
            if (pos + 1 < input_eds_.length()) {
                auto ia = static_cast<std::streamoff>(in_meta_.base_positions[pos]);
                auto ib = static_cast<std::streamoff>(in_meta_.base_positions[pos + 1]);
                raw_ok = (ib - ia == static_cast<std::streamoff>(full_len));
            }

            if (raw_ok) {
                if (eds_batch_count_ == 0) eds_batch_start_ = pos;
                eds_batch_count_++;
            } else {
                flush_eds_batch();
                StringSet symbol = input_eds_.read_symbol(pos);
                eds_out_ << '{';
                for (size_t i = 0; i < symbol.size(); ++i) {
                    if (i > 0) eds_out_ << ',';
                    eds_out_ << symbol[i];
                }
                eds_out_ << '}';
            }

            out_pos_ += static_cast<std::streamoff>(full_len);

            if (has_sources_ && sources_out_) {
                if (seds_batch_count_ == 0) seds_batch_start_ = global_idx;
                seds_batch_count_ += sym_size;
            }

            record_symbol(sym_size);
        }
    };

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
// ── Internal helper ──────────────────────────────────────────────────────────
// Runs the actual l-EDS linear-merge loop given an already-loaded EDS and
// the temp-directory that will hold iteration files.  Extracted so that both
// the stream-based and path-based public overloads can share it without
// duplicating ~200 lines of code.
//
// current_eds_file / current_seds_file: the "input" files for iteration 0.
// These may be plain files (stream path) or symlinks (path-based path).
// Either way, the loop deletes them after they have been consumed, so they
// must NOT be files the caller still needs after this function returns.
// The path-based caller achieves this by creating symlinks in temp_dir;
// deleting the symlink leaves the underlying stage-1 file intact.
static void leds_linear_transform(
    EDS& eds,
    bool has_sources,
    std::filesystem::path current_eds_file,
    std::filesystem::path current_seds_file,
    const std::filesystem::path& temp_dir,
    std::ostream& output,
    std::ostream* phasing_output,
    Length context_length,
    size_t num_threads,
    bool compact
) {
    // Warn when temp space requirement is non-trivial.
    // Peak usage: input copy + previous iteration + current iteration = ~3× input size.
    {
        std::error_code ec;
        auto input_size = std::filesystem::file_size(current_eds_file, ec);
        if (!ec && input_size > 500ULL * 1024 * 1024) {
            auto gb = [](uintmax_t b) { return b / double(1ULL << 30); };
            std::cerr << "[l-EDS] Temp disk needed: ~" << std::fixed << std::setprecision(1)
                      << gb(input_size * 3) << " GB in " << temp_dir << "\n";
        }
    }

    // ===== COMPLEXITY ESTIMATION =====
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
    const size_t MAX_ITERATIONS = 10000;
    const size_t BATCH_SIZE = 1000;

    while (iteration < MAX_ITERATIONS) {
        auto groups = select_merge_groups(eds, context_length);

        if (groups.empty()) {
            std::cerr << "[l-EDS] Converged after " << iteration << " iterations\n";
            break;
        }

        size_t total_symbols = eds.length();
        size_t total_groups  = groups.size();
        {
            size_t n_adj = 0, n_left = 0, n_right = 0, n_common = 0;
            size_t positions_consumed = 0;
            for (const auto& g : groups) {
                positions_consumed += g.count;
                switch (g.reason) {
                    case MergeReason::ADJACENT_DEGENERATE: ++n_adj;    break;
                    case MergeReason::SHORT_COMMON_LEFT:   ++n_left;   break;
                    case MergeReason::SHORT_COMMON_RIGHT:  ++n_right;  break;
                    case MergeReason::SHORT_COMMON_BOTH:   break;
                    case MergeReason::ADJACENT_COMMON:     ++n_common; break;
                }
            }
            std::cerr << "[l-EDS] Iter " << iteration
                      << ": " << total_symbols << " symbols, "
                      << total_groups << " groups (" << positions_consumed << " positions consumed)";
            std::cerr << " (";
            bool first = true;
            auto sep = [&]() { if (!first) std::cerr << ", "; first = false; };
            if (n_adj)   { sep(); std::cerr << n_adj   << " adj-degen"; }
            if (n_left)  { sep(); std::cerr << n_left  << " short-ctx←left";  }
            if (n_right) { sep(); std::cerr << n_right << " short-ctx→right"; }
            if (n_common){ sep(); std::cerr << n_common<< " adj-common";      }
            std::cerr << ")\n";
        }

        const bool stderr_tty = isatty(STDERR_FILENO);
        auto print_bar = [&](size_t done) {
            if (!stderr_tty) return;
            const int BAR_WIDTH = 40;
            float frac = total_groups > 0 ? static_cast<float>(done) / total_groups : 1.0f;
            int filled = static_cast<int>(BAR_WIDTH * frac);
            std::cerr << "\r  [";
            for (int i = 0; i < BAR_WIDTH; i++) {
                if (i < filled)       std::cerr << '#';
                else if (i == filled) std::cerr << '>';
                else                  std::cerr << ' ';
            }
            std::cerr << "] " << std::setw(3) << static_cast<int>(frac * 100) << "%"
                      << " (" << done << "/" << total_groups << ")    ";
            std::cerr.flush();
        };

        std::filesystem::path temp_eds_out  = temp_dir / ("iter_" + std::to_string(iteration) + ".eds");
        std::filesystem::path temp_seds_out = temp_dir / ("iter_" + std::to_string(iteration) + ".seds");

        std::ofstream eds_out_stream(temp_eds_out);
        std::ofstream seds_out_stream;

        if (!eds_out_stream)
            throw std::runtime_error("Failed to create temp output file: " + temp_eds_out.string());

        if (has_sources) {
            seds_out_stream.open(temp_seds_out);
            if (!seds_out_stream)
                throw std::runtime_error("Failed to create temp sources output file: " + temp_seds_out.string());
        }

        // Stream each batch's merge metadata straight to the writer as it is
        // computed, rather than accumulating the whole iteration's results in a
        // single `all_metadata` vector.  This caps retained MergeMetadata to one
        // BATCH_SIZE window (important for dense/exponential CARTESIAN regions
        // where valid_indices_flat dominates memory) — batch_metadata is freed
        // at the end of each loop iteration.
        MergeStreamWriter writer(eds, eds_out_stream,
                                 has_sources ? &seds_out_stream : nullptr);

        for (size_t batch_start = 0; batch_start < groups.size(); batch_start += BATCH_SIZE) {
            print_bar(batch_start);

            size_t batch_end = std::min(batch_start + BATCH_SIZE, groups.size());
            std::vector<MergeGroup> batch_groups(
                groups.begin() + batch_start,
                groups.begin() + batch_end
            );

            auto batch_metadata = compute_merge_metadata(eds, batch_groups, num_threads);
            writer.add_batch(batch_metadata);
        }
        print_bar(total_groups);
        if (stderr_tty) std::cerr << "\n";

        auto stream_result = writer.finish();

        eds_out_stream.close();
        if (has_sources) seds_out_stream.close();

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

        // Remove the file (or symlink) that was the input for this iteration.
        // If current_eds_file is a symlink (path-based overload), only the symlink
        // is removed — the stage-1 file it pointed to is unaffected.
        std::filesystem::remove(current_eds_file);
        if (has_sources) std::filesystem::remove(current_seds_file);

        current_eds_file  = temp_eds_out;
        current_seds_file = temp_seds_out;

        iteration++;
    }

    if (iteration >= MAX_ITERATIONS)
        throw std::runtime_error("Maximum iterations reached without convergence");

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
}

// ── Stream-based overload (existing callers: eds2leds tool) ──────────────────
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

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path()
        / ("edsparser_leds_" + std::to_string(getpid()));
    std::filesystem::create_directories(temp_dir);
    TempDirGuard temp_guard{temp_dir};

    // Copy input EDS stream to a temp file (required for METADATA_ONLY seeks).
    std::filesystem::path temp_input = temp_dir / "input.eds";
    {
        std::ofstream temp_out(temp_input);
        if (!temp_out)
            throw std::runtime_error("Failed to create temp input file: " + temp_input.string());
        temp_out << input.rdbuf();
        if (!temp_out)
            throw std::runtime_error("Failed to write EDS temp file (disk full?): " + temp_input.string());
        temp_out.close();
        if (!temp_out)
            throw std::runtime_error("Failed to close EDS temp file: " + temp_input.string());
    }

    std::filesystem::path temp_sources_input;
    bool has_sources = (phasing_input != nullptr);
    if (has_sources) {
        temp_sources_input = temp_dir / "input.seds";
        std::ofstream temp_seds_out(temp_sources_input);
        if (!temp_seds_out)
            throw std::runtime_error("Failed to create temp sources file: " + temp_sources_input.string());
        temp_seds_out << phasing_input->rdbuf();
        if (!temp_seds_out)
            throw std::runtime_error("Failed to write SEDS temp file (disk full?): " + temp_sources_input.string());
        temp_seds_out.close();
        if (!temp_seds_out)
            throw std::runtime_error("Failed to close SEDS temp file: " + temp_sources_input.string());
    }

    EDS eds = has_sources
        ? EDS::load(temp_input, temp_sources_input)
        : EDS::load(temp_input);

    leds_linear_transform(eds, has_sources, temp_input, temp_sources_input,
                          temp_dir, output, phasing_output,
                          context_length, num_threads, compact);
    // temp_guard removes temp_dir on return or exception.
}

// ── Path-based overload (called from parse_vcf_to_leds_streaming_direct) ─────
// Avoids the stream-copy step by symlinking the existing stage-1 files into
// the iteration temp directory.  The symlinks are removed by the first
// iteration (same remove() call as for real files), leaving the originals
// intact — the caller owns and cleans up the stage-1 files.
//
// This is critical for the VCF→l-EDS pipeline: the intermediate SEDS for a
// full chromosome with 2500 samples can reach 100–200 GB; copying it would
// require twice that disk space and fail on constrained /tmp filesystems.
void eds_to_leds_linear(
    const std::filesystem::path& input_eds_path,
    std::ostream& output,
    Length context_length,
    const std::filesystem::path* input_seds_path,
    std::ostream* phasing_output,
    size_t num_threads,
    bool compact
) {
    if (context_length == 0) {
        throw std::invalid_argument("context_length must be > 0 for l-EDS transformation");
    }

    // Iteration files go into a separate temp dir (input files stay in caller's dir).
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path()
        / ("edsparser_leds_vcf_" + std::to_string(getpid()));
    std::filesystem::create_directories(temp_dir);
    TempDirGuard temp_guard{temp_dir};

    bool has_sources = (input_seds_path != nullptr);

    // Symlink the stage-1 files into temp_dir so the iteration loop can safely
    // remove them (removing a symlink never touches the link target).
    std::filesystem::path temp_input = temp_dir / "input.eds";
    std::filesystem::create_symlink(std::filesystem::absolute(input_eds_path), temp_input);

    std::filesystem::path temp_sources_input;
    if (has_sources) {
        // Preserve the original extension so detect_format() picks the right parser
        // (e.g., "input.edz" for EDZ/EDZ_SPARSE, "input.seds" for SEDS/SEDS_SPARSE).
        temp_sources_input = temp_dir / ("input" + input_seds_path->extension().string());
        std::filesystem::create_symlink(std::filesystem::absolute(*input_seds_path), temp_sources_input);
    }

    EDS eds = has_sources
        ? EDS::load(temp_input, temp_sources_input)
        : EDS::load(temp_input);

    leds_linear_transform(eds, has_sources, temp_input, temp_sources_input,
                          temp_dir, output, phasing_output,
                          context_length, num_threads, compact);
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
        auto groups = select_merge_groups(eds, context_length);

        if (groups.empty()) {
            std::cerr << "[l-EDS] Converged after " << iteration << " iterations\n";
            break;
        }

        size_t total_symbols = eds.length();
        size_t total_groups  = groups.size();
        {
            size_t n_adj = 0, n_left = 0, n_right = 0, n_common = 0;
            size_t positions_consumed = 0;
            for (const auto& g : groups) {
                positions_consumed += g.count;
                switch (g.reason) {
                    case MergeReason::ADJACENT_DEGENERATE: ++n_adj;    break;
                    case MergeReason::SHORT_COMMON_LEFT:   ++n_left;   break;
                    case MergeReason::SHORT_COMMON_RIGHT:  ++n_right;  break;
                    case MergeReason::SHORT_COMMON_BOTH:   break;
                    case MergeReason::ADJACENT_COMMON:     ++n_common; break;
                }
            }
            std::cerr << "[l-EDS] Iter " << iteration
                      << ": " << total_symbols << " symbols, "
                      << total_groups << " groups (" << positions_consumed << " positions consumed)";
            std::cerr << " (";
            bool first = true;
            auto sep = [&]() { if (!first) std::cerr << ", "; first = false; };
            if (n_adj)   { sep(); std::cerr << n_adj   << " adj-degen"; }
            if (n_left)  { sep(); std::cerr << n_left  << " short-ctx←left";  }
            if (n_right) { sep(); std::cerr << n_right << " short-ctx→right"; }
            if (n_common){ sep(); std::cerr << n_common<< " adj-common";      }
            std::cerr << ")\n";
        }

        const bool stderr_tty = isatty(STDERR_FILENO);
        auto print_bar = [&](size_t done) {
            if (!stderr_tty) return;
            const int BAR_WIDTH = 40;
            float frac = total_groups > 0 ? static_cast<float>(done) / total_groups : 1.0f;
            int filled = static_cast<int>(BAR_WIDTH * frac);
            std::cerr << "\r  [";
            for (int i = 0; i < BAR_WIDTH; i++) {
                if (i < filled)       std::cerr << '#';
                else if (i == filled) std::cerr << '>';
                else                  std::cerr << ' ';
            }
            std::cerr << "] " << std::setw(3) << static_cast<int>(frac * 100) << "%"
                      << " (" << done << "/" << total_groups << ")    ";
            std::cerr.flush();
        };

        std::filesystem::path temp_eds_out = temp_dir / ("iter_" + std::to_string(iteration) + ".eds");
        std::ofstream eds_out_stream(temp_eds_out);

        if (!eds_out_stream)
            throw std::runtime_error("Failed to create temp output file: " + temp_eds_out.string());

        // Stream each batch's merge metadata to the writer as computed instead
        // of accumulating the whole iteration in `all_metadata` — caps retained
        // metadata to one BATCH_SIZE window (see the linear path for rationale).
        MergeStreamWriter writer(eds, eds_out_stream, nullptr);

        for (size_t batch_start = 0; batch_start < groups.size(); batch_start += BATCH_SIZE) {
            print_bar(batch_start);

            size_t batch_end = std::min(batch_start + BATCH_SIZE, groups.size());
            std::vector<MergeGroup> batch_groups(
                groups.begin() + batch_start,
                groups.begin() + batch_end
            );

            auto batch_metadata = compute_merge_metadata(eds, batch_groups, num_threads);
            writer.add_batch(batch_metadata);
        }
        print_bar(total_groups);
        if (stderr_tty) std::cerr << "\n";

        auto stream_result = writer.finish();

        eds_out_stream.close();

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

        eds = EDS::from_metadata(std::move(stream_result.metadata),
                                 stream_result.n, stream_result.m, stream_result.N,
                                 temp_eds_out);

        std::filesystem::remove(current_eds_file);
        current_eds_file = temp_eds_out;

        iteration++;
    }

    if (iteration >= MAX_ITERATIONS)
        throw std::runtime_error("Maximum iterations reached without convergence");

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
