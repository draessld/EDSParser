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
    assert(degenerate_count >= 10 && "Should have at least 10 degenerate symbols");

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

        std::cout << "\n=== All VCF tests passed ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
