#ifndef EDSPARSER_TRANSFORMS_VCF_TRANSFORMS_HPP
#define EDSPARSER_TRANSFORMS_VCF_TRANSFORMS_HPP

#include "../common.hpp"
#include "../formats/sources.hpp"
#include <iostream>
#include <string>
#include <utility>
#include <filesystem>

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

    // Helper to get total skipped count
    size_t total_skipped() const {
        return skipped_malformed + skipped_unsupported_sv + skipped_wrong_chrom +
               skipped_out_of_range;
    }
};

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
 */
void parse_vcf_to_eds_streaming(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    std::ostream& eds_output,
    std::ostream& seds_output,
    VCFStats* stats = nullptr,
    size_t block_size = 10000000,
    Sources::Format seds_format = Sources::Format::SEDS);

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
    size_t block_size = 10000000);

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
    const std::filesystem::path* keep_seds_path = nullptr);

/**
 * Parse VCF + FASTA reference directly to l-EDS with source tracking (string return).
 *
 * WARNING: For large files, this accumulates entire output in memory.
 * Prefer parse_vcf_to_leds_streaming_direct() for production use.
 *
 * Uses existing parse_vcf_to_eds_streaming() + eds_to_leds_linear() pipeline.
 *
 * @param vcf_stream Input stream containing VCF file
 * @param fasta_stream Input stream containing reference FASTA
 * @param context_length Minimum context length for l-EDS
 * @param stats Optional pointer to VCFStats structure to receive statistics
 * @param block_size Genomic window size in bases (0 = load all, default 10M)
 * @return Pair of (l-EDS string, sEDS source string)
 */
std::pair<std::string, std::string> parse_vcf_to_leds_streaming(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    size_t context_length,
    VCFStats* stats = nullptr,
    size_t block_size = 10000000);

} // namespace edsparser

#endif // EDSPARSER_TRANSFORMS_VCF_TRANSFORMS_HPP
