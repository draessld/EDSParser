#include "vcf_transforms.hpp"
#include "eds_transforms.hpp"
#include "../formats/eds.hpp"
#include "../common.hpp"
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <vector>
#include <array>
#include <string_view>
#include <algorithm>
#include <stdexcept>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <unistd.h>

namespace edsparser {

// ============================================================================
// SEDS WRITE HELPERS
// ============================================================================
//
// Format: {path_ids_or_ranges}
//   Ranges:     {1-3,7}      → paths 1,2,3,7
//   Complement: {0,5-10}     → all paths EXCEPT 5..10  (0 = complement sentinel)
//   Universal:  {0}          → all paths
//
// write_seds_entry() chooses complement encoding when the set has >50% of all
// paths, because the exception list is then smaller than the membership list.

static void write_ranges(std::ostream& out, const std::set<int>& ids, bool prefix_comma = false) {
    bool first = !prefix_comma;
    auto it = ids.begin();
    while (it != ids.end()) {
        int lo = *it, hi = lo;
        while (std::next(it) != ids.end() && *std::next(it) == hi + 1) { ++it; ++hi; }
        if (!first) out << ',';
        first = false;
        if (hi > lo + 1) {          // 3+ consecutive: use a-b range notation
            out << lo << '-' << hi;
        } else if (hi == lo + 1) {  // exactly 2: a,b (range saves nothing)
            out << lo << ',' << hi;
        } else {
            out << lo;
        }
        ++it;
    }
}

static void write_seds_entry(std::ostream& out, const std::set<int>& paths, size_t total_paths) {
    out << '{';
    if (paths.empty()) {
        // Should not happen; fall back to universal rather than an empty set
        out << '0';
    } else if (paths.size() > total_paths / 2) {
        // Complement form: {0, exceptions...}
        // Compute the exceptions (all path IDs in 1..total_paths not in paths)
        std::set<int> exceptions;
        for (int p = 1; p <= static_cast<int>(total_paths); ++p)
            if (!paths.count(p)) exceptions.insert(p);
        out << '0';
        if (!exceptions.empty()) write_ranges(out, exceptions, /*prefix_comma=*/true);
    } else {
        write_ranges(out, paths);
    }
    out << '}';
}

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
    // Flat diploid genotypes: [s0_a0, s0_a1, s1_a0, s1_a1, ...]; -1 = missing/hemizygous pad
    // n_samples = genotypes.size() / 2
    std::vector<int> genotypes;
};

struct VariantGroup {
    size_t start_pos;            // 0-indexed start position in reference
    size_t end_pos;              // 0-indexed end position (exclusive)
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

    // Seek to the start of the requested region.
    // file_offset accounts for the '\n' after each line_width-char FASTA line.
    std::streamoff file_offset = start_pos + (start_pos / meta.line_width);
    fasta_stream.clear();
    fasta_stream.seekg(meta.seq_start + file_offset);

    // Read in segments aligned to FASTA line boundaries.
    // Each segment fits within one line so no newlines appear inside a read().
    // This replaces the old char-at-a-time get() loop — O(length/line_width)
    // read() calls instead of O(length) get() calls.
    std::string result(length, '\0');
    size_t chars_read = 0;
    size_t cur_seq_pos = start_pos;

    while (chars_read < length && fasta_stream.good()) {
        size_t line_off = cur_seq_pos % meta.line_width;
        size_t to_read  = std::min(length - chars_read, meta.line_width - line_off);

        fasta_stream.read(&result[chars_read], static_cast<std::streamsize>(to_read));
        std::streamsize n = fasta_stream.gcount();
        if (n <= 0) break;

        chars_read    += static_cast<size_t>(n);
        cur_seq_pos   += static_cast<size_t>(n);

        // If we consumed exactly to the end of this FASTA line and still need
        // more chars, skip the '\n' separator before the next line.
        if (static_cast<size_t>(n) == meta.line_width - line_off && chars_read < length) {
            char nl;
            fasta_stream.get(nl);  // skip '\n' (or first byte of '\r\n')
            if (nl == '\r' && fasta_stream.good() && fasta_stream.peek() == '\n')
                fasta_stream.get(nl);  // skip second byte of Windows CRLF
        }
    }

    result.resize(chars_read);
    return result;
}

// ============================================================================
// CHROMOSOME NAME MATCHING
// ============================================================================

// Return true if two chromosome names refer to the same sequence.
// Handles the common "chr1" vs "1" and "chrX" vs "X" mismatches between UCSC
// (chr-prefixed) and Ensembl (bare) reference naming conventions.
static bool chrom_matches(const std::string& a, const std::string& b) {
    if (a == b) return true;
    auto strip_chr = [](const std::string& s) -> std::string {
        if (s.size() > 3 &&
            (s[0]=='c'||s[0]=='C') &&
            (s[1]=='h'||s[1]=='H') &&
            (s[2]=='r'||s[2]=='R'))
            return s.substr(3);
        return s;
    };
    return strip_chr(a) == strip_chr(b);
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
 *
 * Uses single-pass parsing with string_view for efficiency.
 */
std::unique_ptr<VCFVariant> parse_vcf_line(const std::string& line, size_t& n_samples, SkipReason& skip_reason) {
    skip_reason = SkipReason::NONE;

    // Quick check for empty line or header
    if (line.empty() || line[0] == '#') {
        // Header or comment line - check if it's the column header
        if (line.size() >= 6 && line[0] == '#' && line[1] == 'C') {
            std::string_view sv(line);
            if (sv.substr(0, 6) == "#CHROM") {
                // Count sample columns by counting tabs after first 8 fields
                size_t tab_count = 0;
                for (char c : line) {
                    if (c == '\t') tab_count++;
                }
                // VCF: CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tSAMPLE...
                // = 8 tabs for 9 fixed columns, then 1 tab per sample
                if (tab_count >= 9) {
                    n_samples = tab_count - 8;
                } else {
                    // Fallback: space-delimited header (non-standard but accepted by some tools)
                    size_t token_count = 0;
                    bool in_tok = false;
                    for (char c : line) {
                        bool ws = (c == ' ' || c == '\t');
                        if (!ws && !in_tok) { in_tok = true; ++token_count; }
                        else if (ws)          in_tok = false;
                    }
                    if (token_count > 9) n_samples = token_count - 9;
                }
            }
        }
        skip_reason = SkipReason::HEADER;
        return nullptr;
    }

    // Single-pass field extraction using string_view
    std::array<std::string_view, 16> fields;  // VCF has 9 fixed + up to 7 samples for initial parse
    size_t field_count = 0;
    size_t pos = 0;
    const size_t len = line.size();
    char delimiter = '\t';

    // First pass: extract fields with tab delimiter
    while (pos < len && field_count < fields.size()) {
        size_t next = line.find(delimiter, pos);
        if (next == std::string::npos) next = len;

        if (next > pos) {  // Skip empty fields
            fields[field_count++] = std::string_view(line.data() + pos, next - pos);
        }
        pos = next + 1;
    }

    // If fewer than 5 fields found with tabs, try space delimiter
    if (field_count < 5) {
        field_count = 0;
        pos = 0;
        while (pos < len && field_count < fields.size()) {
            // Skip leading whitespace
            while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;
            if (pos >= len) break;

            // Find end of field
            size_t start = pos;
            while (pos < len && line[pos] != ' ' && line[pos] != '\t') pos++;

            fields[field_count++] = std::string_view(line.data() + start, pos - start);
        }
    }

    if (field_count < 5) {
        skip_reason = SkipReason::MALFORMED;
        return nullptr;  // Malformed line
    }

    auto var = std::make_unique<VCFVariant>();

    // Extract core fields (convert string_view to string where needed)
    var->chrom = std::string(fields[0]);

    try {
        // Convert string_view to string for stoull (required by some compilers)
        var->pos = std::stoull(std::string(fields[1]));  // VCF positions are 1-indexed
    } catch (...) {
        skip_reason = SkipReason::MALFORMED;
        return nullptr;  // Invalid position
    }

    var->ref = std::string(fields[3]);

    // Parse ALT field
    try {
        var->alts = parse_alt_field(std::string(fields[4]), var->ref);
    } catch (const std::runtime_error& e) {
        // Unsupported SV type - skip this line
        std::cerr << "Warning: Skipping variant at " << var->chrom << ":" << var->pos
                  << " - " << e.what() << std::endl;
        skip_reason = SkipReason::UNSUPPORTED_SV;
        return nullptr;
    }

    // Parse remaining genotype fields if present.
    // Flat diploid layout: 2 ints per sample; -1 = missing or hemizygous pad.
    if (field_count >= 10) {
        var->genotypes.reserve((field_count - 9) * 2);

        // Helper: push one GT string_view as 2 flat ints
        auto push_gt = [&](std::string_view gt_sv) {
            // Extract GT field (before first ':')
            size_t colon_pos = gt_sv.find(':');
            if (colon_pos != std::string::npos)
                gt_sv = gt_sv.substr(0, colon_pos);
            // Parse allele indices (stop after 2)
            int a[2] = {-1, -1};
            int n = 0;
            for (size_t i = 0; i <= gt_sv.size() && n < 2; i++) {
                char c = (i < gt_sv.size()) ? gt_sv[i] : '\0';
                if (c == '|' || c == '/' || c == '\0') {
                    // end of an allele token — nothing to push yet
                } else if (c >= '0' && c <= '9') {
                    if (a[n] < 0) a[n] = 0;
                    a[n] = a[n] * 10 + (c - '0');
                    continue;
                } else if (c != '.') {
                    continue;
                }
                // Token complete
                n++;
            }
            var->genotypes.push_back(a[0]);
            var->genotypes.push_back(a[1]);
        };

        for (size_t i = 9; i < field_count; i++)
            push_gt(fields[i]);

        // Continue parsing if there are more samples beyond the initial array
        if (field_count == fields.size()) {
            while (pos < len) {
                while (pos < len && (line[pos] == '\t' || line[pos] == ' ')) pos++;
                if (pos >= len) break;
                size_t start = pos;
                while (pos < len && line[pos] != '\t' && line[pos] != ' ') pos++;
                push_gt(std::string_view(line.data() + start, pos - start));
            }
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
    group.start_pos = span_start;
    group.end_pos = span_start + reference_span.size();

    // Flat layout: 2 ints per sample; n_samples = genotypes.size() / 2
    size_t n_samples = group_variants.empty() ? 0 : group_variants[0].genotypes.size() / 2;

    // Initialize merged genotypes (one vector per sample)
    group.merged_genotypes.resize(n_samples);

    // Use unordered_map for faster lookups (O(1) average) in this hot loop
    std::unordered_map<std::string, int> haplotype_to_index;

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
            size_t var_n_samples = var.genotypes.size() / 2;

            if (sample_idx >= var_n_samples) {
                continue;  // Sample not in this variant
            }

            // Flat layout: 2 ints per sample at [sample_idx*2] and [sample_idx*2+1]
            for (int a = 0; a < 2; a++) {
                int allele_idx = var.genotypes[sample_idx * 2 + a];
                if (allele_idx < 0) continue;  // missing / hemizygous pad

                std::string haplotype = apply_variant_to_span(
                    reference_span, span_start, var, allele_idx);

                auto it = haplotype_to_index.find(haplotype);
                if (it != haplotype_to_index.end())
                    sample_haplotype_indices.insert(it->second);
            }
        }

        // If no variants for this sample, they have reference (index 0)
        if (sample_haplotype_indices.empty())
            sample_haplotype_indices.insert(0);

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
 * @return {num_groups, actual_write_cursor} where actual_write_cursor is the
 *         true position in the reference after this call.  It may exceed end_pos
 *         when a variant's REF span crosses the block boundary.
 */
// Returns {num_groups, new_write_pos, total_seds_entries, m_degen_entries}
// total_seds_entries = all EDS strings (including universal)
// m_degen_entries    = non-universal entries written to seds_out (for sparse formats)
// presence_bitvec / bitvec_bit_count: accumulated across calls for sparse; nullptr for dense
std::tuple<size_t, size_t, size_t, size_t> generate_eds_from_variants(
    std::istream& fasta_stream,
    const FASTAMetadata& fasta_meta,
    const std::vector<VCFVariant>& variants,
    size_t n_samples,
    std::ostream& eds_out,
    std::ostream& seds_out,
    size_t start_pos,
    size_t end_pos,
    Sources::Format seds_format = Sources::Format::SEDS,
    std::vector<uint8_t>* presence_bitvec = nullptr,
    size_t* bitvec_bit_count = nullptr)
{

    // Group overlapping variants
    std::vector<VariantGroup> groups = group_overlapping_variants(variants, fasta_stream, fasta_meta);
    size_t num_groups = groups.size();

    size_t current_pos = start_pos;

    const bool is_edz = (seds_format == Sources::Format::EDZ ||
                         seds_format == Sources::Format::EDZ_SPARSE);
    const bool is_sparse = (seds_format == Sources::Format::SEDS_SPARSE ||
                            seds_format == Sources::Format::EDZ_SPARSE);

    // Helper: record one bit in the presence bitvec (1 = non-universal)
    size_t m_degen_entries = 0;
    auto push_bit = [&](bool present) {
        if (!is_sparse || !presence_bitvec || !bitvec_bit_count) return;
        size_t bit_idx  = *bitvec_bit_count;
        size_t byte_idx = bit_idx / 8;
        if (presence_bitvec->size() <= byte_idx) presence_bitvec->resize(byte_idx + 1, 0);
        if (present) (*presence_bitvec)[byte_idx] |= uint8_t{1} << (bit_idx % 8);
        ++(*bitvec_bit_count);
    };

    const PathSet universal_ps{0};
    size_t seds_entries = 0;

    auto write_source = [&](const PathSet& ps) {
        bool is_univ = (ps.size() == 1 && ps[0] == 0);
        ++seds_entries;
        push_bit(!is_univ);
        if (is_sparse && is_univ) return;  // skip universal in sparse mode
        if (is_edz) {
            Sources::write_edz_entry(seds_out, ps, n_samples);
        } else {
            write_seds_entry(seds_out, std::set<int>(ps.begin(), ps.end()), n_samples);
        }
        ++m_degen_entries;
    };
    auto write_source_set = [&](const std::set<int>& s) {
        ++seds_entries;
        push_bit(true);  // explicit path sets are always non-universal
        if (is_edz) {
            Sources::write_edz_entry(seds_out, PathSet(s.begin(), s.end()), n_samples);
        } else {
            write_seds_entry(seds_out, s, n_samples);
        }
        ++m_degen_entries;
    };

    for (const auto& group : groups) {
        // Flush reference region before this variant group
        if (group.start_pos > current_pos) {
            std::string ref_region = read_fasta_region(fasta_stream, fasta_meta,
                                                        current_pos, group.start_pos - current_pos);
            if (!ref_region.empty()) {
                eds_out << '{' << ref_region << '}';
                write_source(universal_ps);
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
            for (size_t i = 0; i < group.merged_haplotypes.size(); i++)
                write_source(universal_ps);

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

        for (const auto& [haplotype, samples] : ordered_haplotypes)
            write_source_set(samples);

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
            write_source(universal_ps);
        }
        current_pos = flush_end;
    }

    // Return: num_groups, true write cursor, total EDS strings, non-universal written.
    // current_pos may exceed end_pos when a variant's REF spans a block boundary.
    return {num_groups, current_pos, seds_entries, m_degen_entries};
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
    size_t block_size,
    Sources::Format seds_format)
{
    // Step 1: Parse FASTA metadata
    FASTAMetadata fasta_meta = parse_fasta_metadata(fasta_stream);

    // Write placeholder header for binary formats (patched at end via seekp).
    if (seds_format == Sources::Format::EDZ) {
        Sources::write_edz_header(seds_output, /*num_paths_placeholder=*/0);
    } else if (seds_format == Sources::Format::EDZ_SPARSE) {
        Sources::write_edz_sparse_header(seds_output, /*num_paths_placeholder=*/0);
    }

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

    // Chromosome filter: warn once and skip all variants from chromosomes that do
    // not match the FASTA reference.  Handles "chr1" vs "1" naming differences.
    bool chrom_warned = false;
    auto accept_chrom = [&](const std::string& chrom) -> bool {
        if (chrom_matches(chrom, fasta_meta.seq_name)) return true;
        if (!chrom_warned) {
            std::cerr << "Warning: VCF contains variants on '" << chrom
                      << "' but FASTA reference is '" << fasta_meta.seq_name
                      << "'. Skipping non-matching variants (split VCF by chromosome first).\n";
            chrom_warned = true;
        }
        return false;
    };
    // actual_write_pos tracks where the EDS cursor truly is after each block.
    // It can exceed current_block_end when a variant's REF span crosses the
    // block boundary; the next generate_eds_from_variants call must start here
    // (not at current_block_start) to avoid re-emitting that reference region.
    size_t actual_write_pos = 0;
    size_t total_seds_entries = 0;
    size_t total_m_degen_entries = 0;

    // Sparse bitvec: accumulated across all blocks; bit i = 1 means string i is non-universal
    std::vector<uint8_t> presence_bitvec;
    size_t bitvec_bit_count = 0;
    const bool is_sparse_fmt = (seds_format == Sources::Format::SEDS_SPARSE ||
                                 seds_format == Sources::Format::EDZ_SPARSE);

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
                    if (!accept_chrom(var->chrom)) {
                        // Undo the processed_variants increment done above;
                        // chromosome mismatches are tracked separately.
                        if (stats) {
                            stats->processed_variants--;
                            stats->skipped_wrong_chrom++;
                        }
                        continue;
                    }

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

        // ── Overlap-extension ────────────────────────────────────────────────
        // A variant V1 in block N can have a REF that reaches past block_end.
        // If a carryover variant V2 starts inside V1's REF span it must be
        // grouped with V1, but the start-position carryover heuristic puts V2
        // in the next block, producing two separate degenerate symbols and
        // duplicating the overlapping reference region.
        //
        // Fix: after collecting block_variants, compute max_reach (the furthest
        // 0-indexed reference position covered by any variant in block_variants)
        // and pull any carryover variant whose start falls before that reach into
        // the current block.  As each such variant is absorbed its own end may
        // push max_reach further, so we keep reading from the VCF stream until
        // the next carryover candidate is truly outside the block's reach.
        if (!vcf_finished || !carryover_variants.empty()) {
            size_t max_reach = current_block_end;
            for (const auto& v : block_variants)
                max_reach = std::max(max_reach, (v.pos - 1) + v.ref.size());

            while (!carryover_variants.empty()) {
                size_t carry_start = carryover_variants.front().pos - 1;
                if (carry_start >= max_reach) break;  // truly outside reach

                // Absorb this carryover; its own REF may extend max_reach further.
                max_reach = std::max(max_reach,
                    carry_start + carryover_variants.front().ref.size());
                block_variants.push_back(std::move(carryover_variants.front()));
                carryover_variants.clear();

                // Refill carryover: read until we find a variant outside max_reach
                // (or EOF).  Any variant still inside max_reach is also absorbed.
                if (!vcf_finished) {
                    while (std::getline(vcf_stream, line)) {
                        SkipReason skip_reason;
                        auto var = parse_vcf_line(line, n_samples, skip_reason);
                        if (stats) {
                            if      (skip_reason == SkipReason::NONE)
                                { stats->total_variants++; stats->processed_variants++; }
                            else if (skip_reason == SkipReason::MALFORMED)
                                { stats->total_variants++; stats->skipped_malformed++; }
                            else if (skip_reason == SkipReason::UNSUPPORTED_SV)
                                { stats->total_variants++; stats->skipped_unsupported_sv++; }
                        }
                        if (var) {
                            if (!accept_chrom(var->chrom)) {
                                if (stats) {
                                    stats->processed_variants--;
                                    stats->skipped_wrong_chrom++;
                                }
                                continue;
                            }
                            size_t var_start = var->pos - 1;
                            if (var_start >= max_reach) {
                                carryover_variants.push_back(*var);
                                break;
                            }
                            max_reach = std::max(max_reach, var_start + var->ref.size());
                            block_variants.push_back(*var);
                        }
                    }
                    if (vcf_stream.eof() || vcf_stream.fail()) vcf_finished = true;
                }
            }
        }

        // Sort block variants by position (should mostly be sorted already from VCF)
        std::sort(block_variants.begin(), block_variants.end(),
                  [](const VCFVariant& a, const VCFVariant& b) {
                      return a.pos < b.pos;
                  });

        // Generate EDS for this block and count groups.
        // Pass actual_write_pos (not current_block_start) so that a variant whose
        // REF extended past the previous block boundary is not re-emitted here.
        auto [block_groups, new_write_pos, block_seds_entries, block_m_degen] =
            generate_eds_from_variants(
                fasta_stream, fasta_meta, block_variants, n_samples,
                eds_output, seds_output,
                actual_write_pos, current_block_end, seds_format,
                is_sparse_fmt ? &presence_bitvec  : nullptr,
                is_sparse_fmt ? &bitvec_bit_count : nullptr);
        actual_write_pos = new_write_pos;
        total_seds_entries    += block_seds_entries;
        total_m_degen_entries += block_m_degen;

        // Flush output to disk after each block (prevent memory accumulation)
        eds_output.flush();
        seds_output.flush();

        // Accumulate variant groups for statistics
        if (stats) {
            total_variant_groups += block_groups;
        }

        // Clear block variants to free memory
        block_variants.clear();

        // Move to next block (logical boundaries for VCF reading are unchanged)
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

    // Finalize header / trailer for binary or sparse formats.
    seds_output.flush();
    if (seds_format == Sources::Format::EDZ) {
        auto write_u64le = [&](std::streamoff off, uint64_t v) {
            uint8_t buf[8];
            for (int i = 0; i < 8; ++i) buf[i] = static_cast<uint8_t>(v >> (8 * i));
            seds_output.seekp(off);
            seds_output.write(reinterpret_cast<const char*>(buf), 8);
        };
        write_u64le(8,  static_cast<uint64_t>(total_seds_entries));  // cardinality
        write_u64le(16, static_cast<uint64_t>(n_samples));            // num_paths
        seds_output.seekp(0, std::ios::end);
    } else if (seds_format == Sources::Format::EDZ_SPARSE) {
        // Ensure bitvec is padded to cover all bits
        size_t bitvec_size = (total_seds_entries + 7) / 8;
        if (presence_bitvec.size() < bitvec_size) presence_bitvec.resize(bitvec_size, 0);
        Sources::write_edz_sparse_finalize(seds_output, total_seds_entries,
                                            n_samples, total_m_degen_entries,
                                            presence_bitvec);
    } else if (seds_format == Sources::Format::SEDS_SPARSE) {
        size_t bitvec_size = (total_seds_entries + 7) / 8;
        if (presence_bitvec.size() < bitvec_size) presence_bitvec.resize(bitvec_size, 0);
        Sources::write_seds_sparse_finalize(seds_output, total_seds_entries,
                                             total_m_degen_entries, presence_bitvec,
                                             /*num_paths=*/n_samples);
    } else if (seds_format == Sources::Format::SEDS) {
        // Dense text SEDS: append the "SEDN" trailer recording num_paths so
        // complement entries expand exactly on load (no max-path-ID inference).
        Sources::write_seds_dense_finalize(seds_output, total_seds_entries,
                                           /*num_paths=*/n_samples);
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
    size_t block_size,
    const std::filesystem::path* keep_eds_path,
    const std::filesystem::path* keep_seds_path)
{
    // Two-stage pipeline VCF→EDS→l-EDS routed through temp files.
    //
    // We deliberately do NOT use an in-memory pipe here. The second stage,
    // eds_to_leds_linear(), consumes its entire EDS input to EOF before it
    // touches the SEDS input. Feeding both streams through bounded pipes from a
    // single producer thread therefore deadlocks as soon as the intermediate
    // SEDS exceeds the pipe buffer: the producer blocks writing SEDS while the
    // consumer blocks waiting for more EDS the producer can no longer emit —
    // which happens precisely on the large population VCFs this path targets.
    //
    // Materialising the intermediate EDS/SEDS to temp files removes the ordering
    // dependency entirely. It also costs no extra disk versus the pipe version:
    // eds_to_leds_linear() copies its input streams into its own temp directory
    // up front regardless, so the data was always going to land on disk.

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path()
        / ("edsparser_vcf2leds_" + std::to_string(getpid()));
    std::filesystem::create_directories(temp_dir);

    // Remove the temp directory on every exit path (normal return or exception).
    struct TempGuard {
        std::filesystem::path dir;
        ~TempGuard() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } guard{temp_dir};

    // When the caller wants to keep the intermediate EDS/SEDS, write stage 1
    // straight to those paths (outside temp_dir, so the TempGuard leaves them in
    // place). Otherwise use throwaway temp files that the guard removes on exit.
    std::filesystem::path temp_eds  = keep_eds_path  ? *keep_eds_path  : (temp_dir / "stage1.eds");
    std::filesystem::path temp_seds = keep_seds_path ? *keep_seds_path : (temp_dir / "stage1.seds");

    // ── Stage 1: VCF → EDS/SEDS (written to temp files) ──────────────────────
    {
        std::ofstream eds_tmp(temp_eds);
        if (!eds_tmp) {
            throw std::runtime_error("Failed to create temp EDS file: " + temp_eds.string());
        }
        std::ofstream seds_tmp(temp_seds);
        if (!seds_tmp) {
            throw std::runtime_error("Failed to create temp SEDS file: " + temp_seds.string());
        }
        parse_vcf_to_eds_streaming(vcf_stream, fasta_stream, eds_tmp, seds_tmp,
                                   stats, block_size);
    }  // ofstreams flushed and closed here before stage 2 reopens them

    // ── Stage 2: EDS → l-EDS ─────────────────────────────────────────────────
    // Use the path-based overload so eds_to_leds_linear() syms the stage-1
    // files instead of copying them.  For a chr1 run with 2504 samples, the
    // intermediate SEDS can reach ~180 GB; the stream-based overload would copy
    // it again (requiring 2× disk) and silently truncate if /tmp is full, leading
    // to "Unmatched '{' in EDS stream" failures.
    eds_to_leds_linear(temp_eds, leds_output, context_length,
                       &temp_seds, &seds_output);
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
