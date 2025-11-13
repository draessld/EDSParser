#ifndef EDSPARSER_TRANSFORMS_VCF_TRANSFORMS_HPP
#define EDSPARSER_TRANSFORMS_VCF_TRANSFORMS_HPP

#include "../common.hpp"
#include <iostream>
#include <string>
#include <utility>

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
    size_t variant_groups = 0;        // Number of variant groups created (after merging overlaps)

    // Helper to get total skipped count
    size_t total_skipped() const {
        return skipped_malformed + skipped_unsupported_sv;
    }
};

/**
 * Parse VCF + FASTA reference to EDS with source tracking.
 *
 * Source tracking: Sample-level (diploid samples contribute to one path).
 * Path IDs are 1-indexed, matching sample order in VCF.
 *
 * Handles:
 * - SNPs and small indels
 * - Simple deletions (<DEL>)
 * - Simple insertions (<INS>)
 * - Multi-allelic sites (multiple ALT alleles)
 *
 * Skips with warnings:
 * - Overlapping variants
 * - Complex structural variants (<INV>, <CN*>, mobile elements)
 * - Malformed VCF lines
 *
 * @param vcf_stream Input stream containing VCF file
 * @param fasta_stream Input stream containing reference FASTA
 * @param stats Optional pointer to VCFStats structure to receive statistics
 * @return Pair of (EDS string, sEDS source string)
 */
std::pair<std::string, std::string> parse_vcf_to_eds_streaming(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    VCFStats* stats = nullptr);

/**
 * Parse VCF + FASTA reference directly to l-EDS with source tracking.
 *
 * Uses existing parse_vcf_to_eds_streaming() + eds_to_leds_linear() pipeline.
 *
 * @param vcf_stream Input stream containing VCF file
 * @param fasta_stream Input stream containing reference FASTA
 * @param context_length Minimum context length for l-EDS
 * @param stats Optional pointer to VCFStats structure to receive statistics
 * @return Pair of (l-EDS string, sEDS source string)
 */
std::pair<std::string, std::string> parse_vcf_to_leds_streaming(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    size_t context_length,
    VCFStats* stats = nullptr);

} // namespace edsparser

#endif // EDSPARSER_TRANSFORMS_VCF_TRANSFORMS_HPP
