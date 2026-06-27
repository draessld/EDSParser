#include "transforms/vcf_transforms.hpp"
#include "formats/eds.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cassert>
#include <algorithm>
#include <filesystem>

using namespace edsparser;

// ---------------------------------------------------------------------------
// Inline test data (no external data/ directory dependency)
// ---------------------------------------------------------------------------

const std::string SMALL_VCF =
    "##fileformat=VCFv4.2\n"
    "##reference=reference.fasta\n"
    "#CHROM  POS     ID      REF     ALT     QUAL    FILTER  INFO    FORMAT  SAMPLE1 SAMPLE2 SAMPLE3 SAMPLE4 SAMPLE5\n"
    "chr1    5       .       T       C       99      PASS    .       GT      0|0     0|1     1|0     1|1     0|0\n"
    "chr1    8      .       A       G       99      PASS    .       GT      0|1     0|0     0|1     1|1     0|0\n"
    "chr1    20      .       C       T       99      PASS    .       GT      1|0     1|1     0|0     0|1     0|0\n"
    "chr1    27      .       G       A       99      PASS    .       GT      0|0     0|1     0|0     1|1     0|0\n"
    "chr1    34      .       T       TA      99      PASS    .       GT      0|1     1|0     1|1     0|0     0|0\n"
    "chr1    42      .       C       <DEL>       99      PASS    .       GT      1|1     0|1     0|0     0|0     1|0\n"
    "chr1    55      .       A       C       99      PASS    .       GT      0|0     0|1     1|1     0|0     0|1\n"
    "chr1    60      .       G       T       99      PASS    .       GT      0|1     1|0     0|0     1|1     0|0\n"
    "chr1    75      .       A       ATG     99      PASS    .       GT      1|0     1|1     0|0     0|1     0|0\n"
    "chr1    90      .       T       G,A       99      PASS    .       GT      0|2     0|0     2|1     1|0     1|1\n";

const std::string SMALL_FA =
    ">chr1\n"
    "AGCTTAGCTAAGCTTACGATCGATCGTACGATCGATCGTACGATCGTACG\n"
    "TAGCTAGCTAGCTGACTGATCGATCGTACGTAGCATCGATCGTAGCTAGC\n"
    "TGATCGTAGCTAGCTGATCGATGCTAGCTAGCTAG";

const std::string SAMEPOS_VCF =
    "##fileformat=VCFv4.2\n"
    "##reference=test_samepos.fa\n"
    "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tSAMPLE1\tSAMPLE2\n"
    "chr1\t5\t.\tA\tC\t99\tPASS\t.\tGT\t0|1\t0|0\n"
    "chr1\t5\t.\tA\tG\t99\tPASS\t.\tGT\t0|0\t1|1\n";

const std::string SAMEPOS_FA =
    ">chr1\n"
    "ACGTACGTACGT\n";

const std::string OVERLAPS_VCF =
    "##fileformat=VCFv4.2\n"
    "##reference=test_overlaps.fa\n"
    "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tSAMPLE1\tSAMPLE2\tSAMPLE3\n"
    "chr1\t2\t.\tGA\tAGTA\t99\tPASS\t.\tGT\t0|1\t1|1\t0|0\n"
    "chr1\t3\t.\tA\t<DEL>\t99\tPASS\t.\tGT\t0|0\t0|1\t1|1\n"
    "chr1\t10\t.\tT\tC\t99\tPASS\t.\tGT\t1|0\t0|0\t0|1\n";

const std::string OVERLAPS_FA =
    ">chr1\n"
    "ACGATGCTTAAGCTAGC\n";

const std::string CNV_INV_VCF =
    "##fileformat=VCFv4.2\n"
    "##reference=test_cnv_inv.fa\n"
    "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tSAMPLE1\tSAMPLE2\tSAMPLE3\n"
    "chr1\t10\t.\tACGT\t<CN0>\t99\tPASS\t.\tGT\t1|1\t0|1\t0|0\n"
    "chr1\t20\t.\tATG\t<CN2>\t99\tPASS\t.\tGT\t0|1\t1|0\t0|0\n"
    "chr1\t30\t.\tGCTA\t<CN3>\t99\tPASS\t.\tGT\t0|0\t1|1\t0|1\n"
    "chr1\t40\t.\tTGCA\t<INV>\t99\tPASS\t.\tGT\t1|0\t0|1\t1|1\n"
    "chr1\t50\t.\tAAGG\t<CN1>\t99\tPASS\t.\tGT\t0|0\t0|1\t1|0\n"
    "chr1\t60\t.\tT\t<CN0>,<INV>\t99\tPASS\t.\tGT\t0|1\t1|2\t2|0\n";

const std::string CNV_INV_FA =
    ">chr1\n"
    "NNNNNNNNACGTNNNNNATGNNNNNGCTANNNNNTGCANNNNAAGGNNNNNT\n";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

EDS load_eds_from_streams(std::stringstream& eds_ss, std::stringstream& seds_ss) {
    auto temp_dir = std::filesystem::temp_directory_path();
    auto eds_path = temp_dir / "test_eds_vcf.tmp";
    auto seds_path = temp_dir / "test_seds_vcf.tmp";
    { std::ofstream f(eds_path);  f << eds_ss.str(); }
    { std::ofstream f(seds_path); f << seds_ss.str(); }
    auto eds = EDS::load(eds_path, seds_path);
    std::filesystem::remove(eds_path);
    std::filesystem::remove(seds_path);
    return eds;
}

static size_t count_degenerate(const std::string& eds_str) {
    size_t count = 0;
    bool in_sym = false, has_comma = false;
    for (char c : eds_str) {
        if      (c == '{') { in_sym = true; has_comma = false; }
        else if (c == '}') { if (has_comma) count++; in_sym = false; }
        else if (c == ',' && in_sym) has_comma = true;
    }
    return count;
}

// Sum of the lengths of the first (reference) haplotype in every EDS symbol.
// With a well-formed EDS each reference position must appear exactly once, so
// this total must equal the length of the original reference sequence.
static size_t ref_path_length(const std::string& eds_str) {
    size_t total = 0;
    size_t i = 0;
    while (i < eds_str.size()) {
        if (eds_str[i] != '{') { i++; continue; }
        size_t j = i + 1;
        while (j < eds_str.size() && eds_str[j] != ',' && eds_str[j] != '}') j++;
        total += j - (i + 1);
        while (j < eds_str.size() && eds_str[j] != '}') j++;
        i = j + 1;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_basic_vcf_parsing() {
    std::cout << "Test 1: Basic VCF parsing..." << std::endl;

    std::stringstream vcf(SMALL_VCF), fa(SMALL_FA);
    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa);

    assert(!eds_str.empty()   && "EDS string should not be empty");
    assert(!seds_str.empty()  && "sEDS string should not be empty");
    assert(eds_str.front() == '{' && "EDS should start with {");
    assert(eds_str.back()  == '}' && "EDS should end with }");

    size_t degenerate_count = count_degenerate(eds_str);
    std::cout << "  EDS length: " << eds_str.size() << std::endl;
    std::cout << "  Degenerate symbols: " << degenerate_count << std::endl;
    // 9 degenerate symbols: variant at pos 20 has FASTA_ref==VCF_ALT so it
    // collapses to a single haplotype (non-degenerate). This is correct output.
    assert(degenerate_count >= 9 && "Should have at least 9 degenerate symbols");

    std::cout << "  PASS" << std::endl;
}

void test_multiallelic() {
    std::cout << "Test 4: Multi-allelic site handling..." << std::endl;

    std::stringstream vcf(SMALL_VCF), fa(SMALL_FA);
    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa);

    bool found = false;
    for (size_t pos = 0; pos < eds_str.size(); ) {
        if (eds_str[pos] == '{') {
            size_t end = eds_str.find('}', pos);
            std::string sym = eds_str.substr(pos + 1, end - pos - 1);
            if (std::count(sym.begin(), sym.end(), ',') >= 2) {
                found = true;
                std::cout << "  Found multi-allelic: {" << sym << "}" << std::endl;
                break;
            }
            pos = end + 1;
        } else { pos++; }
    }

    assert(found && "Should find at least one multi-allelic symbol");
    std::cout << "  PASS" << std::endl;
}

void test_deletion() {
    std::cout << "Test 5: Deletion handling (<DEL>)..." << std::endl;

    std::stringstream vcf(SMALL_VCF), fa(SMALL_FA);
    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa);

    for (size_t pos = 0; pos < eds_str.size(); ) {
        if (eds_str[pos] == '{') {
            size_t end = eds_str.find('}', pos);
            std::string sym = eds_str.substr(pos + 1, end - pos - 1);
            if (sym.find(",,") != std::string::npos || sym.back() == ',') {
                std::cout << "  Found deletion symbol: {" << sym << "}" << std::endl;
                break;
            }
            pos = end + 1;
        } else { pos++; }
    }

    std::cout << "  Deletion parsing completed without errors" << std::endl;
    std::cout << "  PASS" << std::endl;
}

void test_same_position_merging() {
    std::cout << "Test 6: Same-position variant merging..." << std::endl;

    std::stringstream vcf(SAMEPOS_VCF), fa(SAMEPOS_FA);
    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa);

    std::cout << "  EDS:  " << eds_str  << std::endl;
    std::cout << "  sEDS: " << seds_str << std::endl;

    bool found = false;
    for (size_t pos = 0; pos < eds_str.size(); ) {
        if (eds_str[pos] == '{') {
            size_t end = eds_str.find('}', pos);
            std::string sym = eds_str.substr(pos + 1, end - pos - 1);
            if (std::count(sym.begin(), sym.end(), ',') >= 2 &&
                sym.find('A') != std::string::npos &&
                sym.find('C') != std::string::npos &&
                sym.find('G') != std::string::npos) {
                found = true;
                std::cout << "  Found merged symbol: {" << sym << "}" << std::endl;
                break;
            }
            pos = end + 1;
        } else { pos++; }
    }

    assert(found && "Should find merged symbol with A,C,G");
    std::cout << "  PASS" << std::endl;
}

void test_overlapping_merging() {
    std::cout << "Test 7: Overlapping variant merging..." << std::endl;

    std::stringstream vcf(OVERLAPS_VCF), fa(OVERLAPS_FA);
    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa);

    std::cout << "  EDS:  " << eds_str  << std::endl;
    std::cout << "  sEDS: " << seds_str << std::endl;

    bool found_overlap = false;
    size_t degenerate_count = 0;
    for (size_t pos = 0; pos < eds_str.size(); ) {
        if (eds_str[pos] == '{') {
            size_t end = eds_str.find('}', pos);
            std::string sym = eds_str.substr(pos + 1, end - pos - 1);
            if (sym.find(',') != std::string::npos) {
                degenerate_count++;
                if (std::count(sym.begin(), sym.end(), ',') >= 2 && !found_overlap) {
                    found_overlap = true;
                    std::cout << "  Found overlapping merge: {" << sym << "}" << std::endl;
                }
            }
            pos = end + 1;
        } else { pos++; }
    }

    assert(found_overlap && "Should find merged symbol for overlapping variants");
    assert(degenerate_count >= 2 && "Should have at least 2 degenerate symbols");
    std::cout << "  Total degenerate symbols: " << degenerate_count << std::endl;
    std::cout << "  PASS" << std::endl;
}

void test_cnv_handling() {
    std::cout << "Test 8: Copy Number Variation (CNV) handling..." << std::endl;

    std::stringstream vcf(CNV_INV_VCF), fa(CNV_INV_FA);
    VCFStats stats;
    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa, &stats);

    std::cout << "  EDS: " << eds_str << std::endl;

    assert(stats.skipped_unsupported_sv == 0 && "CNV variants should be supported, not skipped");
    assert(stats.processed_variants >= 5     && "Should process at least 5 CNV variants");
    assert(count_degenerate(eds_str) >= 5    && "Should have degenerate symbols for CNV variants");

    std::cout << "  Degenerate symbols: " << count_degenerate(eds_str) << std::endl;
    std::cout << "  Processed variants: " << stats.processed_variants << std::endl;
    std::cout << "  PASS" << std::endl;
}

void test_inversion_handling() {
    std::cout << "Test 9: Inversion (INV) handling..." << std::endl;

    std::stringstream vcf(CNV_INV_VCF), fa(CNV_INV_FA);
    VCFStats stats;
    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa, &stats);

    assert(stats.skipped_unsupported_sv == 0 && "INV variants should be supported, not skipped");

    bool found = false;
    for (size_t pos = 0; pos < eds_str.size(); ) {
        if (eds_str[pos] == '{') {
            size_t end = eds_str.find('}', pos);
            std::string sym = eds_str.substr(pos + 1, end - pos - 1);
            if (sym.find("TGCA") != std::string::npos && sym.find(',') != std::string::npos) {
                found = true;
                std::cout << "  Found inversion symbol: {" << sym << "}" << std::endl;
                break;
            }
            pos = end + 1;
        } else { pos++; }
    }

    assert(found && "Should find inversion variant");
    std::cout << "  PASS" << std::endl;
}

void test_multiallelic_cnv_inv() {
    std::cout << "Test 10: Multi-allelic with CNV and INV..." << std::endl;

    std::stringstream vcf(CNV_INV_VCF), fa(CNV_INV_FA);
    VCFStats stats;
    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa, &stats);

    bool found = false;
    for (size_t pos = 0; pos < eds_str.size(); ) {
        if (eds_str[pos] == '{') {
            size_t end = eds_str.find('}', pos);
            std::string sym = eds_str.substr(pos + 1, end - pos - 1);
            if (std::count(sym.begin(), sym.end(), ',') >= 2 &&
                sym.find('T') != std::string::npos) {
                found = true;
                std::cout << "  Found multi-allelic CNV/INV: {" << sym << "}" << std::endl;
                break;
            }
            pos = end + 1;
        } else { pos++; }
    }

    assert(found && "Should find multi-allelic symbol with CNV and INV");
    std::cout << "  PASS" << std::endl;
}

void test_header_only_vcf() {
    std::cout << "Test 11: VCF with only headers..." << std::endl;

    std::stringstream vcf("##fileformat=VCFv4.2\n#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n");
    std::stringstream fa(">chr1\nACGTACGTACGT");

    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa);

    assert(eds_str  == "{ACGTACGTACGT}" && "EDS should match reference sequence");
    assert(seds_str == "{0}"            && "sEDS should be universal path");

    std::cout << "  PASS" << std::endl;
}

void test_variant_across_block_boundary() {
    std::cout << "Test 12: Variant spanning a block boundary..." << std::endl;

    std::stringstream vcf(
        "##fileformat=VCFv4.2\n"
        "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n"
        "chr1\t8\t.\tCGTAC\tA\t.\tPASS\t.\n");
    std::stringstream fa(">chr1\nACGTACGTACGTACGTACGT");

    VCFStats stats;
    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa, &stats, 10);

    std::cout << "  EDS: " << eds_str << std::endl;

    assert(eds_str.find("{CGTAC,A}") != std::string::npos && "Variant spanning block boundary not processed correctly");
    assert(stats.processed_variants == 1 && "Should have processed exactly one variant");

    std::cout << "  PASS" << std::endl;
}

void test_block_boundary_no_reference_duplication() {
    std::cout << "Test 13: Block-boundary variant must not duplicate reference..." << std::endl;

    // Reference: 20 chars.  A variant at VCF POS=3 (0-indexed [2,8)) with
    // REF="GTACGT" (6 chars) crosses the block boundary at position 5.
    // Bug: block 2 would re-emit ref[5:10) duplicating positions 5-7 that are
    //      already the REF haplotype of the degenerate symbol from block 1.
    // Fix: block 2 must start from position 8 (the true end of the variant).

    const std::string ref_seq = "ACGTACGTACGTACGTACGT";  // 20 chars
    std::stringstream vcf(
        "##fileformat=VCFv4.2\n"
        "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n"
        "chr1\t3\t.\tGTACGT\tT\t.\tPASS\t.\n");
    std::stringstream fa(">chr1\n" + ref_seq + "\n");

    // block_size=5 forces the boundary at pos 5, squarely inside REF=[2,8)
    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa, nullptr, 5);

    std::cout << "  EDS: " << eds_str << std::endl;

    // The degenerate symbol must be present.
    if (eds_str.find("{GTACGT,T}") == std::string::npos)
        throw std::runtime_error("Degenerate symbol {GTACGT,T} spanning block boundary not found in EDS");

    // The reference path (first haplotype in every symbol) must cover exactly
    // the 20 positions of the reference sequence -- no more, no less.
    size_t rpl = ref_path_length(eds_str);
    std::cout << "  Reference path length: " << rpl
              << " (expected " << ref_seq.size() << ")" << std::endl;
    if (rpl != ref_seq.size())
        throw std::runtime_error(
            "Reference path length " + std::to_string(rpl) +
            " != " + std::to_string(ref_seq.size()) +
            " -- block-boundary variant duplicated reference region");

    std::cout << "  PASS" << std::endl;
}

void test_block_boundary_overlapping_grouping() {
    std::cout << "Test 14: Overlapping variants across block boundary must be merged..." << std::endl;

    // Reference: ACGTACGTACGTACGTACGT (20 chars, 0-indexed)
    //   pos 2-7: G T A C G T
    //   pos 5-6: C G
    //
    // V1: POS=3 (1-idx, 0-idx=2), REF=GTACGT (6 chars), spans [2,8), ALT=T
    // V2: POS=6 (1-idx, 0-idx=5), REF=CG     (2 chars), spans [5,7), ALT=AA
    //   → V2 starts inside V1's REF span [2,8) and should be merged with V1.
    //
    // block_size=5: boundary at 5. V1 (start=2 < 5) → block N; V2 (start=5 ≥ 5) → carryover.
    //
    // Bug: V1 and V2 are processed in separate blocks and produce 2 degenerate symbols,
    //      duplicating positions 5-6 in the reference path.
    // Fix: after reading block N's variants, the overlap-extension loop detects that
    //      carryover V2 starts inside V1's reach (5 < 8) and pulls it into block N.

    const std::string ref_seq = "ACGTACGTACGTACGTACGT";
    std::stringstream vcf(
        "##fileformat=VCFv4.2\n"
        "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n"
        "chr1\t3\t.\tGTACGT\tT\t.\tPASS\t.\n"
        "chr1\t6\t.\tCG\tAA\t.\tPASS\t.\n");
    std::stringstream fa(">chr1\n" + ref_seq + "\n");

    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa, nullptr, 5);

    std::cout << "  EDS: " << eds_str << std::endl;

    size_t rpl = ref_path_length(eds_str);
    std::cout << "  Reference path length: " << rpl << " (expected 20)" << std::endl;
    if (rpl != ref_seq.size())
        throw std::runtime_error(
            "Reference path length " + std::to_string(rpl) +
            " != 20 -- overlapping variants across block boundary were not merged");

    size_t degen = count_degenerate(eds_str);
    std::cout << "  Degenerate symbols: " << degen << " (expected 1)" << std::endl;
    if (degen != 1)
        throw std::runtime_error(
            "Expected 1 merged degenerate symbol but got " + std::to_string(degen) +
            " -- V1 and V2 were not grouped despite overlapping across block boundary");

    std::cout << "  PASS" << std::endl;
}

void test_multi_chromosome_filtering() {
    std::cout << "Test 15: Multi-chromosome VCF — wrong-chrom variants are skipped..." << std::endl;

    // FASTA is chr1 (10 chars). VCF has one chr1 variant and one chr2 variant.
    // The chr2 variant must be skipped, not applied to chr1 positions.
    std::stringstream vcf(
        "##fileformat=VCFv4.2\n"
        "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n"
        "chr1\t3\t.\tG\tA\t.\tPASS\t.\n"
        "chr2\t3\t.\tG\tT\t.\tPASS\t.\n");
    std::stringstream fa(">chr1\nACGTACGTAC\n");

    VCFStats stats;
    auto [eds_str, seds_str] = parse_vcf_to_eds_streaming_str(vcf, fa, &stats);

    std::cout << "  EDS: " << eds_str << std::endl;
    std::cout << "  skipped_wrong_chrom: " << stats.skipped_wrong_chrom << std::endl;

    if (stats.skipped_wrong_chrom != 1)
        throw std::runtime_error("Expected 1 skipped_wrong_chrom, got " +
                                 std::to_string(stats.skipped_wrong_chrom));

    // Only the chr1 variant (G→A at pos 3) should appear in the EDS
    if (eds_str.find("{G,A}") == std::string::npos)
        throw std::runtime_error("chr1 variant {G,A} not found in EDS");

    // The chr2 variant (G→T) must NOT appear — its ALT 'T' should not be present
    // at any position as a degenerate alternative to 'G' here
    if (count_degenerate(eds_str) != 1)
        throw std::runtime_error("Expected exactly 1 degenerate symbol, got " +
                                 std::to_string(count_degenerate(eds_str)));

    std::cout << "  PASS" << std::endl;
}

void test_no_genotype_seds_cardinality() {
    std::cout << "Test 16: No-genotype VCF — EDS/SEDS cardinality must match..." << std::endl;

    // VCF without FORMAT/SAMPLE columns: haplotype_to_samples is always empty.
    // Bug: emitting N alternatives in EDS but only 1 {0} in SEDS → cardinality
    // mismatch when EDS::load validates the pair.
    std::stringstream vcf(
        "##fileformat=VCFv4.2\n"
        "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n"
        "chr1\t3\t.\tG\tA\t.\tPASS\t.\n"
        "chr1\t7\t.\tC\tT\t.\tPASS\t.\n");
    std::stringstream fa(">chr1\nACGTACGTAC\n");

    std::ostringstream eds_ss, seds_ss;
    parse_vcf_to_eds_streaming(vcf, fa, eds_ss, seds_ss);

    const std::string eds_str  = eds_ss.str();
    const std::string seds_str = seds_ss.str();
    std::cout << "  EDS:  " << eds_str  << std::endl;
    std::cout << "  SEDS: " << seds_str << std::endl;

    // Count EDS cardinality (total strings = sum of alternatives per symbol)
    size_t eds_card = 0;
    for (size_t i = 0; i < eds_str.size(); ) {
        if (eds_str[i] != '{') { i++; continue; }
        size_t j = i + 1;
        eds_card++;  // at least one string
        while (j < eds_str.size() && eds_str[j] != '}') {
            if (eds_str[j] == ',') eds_card++;
            j++;
        }
        i = j + 1;
    }

    // Count SEDS cardinality (number of {…} sets)
    size_t seds_card = 0;
    for (size_t i = 0; i < seds_str.size(); ) {
        if (seds_str[i] != '{') { i++; continue; }
        seds_card++;
        while (i < seds_str.size() && seds_str[i] != '}') i++;
        i++;
    }

    std::cout << "  EDS cardinality:  " << eds_card  << std::endl;
    std::cout << "  SEDS cardinality: " << seds_card << std::endl;

    if (eds_card != seds_card)
        throw std::runtime_error(
            "EDS/SEDS cardinality mismatch: EDS=" + std::to_string(eds_card) +
            " SEDS=" + std::to_string(seds_card));

    // Also verify the pair loads back cleanly via EDS::load
    auto temp_dir = std::filesystem::temp_directory_path();
    auto eds_path  = temp_dir / "test_nogenotype.eds";
    auto seds_path = temp_dir / "test_nogenotype.seds";
    { std::ofstream f(eds_path);  f << eds_str; }
    { std::ofstream f(seds_path); f << seds_str; }
    EDS loaded = EDS::load(eds_path, seds_path);  // throws if cardinality mismatches
    std::filesystem::remove(eds_path);
    std::filesystem::remove(seds_path);

    std::cout << "  EDS::load succeeded (cardinality consistent)" << std::endl;
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== VCF Transform Tests ===" << std::endl;

    try {
        test_basic_vcf_parsing();
        test_multiallelic();
        test_deletion();
        test_same_position_merging();
        test_overlapping_merging();
        test_cnv_handling();
        test_inversion_handling();
        test_multiallelic_cnv_inv();
        test_header_only_vcf();
        test_variant_across_block_boundary();
        test_block_boundary_no_reference_duplication();
        test_block_boundary_overlapping_grouping();
        test_multi_chromosome_filtering();
        test_no_genotype_seds_cardinality();

        std::cout << "\n=== All VCF tests passed ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
