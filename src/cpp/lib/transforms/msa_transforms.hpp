#ifndef EDSPARSER_TRANSFORMS_MSA_TRANSFORMS_HPP
#define EDSPARSER_TRANSFORMS_MSA_TRANSFORMS_HPP

#include "../common.hpp"
#include <iostream>
#include <string>
#include <utility>

namespace edsparser {

/**
 * MSA Transformation Functions
 *
 * This module provides transformations from Multiple Sequence Alignments (MSA)
 * to Elastic-Degenerate Strings (EDS) and length-constrained EDS (l-EDS).
 *
 * Uses streaming approach - only reference sequence kept in memory.
 */

/**
 * Parse MSA (Multiple Sequence Alignment) to EDS with source tracking.
 * Uses streaming approach - only reference sequence kept in memory.
 * Writes directly to output streams for memory efficiency.
 *
 * @param msa_stream Input stream containing MSA in FASTA format (with gaps as '-')
 * @param eds_out Output stream for EDS data
 * @param seds_out Output stream for source tracking data
 */
void parse_msa_to_eds_streaming(
    std::istream& msa_stream,
    std::ostream& eds_out,
    std::ostream& seds_out);

/**
 * Parse MSA directly to l-EDS with source tracking.
 * Uses streaming approach with merging based on context length.
 * Writes directly to output streams for memory efficiency.
 *
 * @param msa_stream Input stream containing MSA in FASTA format (with gaps as '-')
 * @param eds_out Output stream for l-EDS data
 * @param seds_out Output stream for source tracking data
 * @param context_length Minimum context length for l-EDS
 */
void parse_msa_to_leds_streaming(
    std::istream& msa_stream,
    std::ostream& eds_out,
    std::ostream& seds_out,
    size_t context_length);

} // namespace edsparser

#endif // EDSPARSER_TRANSFORMS_MSA_TRANSFORMS_HPP
