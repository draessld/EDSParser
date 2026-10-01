// VCF plus reference FASTA to EDS or l-EDS, with one source path per sample.
//
// Handles SNPs, indels, and the symbolic alleles <DEL>, <INS>, <INV> and <CNn>.
// Variants are processed in genomic blocks so peak memory tracks the block, not
// the VCF; the reference is read by random access and never held whole.
#ifndef EDSPARSER_TRANSFORMS_VCF_TRANSFORMS_HPP
#define EDSPARSER_TRANSFORMS_VCF_TRANSFORMS_HPP

#include "../common.hpp"
#include "../formats/sources.hpp"
#include <iostream>
#include <string>
#include <utility>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace edsparser {

/**
 * VCF Transformation Functions
 *
 * This module provides transformations from VCF (Variant Call Format) files
 * to Elastic-Degenerate Strings (EDS) and length-constrained EDS (l-EDS).
 *
 * Uses streaming approach for FASTA reference - only active regions loaded.
 * Sources track samples at the sample level (one path per sample).
 */

/**
 * Statistics for VCF parsing and transformation.
 */
struct VCFStats {
    size_t total_variants = 0;        // Total variant lines processed (excluding headers)
    size_t processed_variants = 0;    // Successfully processed variants
    size_t skipped_malformed = 0;     // Skipped due to malformed VCF lines
    size_t skipped_unsupported_sv = 0;  // Skipped due to unsupported SV types
    size_t skipped_wrong_chrom = 0;   // Skipped: chromosome doesn't match FASTA reference
    size_t skipped_out_of_range = 0;  // Skipped: POS lies beyond the end of the reference
    size_t variant_groups = 0;        // Number of variant groups created (after merging overlaps)

    // Variants whose REF field disagrees with the reference FASTA at that
    // position. NOT a skip count: the variant is still processed, because real
    // VCFs contain genuine mismatches. It is a data-quality signal — a nonzero
    // value usually means the VCF and the FASTA are different assemblies, in
    // which case every span is wrong. Spans containing N are not compared.
    size_t ref_mismatches = 0;
    size_t ref_checked = 0;           // Variants whose REF could actually be compared

    // ALT calls ignored because the same allele copy already carries an ALT at
    // an overlapping record in its group. One chromosome cannot carry both, so
    // the first record in file order applies and the later one does not — what
    // `bcftools consensus` does. NOT a skip count: the record is still emitted
    // for every other copy. Nonzero means the VCF makes contradictory calls;
    // normalise it first if the genomes must match their assemblies exactly.
    size_t overlap_conflicts = 0;

    // Where vcf2eds and `bcftools consensus -s <sample>` spell a genome
    // differently (2026-10-01). The two tools resolve calls at overlapping
    // records by different rules: vcf2eds applies the first ALT a copy carries
    // in file order and lets REF/missing calls block nothing; bcftools lets any
    // non-missing call, REF included, claim its REF span and skips a later
    // record starting inside it, except a pure indel anchored on the last
    // claimed base that does not follow an insertion. For every allele copy
    // with calls in a group of two or more records, both rules are applied to
    // the group's span and the haplotypes compared; only copies whose spelling
    // actually differs are counted. The conversion itself is unchanged.
    size_t overlap_divergent_copies = 0;   // (allele copy, group) pairs that differ
    size_t overlap_divergent_groups = 0;   // groups holding at least one
    size_t overlap_divergent_records = 0;  // records in those groups
    std::vector<size_t> overlap_divergent_samples;   // 0-based sample indices, sorted
    std::vector<std::string> overlap_divergence_examples;  // first few, human-readable

    // Helper to get total skipped count
    size_t total_skipped() const {
        return skipped_malformed + skipped_unsupported_sv + skipped_wrong_chrom +
               skipped_out_of_range;
    }
};

/**
 * Thrown at the end of the VCF -> EDS stage when `strict_overlaps` is set and
 * some genome would differ from `bcftools consensus` (see VCFStats). what() is
 * format_overlap_divergence().
 */
struct OverlapDivergenceError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * The divergence summary (counts plus the first examples) vcf2eds prints as a
 * warning and --strict-overlaps reports as its error. Empty when there is none.
 */
std::string format_overlap_divergence(const VCFStats& stats);

/**
 * Parse VCF + FASTA reference to EDS with source tracking (file stream output).
 *
 * Memory-efficient version that writes output directly to file streams.
 * Recommended for large VCF files to avoid output accumulation in memory.
 *
 * @param vcf_stream Input stream containing VCF file
 * @param fasta_stream Input stream containing reference FASTA
 * @param eds_output Output stream for EDS (written incrementally per block)
 * @param seds_output Output stream for sEDS (written incrementally per block)
 * @param stats Optional pointer to VCFStats structure to receive statistics
 * @param block_size Genomic window size in bases (0 = load all, default 10M)
 * @param split_groups Emit a group of overlapping records as one symbol per
 *        atomic segment of its span instead of one symbol of full-span
 *        haplotypes (`vcf2eds --split-groups`). Same LINEAR language, same
 *        source partition, far smaller EDS when long deletions overlap
 *        polymorphic sites; see merge_variant_group() in the .cpp.
 * @param strict_overlaps Throw OverlapDivergenceError, once the EDS has been
 *        written, if any genome differs from `bcftools consensus`
 *        (`vcf2eds --strict-overlaps`). Without it the counts are only reported
 *        in `stats`.
 */
void parse_vcf_to_eds_streaming(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    std::ostream& eds_output,
    std::ostream& seds_output,
    VCFStats* stats = nullptr,
    size_t block_size = 10000000,
    Sources::Format seds_format = Sources::Format::SEDS,
    bool split_groups = false,
    bool strict_overlaps = false);

/**
 * Parse VCF + FASTA reference to EDS with source tracking (string return).
 *
 * Convenience wrapper that returns strings. For large files, prefer the
 * file stream version to avoid memory accumulation.
 *
 * Source tracking: Sample-level (diploid samples contribute to one path).
 * Path IDs are 1-indexed, matching sample order in VCF.
 *
 * Handles:
 * - SNPs and small indels
 * - Simple deletions (<DEL>)
 * - Simple insertions (<INS>)
 * - Inversions (<INV>)
 * - Copy number variations (<CN0>, <CN1>, <CN2>, etc.)
 * - Multi-allelic sites (multiple ALT alleles)
 *
 * Skips with warnings:
 * - Overlapping variants
 * - Complex structural variants (translocations, mobile elements, etc.)
 * - Malformed VCF lines
 *
 * Memory optimization: Uses block-based processing to limit memory usage.
 * Block size determines genomic window size (default 10M bases).
 *
 * @param vcf_stream Input stream containing VCF file
 * @param fasta_stream Input stream containing reference FASTA
 * @param stats Optional pointer to VCFStats structure to receive statistics
 * @param block_size Genomic window size in bases (0 = load all, default 10M)
 * @return Pair of (EDS string, sEDS source string)
 */
std::pair<std::string, std::string> parse_vcf_to_eds_streaming_str(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    VCFStats* stats = nullptr,
    size_t block_size = 10000000,
    bool split_groups = false);

/**
 * Parse VCF + FASTA reference directly to l-EDS with source tracking (file stream output).
 *
 * Memory-efficient version that uses temporary files for the two-stage pipeline.
 * Recommended for large VCF files to avoid accumulating full EDS in memory.
 *
 * Pipeline: VCF → EDS (temp file) → l-EDS (output stream)
 *
 * By default the intermediate EDS/SEDS are throwaway temp files deleted on exit.
 * Pass keep_eds_path / keep_seds_path to materialise the stage-1 EDS/SEDS to
 * those locations instead (they survive the run) — used by `vcf2eds --keep-eds`
 * to emit both the plain EDS and the l-EDS in a single pass. Each path is
 * independent; a null pointer falls back to a temp file for that stage-1 output.
 *
 * @param vcf_stream Input stream containing VCF file
 * @param fasta_stream Input stream containing reference FASTA
 * @param leds_output Output stream for l-EDS (written directly)
 * @param seds_output Output stream for sEDS (written directly)
 * @param context_length Minimum context length for l-EDS
 * @param stats Optional pointer to VCFStats structure to receive statistics
 * @param block_size Genomic window size in bases (0 = load all, default 10M)
 * @param keep_eds_path  If non-null, write the intermediate EDS here (kept)
 * @param keep_seds_path If non-null, write the intermediate SEDS here (kept)
 */
void parse_vcf_to_leds_streaming_direct(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    std::ostream& leds_output,
    std::ostream& seds_output,
    size_t context_length,
    VCFStats* stats = nullptr,
    size_t block_size = 10000000,
    const std::filesystem::path* keep_eds_path = nullptr,
    const std::filesystem::path* keep_seds_path = nullptr,
    bool split_groups = false,
    bool strict_overlaps = false);

} // namespace edsparser

#endif // EDSPARSER_TRANSFORMS_VCF_TRANSFORMS_HPP
