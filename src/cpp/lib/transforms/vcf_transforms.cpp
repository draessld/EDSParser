#include "vcf_transforms.hpp"
#include "eds_transforms.hpp"
#include "../formats/eds.hpp"
#include "../common.hpp"
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace edsparser {

// ============================================================================
// HELPER STRUCTURES
// ============================================================================

struct FASTAMetadata {
    std::string seq_name;        // Sequence name (from FASTA header)
    size_t seq_size;             // Total sequence length
    size_t line_width;           // Characters per line (for seeking)
    std::streampos seq_start;    // File position where sequence starts
};

struct VCFVariant {
    std::string chrom;           // Chromosome/sequence name
    size_t pos;                  // Position (1-indexed)
    std::string ref;             // Reference allele
    std::vector<std::string> alts;  // Alternative alleles
    std::vector<std::vector<int>> genotypes;  // Genotype for each sample (ALT indices)
};

struct VariantGroup {
    size_t start_pos;            // 0-indexed start position in reference
    size_t end_pos;              // 0-indexed end position (exclusive)
    std::vector<VCFVariant> variants;  // All variants in this group
    std::vector<std::string> merged_haplotypes;  // All possible haplotype strings
    std::vector<std::vector<int>> merged_genotypes;  // Remapped genotypes per sample
};

// ============================================================================
// FASTA PARSING
// ============================================================================

/**
 * Parse FASTA file to extract metadata for efficient random access.
 * Adapted from old VCFParser::parseFasta()
 */
FASTAMetadata parse_fasta_metadata(std::istream& fasta_stream) {
    FASTAMetadata meta;
    std::string line;

    // Read header line
    if (!std::getline(fasta_stream, line) || line.empty() || line[0] != '>') {
        throw std::runtime_error("Invalid FASTA format: expected header line starting with '>'");
    }

    // Extract sequence name (everything after '>' until first whitespace)
    size_t space_pos = line.find(' ');
    if (space_pos != std::string::npos) {
        meta.seq_name = line.substr(1, space_pos - 1);
    } else {
        meta.seq_name = line.substr(1);
    }

    // Record position where sequence starts
    meta.seq_start = fasta_stream.tellg();

    // Read first sequence line to determine line width
    if (!std::getline(fasta_stream, line)) {
        throw std::runtime_error("FASTA file is empty");
    }
    meta.line_width = line.size();

    // Calculate total sequence size
    meta.seq_size = line.size();
    while (std::getline(fasta_stream, line)) {
        if (line.empty()) continue;
        if (line[0] == '>') break;  // Next sequence
        meta.seq_size += line.size();
    }

    return meta;
}

/**
 * Read a substring from FASTA file using random access.
 * Adapted from old VCFParser::flush_ref()
 *
 * @param fasta_stream FASTA file stream
 * @param meta FASTA metadata
 * @param start_pos 0-indexed start position in sequence
 * @param length Number of bases to read
 * @return Extracted sequence substring
 */
std::string read_fasta_region(std::istream& fasta_stream,
                               const FASTAMetadata& meta,
                               size_t start_pos,
                               size_t length) {
    if (start_pos >= meta.seq_size) {
        return "";
    }

    // Adjust length if it exceeds sequence end
    if (start_pos + length > meta.seq_size) {
        length = meta.seq_size - start_pos;
    }

    // Pre-allocate string to avoid reallocations
    std::string result;
    result.reserve(length);

    // Calculate file position (accounting for newlines)
    std::streamoff file_offset = start_pos + (start_pos / meta.line_width);
    fasta_stream.clear();
    fasta_stream.seekg(meta.seq_start + file_offset);

    // Read characters, skipping newlines
    size_t chars_read = 0;
    char c;
    while (chars_read < length && fasta_stream.get(c)) {
        if (c != '\n' && c != '\r') {
            result += c;  // More efficient than ostringstream::operator<<
            chars_read++;
        }
    }

    return result;
}

// ============================================================================
// VCF PARSING
// ============================================================================

/**
 * Reverse complement a DNA sequence.
 * Used for handling inversions (INV).
 */
std::string reverse_complement(const std::string& seq) {
    std::string result;
    result.reserve(seq.size());

    // Complement mapping
    auto complement = [](char base) -> char {
        switch (std::toupper(base)) {
            case 'A': return 'T';
            case 'T': return 'A';
            case 'C': return 'G';
            case 'G': return 'C';
            case 'N': return 'N';
            default: return base;  // Keep other characters as-is
        }
    };

    // Reverse and complement
    for (auto it = seq.rbegin(); it != seq.rend(); ++it) {
        result += complement(*it);
    }

    return result;
}

/**
 * Parse ALT field to handle special symbolic alleles and multi-allelic sites.
 * Adapted from old VCFParser::check_alt()
 *
 * Returns vector of ALT alleles. Empty string = deletion.
 * Throws runtime_error for unsupported SV types.
 */
std::vector<std::string> parse_alt_field(const std::string& alt_field,
                                          const std::string& ref) {
    std::vector<std::string> alts;

    // Split ALT field by comma (multi-allelic sites)
    std::stringstream ss(alt_field);
    std::string alt_allele;

    while (std::getline(ss, alt_allele, ',')) {
        // Handle symbolic alleles
        if (alt_allele[0] == '<' && alt_allele[alt_allele.size()-1] == '>') {
            std::string sv_type = alt_allele.substr(1, alt_allele.size() - 2);

            if (sv_type == "DEL") {
                // Deletion - empty string
                alts.push_back("");
            }
            else if (sv_type == "INS") {
                // Simple insertion - swap ref/alt semantics
                // VCF: REF=A, ALT=<INS> means insert the REF sequence
                alts.push_back(ref);
            }
            else if (sv_type == "INV") {
                // Inversion - reverse complement of the reference
                alts.push_back(reverse_complement(ref));
            }
            else if (sv_type.substr(0, 2) == "CN") {
                // Copy number variation: CN<N> where N is the number of copies
                try {
                    int copy_number = std::stoi(sv_type.substr(2));

                    if (copy_number == 0) {
                        // CN0 = deletion (0 copies)
                        alts.push_back("");
                    } else if (copy_number == 1) {
                        // CN1 = reference (1 copy, no change)
                        // This is technically the reference allele, but we'll include it
                        alts.push_back(ref);
                    } else {
                        // CN2+ = multiple copies (repeat the reference)
                        std::string repeated;
                        repeated.reserve(ref.size() * copy_number);
                        for (int i = 0; i < copy_number; i++) {
                            repeated += ref;
                        }
                        alts.push_back(repeated);
                    }
                } catch (const std::exception& e) {
                    throw std::runtime_error("Invalid copy number format: " + sv_type);
                }
            }
            else {
                // Unsupported SV type (mobile elements, translocations, etc.)
                throw std::runtime_error("Unsupported structural variant type: " + sv_type);
            }
        }
        else {
            // Regular SNP or indel
            alts.push_back(alt_allele);
        }
    }

    return alts;
}

/**
 * Parse genotype field (FORMAT column) for a single sample.
 * Returns vector of ALT indices (0 = REF, 1 = first ALT, 2 = second ALT, etc.)
 *
 * Examples:
 *   "0|0" -> {0, 0}
 *   "0|1" -> {0, 1}
 *   "1|1" -> {1, 1}
 *   "1|2" -> {1, 2}  (multi-allelic)
 *   "0/1" -> {0, 1}  (unphased, treated same as phased)
 *   ".|." -> {}      (missing, ignored)
 */
std::vector<int> parse_genotype(const std::string& gt_field) {
    std::vector<int> alleles;

    // Determine delimiter: '|' for phased, '/' for unphased
    char delimiter = '|';
    if (gt_field.find('/') != std::string::npos) {
        delimiter = '/';
    }

    // Split on delimiter
    std::stringstream ss(gt_field);
    std::string allele_str;

    while (std::getline(ss, allele_str, delimiter)) {
        if (allele_str == ".") {
            continue;  // Missing genotype
        }
        try {
            alleles.push_back(std::stoi(allele_str));
        } catch (...) {
            // Ignore malformed genotypes
            continue;
        }
    }

    return alleles;
}

/**
 * Enum for tracking why a variant was skipped
 */
enum class SkipReason {
    NONE,              // Not skipped
    HEADER,            // Header or comment line
    MALFORMED,         // Malformed VCF line
    UNSUPPORTED_SV     // Unsupported structural variant
};

/**
 * Parse a single VCF line (non-header) into VCFVariant structure.
 * Returns nullptr if line should be skipped (header, comment, malformed).
 */
std::unique_ptr<VCFVariant> parse_vcf_line(const std::string& line, size_t& n_samples, SkipReason& skip_reason) {
    skip_reason = SkipReason::NONE;
    if (line.empty() || line[0] == '#') {
        // Header or comment line - check if it's the column header
        if (line.substr(0, 6) == "#CHROM") {
            // Count sample columns (everything after FORMAT)
            std::stringstream ss(line);
            std::string token;
            std::vector<std::string> columns;
            while (ss >> token) {
                columns.push_back(token);
            }
            // VCF format: CHROM POS ID REF ALT QUAL FILTER INFO FORMAT [SAMPLE1 SAMPLE2 ...]
            if (columns.size() > 9) {
                n_samples = columns.size() - 9;
            }
        }
        skip_reason = SkipReason::HEADER;
        return nullptr;
    }

    auto var = std::make_unique<VCFVariant>();

    // Parse tab-separated or space-separated fields
    // VCF standard requires tabs, but handle spaces for compatibility
    std::stringstream ss(line);
    std::string token;
    std::vector<std::string> fields;

    // Try tab delimiter first
    while (std::getline(ss, token, '\t')) {
        // Skip empty tokens (from multiple consecutive delimiters)
        if (!token.empty()) {
            fields.push_back(token);
        }
    }

    // If we only got 1 field, the file might be using spaces instead of tabs
    if (fields.size() < 5) {
        fields.clear();
        ss.clear();
        ss.str(line);

        // Split on whitespace
        while (ss >> token) {
            fields.push_back(token);
        }
    }

    if (fields.size() < 5) {
        skip_reason = SkipReason::MALFORMED;
        return nullptr;  // Malformed line
    }

    // Extract core fields
    var->chrom = fields[0];
    try {
        var->pos = std::stoull(fields[1]);  // VCF positions are 1-indexed
    } catch (...) {
        skip_reason = SkipReason::MALFORMED;
        return nullptr;  // Invalid position
    }
    var->ref = fields[3];

    // Parse ALT field
    try {
        var->alts = parse_alt_field(fields[4], var->ref);
    } catch (const std::runtime_error& e) {
        // Unsupported SV type - skip this line
        std::cerr << "Warning: Skipping variant at " << var->chrom << ":" << var->pos
                  << " - " << e.what() << std::endl;
        skip_reason = SkipReason::UNSUPPORTED_SV;
        return nullptr;
    }

    // Parse genotypes (if present)
    if (fields.size() >= 10) {
        // Fields 9+ are sample genotypes
        // Field 8 is FORMAT (e.g., "GT:DP:GQ")
        // We only care about GT (first field)

        for (size_t i = 9; i < fields.size(); i++) {
            // Extract GT field (before first ':')
            std::string gt_field = fields[i];
            size_t colon_pos = gt_field.find(':');
            if (colon_pos != std::string::npos) {
                gt_field = gt_field.substr(0, colon_pos);
            }

            var->genotypes.push_back(parse_genotype(gt_field));
        }
    }

    return var;
}

// ============================================================================
// VARIANT MERGING (for overlapping/same-position variants)
// ============================================================================

/**
 * Check if two variants overlap.
 * Two variants overlap if their reference spans intersect.
 */
bool variants_overlap(const VCFVariant& v1, const VCFVariant& v2) {
    // Convert to 0-indexed positions
    size_t v1_start = v1.pos - 1;
    size_t v1_end = v1_start + v1.ref.size();
    size_t v2_start = v2.pos - 1;
    size_t v2_end = v2_start + v2.ref.size();

    // Check if spans overlap
    return v1_start < v2_end && v2_start < v1_end;
}

/**
 * Apply a variant to a reference string to generate a haplotype.
 *
 * @param ref_span Reference sequence spanning the variant group
 * @param ref_start 0-indexed start position of ref_span in full reference
 * @param variant The variant to apply
 * @param alt_index Index into variant.alts (0 = REF, 1+ = ALT)
 * @return Resulting haplotype string
 */
std::string apply_variant_to_span(
    const std::string& ref_span,
    size_t ref_start,
    const VCFVariant& variant,
    int alt_index)
{
    // alt_index == 0 means reference allele
    if (alt_index == 0) {
        return ref_span;
    }

    // Get the ALT allele (1-indexed in alt_index, 0-indexed in vector)
    if (alt_index < 1 || alt_index > static_cast<int>(variant.alts.size())) {
        return ref_span;  // Invalid index, return reference
    }

    const std::string& alt_allele = variant.alts[alt_index - 1];

    // Calculate where in ref_span this variant starts
    size_t variant_start_0idx = variant.pos - 1;  // Variant position (0-indexed)
    size_t offset_in_span = variant_start_0idx - ref_start;

    // Build haplotype: prefix + alt + suffix
    std::string result;
    result = ref_span.substr(0, offset_in_span);  // Before variant
    result += alt_allele;  // Alt allele (could be empty for deletion)

    // After variant
    size_t after_variant = offset_in_span + variant.ref.size();
    if (after_variant < ref_span.size()) {
        result += ref_span.substr(after_variant);
    }

    return result;
}

/**
 * Merge overlapping variants into a single VariantGroup.
 * Generates all valid haplotypes and remaps sample genotypes.
 */
VariantGroup merge_variant_group(
    const std::vector<VCFVariant>& group_variants,
    const std::string& reference_span,
    size_t span_start)
{
    VariantGroup group;
    group.variants = group_variants;
    group.start_pos = span_start;
    group.end_pos = span_start + reference_span.size();

    // Determine number of samples from first variant
    size_t n_samples = group_variants.empty() ? 0 : group_variants[0].genotypes.size();

    // Initialize merged genotypes (one vector per sample)
    group.merged_genotypes.resize(n_samples);

    // Map: haplotype string -> index in merged_haplotypes
    std::map<std::string, int> haplotype_to_index;

    // Always add reference haplotype first (index 0)
    group.merged_haplotypes.push_back(reference_span);
    haplotype_to_index[reference_span] = 0;

    // For each variant in the group, generate haplotypes for each ALT
    for (size_t var_idx = 0; var_idx < group_variants.size(); var_idx++) {
        const VCFVariant& var = group_variants[var_idx];

        // For each ALT allele
        for (size_t alt_idx = 0; alt_idx < var.alts.size(); alt_idx++) {
            // Generate haplotype by applying this variant to reference span
            std::string haplotype = apply_variant_to_span(
                reference_span, span_start, var, alt_idx + 1);

            // Add to merged haplotypes if not already present
            if (haplotype_to_index.find(haplotype) == haplotype_to_index.end()) {
                haplotype_to_index[haplotype] = group.merged_haplotypes.size();
                group.merged_haplotypes.push_back(haplotype);
            }
        }
    }

    // Remap sample genotypes to merged haplotype indices
    for (size_t sample_idx = 0; sample_idx < n_samples; sample_idx++) {
        std::set<int> sample_haplotype_indices;

        // For each variant in the group
        for (size_t var_idx = 0; var_idx < group_variants.size(); var_idx++) {
            const VCFVariant& var = group_variants[var_idx];

            if (sample_idx >= var.genotypes.size()) {
                continue;  // Sample not in this variant
            }

            const std::vector<int>& genotype = var.genotypes[sample_idx];

            // For each allele in this sample's genotype
            for (int allele_idx : genotype) {
                // Generate the haplotype for this allele
                std::string haplotype = apply_variant_to_span(
                    reference_span, span_start, var, allele_idx);

                // Find its index in merged haplotypes
                auto it = haplotype_to_index.find(haplotype);
                if (it != haplotype_to_index.end()) {
                    sample_haplotype_indices.insert(it->second);
                }
            }
        }

        // If no variants for this sample, they have reference (index 0)
        if (sample_haplotype_indices.empty()) {
            sample_haplotype_indices.insert(0);
        }

        // Convert set to vector
        group.merged_genotypes[sample_idx].assign(
            sample_haplotype_indices.begin(), sample_haplotype_indices.end());
    }

    return group;
}

/**
 * Group variants by overlap.
 * Returns vector of VariantGroups, where each group contains overlapping variants.
 */
std::vector<VariantGroup> group_overlapping_variants(
    const std::vector<VCFVariant>& variants,
    std::istream& fasta_stream,
    const FASTAMetadata& fasta_meta)
{
    std::vector<VariantGroup> groups;

    if (variants.empty()) {
        return groups;
    }

    size_t i = 0;
    while (i < variants.size()) {
        // Start new group with current variant
        std::vector<VCFVariant> current_group;
        current_group.push_back(variants[i]);

        size_t group_start = variants[i].pos - 1;  // 0-indexed
        size_t group_end = group_start + variants[i].ref.size();

        // Find all subsequent variants that overlap with this group
        size_t j = i + 1;
        while (j < variants.size()) {
            const VCFVariant& next_var = variants[j];
            size_t next_start = next_var.pos - 1;
            size_t next_end = next_start + next_var.ref.size();

            // Check if next variant overlaps with current group span
            if (next_start < group_end) {
                // Overlaps! Add to group and extend span
                current_group.push_back(next_var);
                group_end = std::max(group_end, next_end);
                j++;
            } else {
                // No overlap, stop extending this group
                break;
            }
        }

        // Read reference span for this group
        size_t span_length = group_end - group_start;
        std::string ref_span = read_fasta_region(fasta_stream, fasta_meta, group_start, span_length);

        // Merge the group
        VariantGroup merged = merge_variant_group(current_group, ref_span, group_start);
        groups.push_back(merged);

        // Move to next ungrouped variant
        i = j;
    }

    return groups;
}

// ============================================================================
// EDS GENERATION
// ============================================================================

/**
 * Generate EDS and sEDS strings from FASTA + VCF variants.
 * Sample-level source tracking: each sample gets one path ID (1-indexed).
 *
 * Algorithm:
 * 1. Group overlapping variants together
 * 2. For each region between variant groups:
 *    - Flush reference sequence as common symbol: {REF} with sources {0}
 * 3. For each variant group:
 *    - If single variant: create degenerate symbol {REF,ALT1,ALT2,...}
 *    - If multiple overlapping variants: merge into single symbol with all haplotypes
 *    - Sources: {samples_with_haplotype1}{samples_with_haplotype2}...
 * 4. Flush final reference region
 *
 * @param fasta_stream Reference FASTA stream
 * @param fasta_meta FASTA metadata
 * @param variants Vector of variants to process
 * @param n_samples Number of samples
 * @param eds_out Output stream for EDS
 * @param seds_out Output stream for sEDS
 * @param start_pos Starting position in reference (0-indexed)
 * @param end_pos Ending position in reference (0-indexed, exclusive)
 * @return Number of variant groups created (for statistics)
 */
size_t generate_eds_from_variants(
    std::istream& fasta_stream,
    const FASTAMetadata& fasta_meta,
    const std::vector<VCFVariant>& variants,
    [[maybe_unused]] size_t n_samples,
    std::ostream& eds_out,
    std::ostream& seds_out,
    size_t start_pos,
    size_t end_pos)
{

    // Group overlapping variants
    std::vector<VariantGroup> groups = group_overlapping_variants(variants, fasta_stream, fasta_meta);
    size_t num_groups = groups.size();

    size_t current_pos = start_pos;  // Start from provided position

    for (const auto& group : groups) {
        // Flush reference region before this variant group
        if (group.start_pos > current_pos) {
            std::string ref_region = read_fasta_region(fasta_stream, fasta_meta,
                                                        current_pos, group.start_pos - current_pos);
            if (!ref_region.empty()) {
                eds_out << '{' << ref_region << '}';
                seds_out << "{0}";  // Universal path for common regions
            }
            current_pos = group.start_pos;
        }

        // Generate degenerate symbol from merged haplotypes
        eds_out << '{';

        // Build map: haplotype -> set of sample IDs that have it
        std::map<std::string, std::set<int>> haplotype_to_samples;

        // Process each sample's merged genotype
        for (size_t sample_id = 0; sample_id < group.merged_genotypes.size(); sample_id++) {
            int path_id = sample_id + 1;  // 1-indexed path IDs

            const std::vector<int>& genotype = group.merged_genotypes[sample_id];

            // For each haplotype index in this sample's genotype
            for (int haplotype_idx : genotype) {
                if (haplotype_idx >= 0 && haplotype_idx < static_cast<int>(group.merged_haplotypes.size())) {
                    const std::string& haplotype = group.merged_haplotypes[haplotype_idx];
                    haplotype_to_samples[haplotype].insert(path_id);
                }
            }
        }

        // If no samples were tracked, use universal path for all haplotypes
        // (happens when VCF has no genotype columns)
        if (haplotype_to_samples.empty()) {
            for (size_t i = 0; i < group.merged_haplotypes.size(); i++) {
                eds_out << group.merged_haplotypes[i];
                if (i < group.merged_haplotypes.size() - 1) {
                    eds_out << ',';
                }
            }
            eds_out << '}';
            seds_out << "{0}";  // Universal path

            current_pos = group.end_pos;
            continue;
        }

        // Output haplotypes in order they appear in merged_haplotypes
        // (reference is always first, then ALTs)
        std::vector<std::pair<std::string, std::set<int>>> ordered_haplotypes;
        for (const auto& haplotype : group.merged_haplotypes) {
            if (haplotype_to_samples.find(haplotype) != haplotype_to_samples.end()) {
                ordered_haplotypes.push_back({haplotype, haplotype_to_samples[haplotype]});
            }
        }

        // Output EDS symbol
        for (size_t i = 0; i < ordered_haplotypes.size(); i++) {
            eds_out << ordered_haplotypes[i].first;
            if (i < ordered_haplotypes.size() - 1) {
                eds_out << ',';
            }
        }
        eds_out << '}';

        // Output sources
        for (const auto& [haplotype, samples] : ordered_haplotypes) {
            seds_out << '{';
            if (samples.empty()) {
                // No samples have this haplotype - should not happen, but use universal path
                seds_out << '0';
            } else {
                auto it = samples.begin();
                for (size_t i = 0; i < samples.size(); i++, it++) {
                    seds_out << *it;
                    if (i < samples.size() - 1) {
                        seds_out << ',';
                    }
                }
            }
            seds_out << '}';
        }

        // Update position to end of this variant group
        current_pos = group.end_pos;
    }

    // Flush remaining reference sequence up to end_pos
    size_t flush_end = std::min(end_pos, fasta_meta.seq_size);
    if (current_pos < flush_end) {
        std::string ref_region = read_fasta_region(fasta_stream, fasta_meta,
                                                    current_pos, flush_end - current_pos);
        if (!ref_region.empty()) {
            eds_out << '{' << ref_region << '}';
            seds_out << "{0}";
        }
    }

    return num_groups;
}

// ============================================================================
// PUBLIC API FUNCTIONS
// ============================================================================

/**
 * Parse VCF + FASTA to EDS with source tracking (file stream output).
 */
void parse_vcf_to_eds_streaming(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    std::ostream& eds_output,
    std::ostream& seds_output,
    VCFStats* stats,
    size_t block_size)
{
    // Step 1: Parse FASTA metadata
    FASTAMetadata fasta_meta = parse_fasta_metadata(fasta_stream);

    size_t n_samples = 0;
    size_t total_variant_groups = 0;

    // If block_size is 0, use old behavior (load all variants)
    // Otherwise, if sequence is shorter than block size, process as single block
    if (block_size == 0) {
        block_size = fasta_meta.seq_size;  // Legacy mode: load all
    } else if (fasta_meta.seq_size < block_size) {
        block_size = fasta_meta.seq_size;  // Sequence smaller than block: process all
    }

    // Block-based processing
    std::vector<VCFVariant> block_variants;
    std::vector<VCFVariant> carryover_variants;  // Variants that extend beyond current block
    std::string line;

    size_t current_block_start = 0;
    size_t current_block_end = block_size;

    // Maximum overlap margin - variants can extend beyond their start position
    // We use a conservative estimate: max variant length we expect
    const size_t OVERLAP_MARGIN = 1000000;  // 1MB margin for large structural variants

    bool vcf_finished = false;

    while (current_block_start < fasta_meta.seq_size) {
        // Start with variants from carryover (already read from VCF but belong to this block)
        block_variants = carryover_variants;
        carryover_variants.clear();

        // Read more VCF lines for this block (if not finished)
        if (!vcf_finished) {
            while (std::getline(vcf_stream, line)) {
                SkipReason skip_reason;
                auto var = parse_vcf_line(line, n_samples, skip_reason);

                // Track statistics
                if (stats) {
                    if (skip_reason == SkipReason::NONE) {
                        stats->total_variants++;
                        stats->processed_variants++;
                    } else if (skip_reason == SkipReason::MALFORMED) {
                        stats->total_variants++;
                        stats->skipped_malformed++;
                    } else if (skip_reason == SkipReason::UNSUPPORTED_SV) {
                        stats->total_variants++;
                        stats->skipped_unsupported_sv++;
                    }
                }

                if (var) {
                    size_t var_start = var->pos - 1;  // 0-indexed

                    // Check if variant starts beyond current block end
                    if (var_start >= current_block_end) {
                        // This variant belongs to future blocks
                        // Put it in carryover and stop reading for this block
                        carryover_variants.push_back(*var);
                        break;
                    }

                    // Add to current block (variant starts in this block)
                    block_variants.push_back(*var);
                }
            }

            // Check if we've finished reading VCF
            if (vcf_stream.eof() || vcf_stream.fail()) {
                vcf_finished = true;
            }
        }

        // Sort block variants by position (should mostly be sorted already from VCF)
        std::sort(block_variants.begin(), block_variants.end(),
                  [](const VCFVariant& a, const VCFVariant& b) {
                      return a.pos < b.pos;
                  });

        // Generate EDS for this block and count groups
        size_t block_groups = generate_eds_from_variants(fasta_stream, fasta_meta, block_variants, n_samples,
                                                          eds_output, seds_output,
                                                          current_block_start, current_block_end);

        // Flush output to disk after each block (prevent memory accumulation)
        eds_output.flush();
        seds_output.flush();

        // Accumulate variant groups for statistics
        if (stats) {
            total_variant_groups += block_groups;
        }

        // Clear block variants to free memory
        block_variants.clear();

        // Move to next block
        current_block_start = current_block_end;
        current_block_end = std::min(current_block_start + block_size, fasta_meta.seq_size);

        // If we've processed all VCF variants and reached the end, we're done
        if (vcf_finished && carryover_variants.empty() && current_block_start >= fasta_meta.seq_size) {
            break;
        }
    }

    // Update variant groups count
    if (stats) {
        stats->variant_groups = total_variant_groups;
    }

    // Final flush
    eds_output.flush();
    seds_output.flush();
}

/**
 * Parse VCF + FASTA to EDS with source tracking (string return wrapper).
 * For large files, prefer the file stream version to avoid memory accumulation.
 */
std::pair<std::string, std::string> parse_vcf_to_eds_streaming_str(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    VCFStats* stats,
    size_t block_size)
{
    // Use stringstreams for backward compatibility
    std::ostringstream eds_output;
    std::ostringstream seds_output;

    // Call file stream version
    parse_vcf_to_eds_streaming(vcf_stream, fasta_stream, eds_output, seds_output, stats, block_size);

    return {eds_output.str(), seds_output.str()};
}

/**
 * Parse VCF + FASTA to l-EDS with source tracking (file stream output).
 *
 * Memory-efficient version that uses temporary files for the two-stage pipeline:
 * VCF→EDS (temp file) → l-EDS (output file)
 *
 * This prevents accumulating the full EDS string in memory, which can be very large
 * for population-scale VCF files.
 *
 * @param vcf_stream Input stream containing VCF file
 * @param fasta_stream Input stream containing reference FASTA
 * @param leds_output Output stream for l-EDS (written directly)
 * @param seds_output Output stream for sEDS (written directly)
 * @param context_length Minimum context length for l-EDS
 * @param stats Optional pointer to VCFStats structure to receive statistics
 * @param block_size Genomic window size in bases (0 = load all, default 10M)
 */
void parse_vcf_to_leds_streaming_direct(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    std::ostream& leds_output,
    std::ostream& seds_output,
    size_t context_length,
    VCFStats* stats,
    size_t block_size)
{
    // Create temporary files for intermediate EDS/sEDS
    // Use std::filesystem::temp_directory_path() for portability
    auto temp_dir = std::filesystem::temp_directory_path();
    auto temp_eds_path = temp_dir / ("edsparser_temp_eds_" + std::to_string(std::time(nullptr)) + ".tmp");
    auto temp_seds_path = temp_dir / ("edsparser_temp_seds_" + std::to_string(std::time(nullptr)) + ".tmp");

    try {
        // Step 1: VCF → EDS (write to temp files)
        {
            std::ofstream temp_eds(temp_eds_path);
            std::ofstream temp_seds(temp_seds_path);

            if (!temp_eds || !temp_seds) {
                throw std::runtime_error("Failed to create temporary files for l-EDS transformation");
            }

            parse_vcf_to_eds_streaming(vcf_stream, fasta_stream, temp_eds, temp_seds, stats, block_size);

            temp_eds.close();
            temp_seds.close();
        }

        // Step 2: EDS → l-EDS (read from temp files, write to output)
        {
            std::ifstream temp_eds(temp_eds_path);
            std::ifstream temp_seds(temp_seds_path);

            if (!temp_eds || !temp_seds) {
                throw std::runtime_error("Failed to open temporary files for l-EDS transformation");
            }

            eds_to_leds_linear(temp_eds, leds_output, context_length,
                             &temp_seds, &seds_output);

            temp_eds.close();
            temp_seds.close();
        }

        // Clean up temporary files
        std::filesystem::remove(temp_eds_path);
        std::filesystem::remove(temp_seds_path);

    } catch (...) {
        // Clean up temporary files on error
        std::filesystem::remove(temp_eds_path);
        std::filesystem::remove(temp_seds_path);
        throw;
    }
}

/**
 * Parse VCF + FASTA to l-EDS with source tracking (string return wrapper).
 *
 * WARNING: For large files, this accumulates entire output in memory.
 * Prefer parse_vcf_to_leds_streaming_direct() for production use.
 *
 * Uses two-pass approach: VCF→EDS→l-EDS
 */
std::pair<std::string, std::string> parse_vcf_to_leds_streaming(
    std::istream& vcf_stream,
    std::istream& fasta_stream,
    size_t context_length,
    VCFStats* stats,
    size_t block_size)
{
    // Use stringstreams (accumulates in memory - not recommended for large files)
    std::ostringstream leds_output;
    std::ostringstream seds_output;

    // Call the streaming version
    parse_vcf_to_leds_streaming_direct(vcf_stream, fasta_stream, leds_output, seds_output,
                                       context_length, stats, block_size);

    return {leds_output.str(), seds_output.str()};
}

} // namespace edsparser
