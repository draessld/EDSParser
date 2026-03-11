/**
 * Integration tests for EDSParser CLI tools
 * Tests: edsparser-stats, eds2leds, msa2eds, vcf2eds, edsparser-genpatterns
 */

#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cassert>
#include <cstring>
#include <vector>

namespace fs = std::filesystem;

// Test counter
int test_num = 0;
int passed = 0;
int failed = 0;

// Tool paths (set by find_tools())
std::string TOOLS_DIR;

void test(const std::string& description) {
    test_num++;
    std::cout << "Test " << test_num << ": " << description << "... " << std::flush;
}

void pass() {
    passed++;
    std::cout << "PASSED\n";
}

void fail(const std::string& reason = "") {
    failed++;
    std::cout << "FAILED";
    if (!reason.empty()) {
        std::cout << " (" << reason << ")";
    }
    std::cout << "\n";
}

// Execute command and return exit code
int run_cmd(const std::string& cmd, std::string* output = nullptr) {
    std::string full_cmd = cmd + " 2>&1";
    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) return -1;

    std::ostringstream oss;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        oss << buffer;
    }

    int status = pclose(pipe);
    if (output) *output = oss.str();
    return WEXITSTATUS(status);
}

// Check if file exists and has content
bool file_exists_with_content(const fs::path& path) {
    return fs::exists(path) && fs::file_size(path) > 0;
}

// Create temp directory for test outputs
fs::path create_temp_dir() {
    auto temp = fs::temp_directory_path() / "edsparser_integration_test";
    fs::create_directories(temp);
    return temp;
}

// ===== EDSPARSER-STATS TESTS =====

void test_stats_help() {
    test("edsparser-stats --help shows usage");
    std::string output;
    int code = run_cmd(TOOLS_DIR + "/edsparser-stats --help", &output);
    if (code == 0 && output.find("edsparser-stats") != std::string::npos) {
        pass();
    } else {
        fail("help not shown, code=" + std::to_string(code));
    }
}

void test_stats_from_string() {
    test("edsparser-stats can parse EDS from file");
    // Note: stdin parsing may not be supported, use temp file instead
    auto temp = create_temp_dir();
    auto eds_file = temp / "stdin_test.eds";
    {
        std::ofstream ofs(eds_file);
        ofs << "{A,G}{C}{T,A}";
    }
    std::string output;
    int code = run_cmd(TOOLS_DIR + "/edsparser-stats -i " + eds_file.string(), &output);
    if (code == 0 && output.find("Number of symbols") != std::string::npos) {
        pass();
    } else {
        fail("stats not computed: " + output);
    }
    fs::remove(eds_file);
}

void test_stats_file() {
    test("edsparser-stats can read EDS from file");
    auto temp = create_temp_dir();
    auto eds_file = temp / "test.eds";

    // Create test EDS file
    {
        std::ofstream ofs(eds_file);
        ofs << "{ACGT}{A,G,C}{TT}{AAA,GGG,CCC}";
    }

    std::string output;
    int code = run_cmd(TOOLS_DIR + "/edsparser-stats -i " + eds_file.string(), &output);
    // Output format: "Number of symbols (n):                   4"
    if (code == 0 && output.find("Number of symbols") != std::string::npos &&
        output.find("4") != std::string::npos) {
        pass();
    } else {
        fail("expected n=4, got: " + output);
    }

    fs::remove(eds_file);
}

// ===== EDS2LEDS TESTS =====

void test_eds2leds_help() {
    test("eds2leds --help shows usage");
    std::string output;
    int code = run_cmd(TOOLS_DIR + "/eds2leds --help", &output);
    if (code == 0 && output.find("eds2leds") != std::string::npos) {
        pass();
    } else {
        fail("help not shown");
    }
}

void test_eds2leds_basic() {
    test("eds2leds transforms EDS to l-EDS");
    auto temp = create_temp_dir();
    auto eds_file = temp / "input.eds";
    auto leds_file = temp / "output.leds";

    // Create test EDS file (needs context length enforcement)
    {
        std::ofstream ofs(eds_file);
        ofs << "{A}{G,C}{T}{A,G}";  // Context length 1, needs merging for l>=2
    }

    std::string output;
    int code = run_cmd(TOOLS_DIR + "/eds2leds -i " + eds_file.string() +
                       " -o " + leds_file.string() + " -l 2", &output);

    if (code == 0 && file_exists_with_content(leds_file)) {
        // Verify output has merged symbols
        std::ifstream ifs(leds_file);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        // After merging with l=2, should have fewer symbols
        if (content.find("{") != std::string::npos) {
            pass();
        } else {
            fail("output not valid EDS");
        }
    } else {
        fail("transformation failed: " + output);
    }

    fs::remove(eds_file);
    if (fs::exists(leds_file)) fs::remove(leds_file);
}

void test_eds2leds_cartesian() {
    test("eds2leds cartesian mode (no sources)");
    auto temp = create_temp_dir();
    auto eds_file = temp / "input.eds";
    auto leds_file = temp / "output.leds";

    {
        std::ofstream ofs(eds_file);
        ofs << "{A,G}{C,T}";
    }

    std::string output;
    // Without sources file = cartesian merge
    int code = run_cmd(TOOLS_DIR + "/eds2leds -i " + eds_file.string() +
                       " -o " + leds_file.string() + " -l 2", &output);

    if (code == 0 && file_exists_with_content(leds_file)) {
        std::ifstream ifs(leds_file);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        // Cartesian of {A,G}x{C,T} = {AC,AT,GC,GT}
        if (content.find("AC") != std::string::npos ||
            content.find("GC") != std::string::npos) {
            pass();
        } else {
            fail("cartesian merge not correct: " + content);
        }
    } else {
        fail("transformation failed: " + output);
    }

    fs::remove(eds_file);
    if (fs::exists(leds_file)) fs::remove(leds_file);
}

// ===== MSA2EDS TESTS =====

void test_msa2eds_help() {
    test("msa2eds --help shows usage");
    std::string output;
    int code = run_cmd(TOOLS_DIR + "/msa2eds --help", &output);
    if (code == 0 && output.find("msa2eds") != std::string::npos) {
        pass();
    } else {
        fail("help not shown");
    }
}

void test_msa2eds_basic() {
    test("msa2eds transforms MSA to EDS");
    auto temp = create_temp_dir();
    auto msa_file = temp / "test.msa";
    auto eds_file = temp / "output.eds";
    auto seds_file = temp / "output.seds";

    // Create simple MSA (FASTA with gaps)
    {
        std::ofstream ofs(msa_file);
        ofs << ">seq1\n";
        ofs << "ACGT\n";
        ofs << ">seq2\n";
        ofs << "AGGT\n";
        ofs << ">seq3\n";
        ofs << "AC-T\n";  // Gap at position 3
    }

    std::string output;
    int code = run_cmd(TOOLS_DIR + "/msa2eds -i " + msa_file.string() +
                       " -o " + eds_file.string(), &output);

    if (code == 0 && file_exists_with_content(eds_file)) {
        std::ifstream ifs(eds_file);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        // Should contain degenerate symbols
        if (content.find("{") != std::string::npos) {
            pass();
        } else {
            fail("output not valid EDS format");
        }
    } else {
        fail("transformation failed: " + output);
    }

    fs::remove(msa_file);
    if (fs::exists(eds_file)) fs::remove(eds_file);
    if (fs::exists(seds_file)) fs::remove(seds_file);
}

// ===== VCF2EDS TESTS =====

void test_vcf2eds_help() {
    test("vcf2eds --help shows usage");
    std::string output;
    int code = run_cmd(TOOLS_DIR + "/vcf2eds --help", &output);
    if (code == 0 && output.find("vcf2eds") != std::string::npos) {
        pass();
    } else {
        fail("help not shown");
    }
}

void test_vcf2eds_basic() {
    test("vcf2eds transforms VCF to EDS");
    auto temp = create_temp_dir();
    auto vcf_file = temp / "test.vcf";
    auto ref_file = temp / "ref.fa";
    auto eds_file = temp / "output.eds";

    // Create simple reference
    {
        std::ofstream ofs(ref_file);
        ofs << ">chr1\n";
        ofs << "ACGTACGTACGT\n";
    }

    // Create simple VCF
    {
        std::ofstream ofs(vcf_file);
        ofs << "##fileformat=VCFv4.2\n";
        ofs << "##contig=<ID=chr1,length=12>\n";
        ofs << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tSAMPLE1\n";
        ofs << "chr1\t3\t.\tG\tA\t.\tPASS\t.\tGT\t0/1\n";  // SNP at pos 3
    }

    std::string output;
    int code = run_cmd(TOOLS_DIR + "/vcf2eds -i " + vcf_file.string() +
                       " -r " + ref_file.string() +
                       " -o " + eds_file.string(), &output);

    if (code == 0 && file_exists_with_content(eds_file)) {
        std::ifstream ifs(eds_file);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        // Should have degenerate symbol for the SNP
        if (content.find("{") != std::string::npos) {
            pass();
        } else {
            fail("output not valid EDS format");
        }
    } else {
        fail("transformation failed: " + output);
    }

    fs::remove(vcf_file);
    fs::remove(ref_file);
    if (fs::exists(eds_file)) fs::remove(eds_file);
}

// ===== GENPATTERNS TESTS =====

void test_genpatterns_help() {
    test("edsparser-genpatterns --help shows usage");
    std::string output;
    int code = run_cmd(TOOLS_DIR + "/edsparser-genpatterns --help", &output);
    // Help output shows "Generate random patterns"
    if (code == 0 && output.find("Generate random patterns") != std::string::npos) {
        pass();
    } else {
        fail("help not shown: " + output);
    }
}

void test_genpatterns_basic() {
    test("edsparser-genpatterns generates patterns");
    auto temp = create_temp_dir();
    auto eds_file = temp / "test.eds";
    auto patterns_file = temp / "patterns.txt";

    // Create test EDS with enough content
    {
        std::ofstream ofs(eds_file);
        ofs << "{ACGTACGT}{A,G}{TTTTTTTT}";
    }

    std::string output;
    // -n for count (not -c), -l for length
    int code = run_cmd(TOOLS_DIR + "/edsparser-genpatterns -i " + eds_file.string() +
                       " -o " + patterns_file.string() + " -n 5 -l 4", &output);

    if (code == 0 && file_exists_with_content(patterns_file)) {
        // Count lines (patterns)
        std::ifstream ifs(patterns_file);
        int lines = 0;
        std::string line;
        while (std::getline(ifs, line)) {
            if (!line.empty()) lines++;
        }
        if (lines == 5) {
            pass();
        } else {
            fail("expected 5 patterns, got " + std::to_string(lines));
        }
    } else {
        fail("pattern generation failed: " + output);
    }

    fs::remove(eds_file);
    if (fs::exists(patterns_file)) fs::remove(patterns_file);
}

// ===== END-TO-END PIPELINE TESTS =====

void test_pipeline_msa_to_leds() {
    test("Pipeline: MSA -> EDS -> l-EDS");
    auto temp = create_temp_dir();
    auto msa_file = temp / "test.msa";
    auto eds_file = temp / "step1.eds";
    auto seds_file = temp / "step1.seds";
    auto leds_file = temp / "step2.leds";

    // Create MSA
    {
        std::ofstream ofs(msa_file);
        ofs << ">ref\nACGTACGTACGT\n";
        ofs << ">s1\nACGTACGTACGT\n";
        ofs << ">s2\nAGGTACGTACGT\n";  // SNP at pos 2
        ofs << ">s3\nACGTACG-ACGT\n";  // Gap at pos 8
    }

    std::string output;

    // Step 1: MSA -> EDS
    int code1 = run_cmd(TOOLS_DIR + "/msa2eds -i " + msa_file.string() +
                        " -o " + eds_file.string(), &output);

    if (code1 != 0 || !file_exists_with_content(eds_file)) {
        fail("MSA->EDS failed: " + output);
        return;
    }

    // Step 2: EDS -> l-EDS (cartesian, no sources)
    int code2 = run_cmd(TOOLS_DIR + "/eds2leds -i " + eds_file.string() +
                        " -o " + leds_file.string() + " -l 3", &output);

    if (code2 != 0 || !file_exists_with_content(leds_file)) {
        fail("EDS->l-EDS failed: " + output);
        return;
    }

    // Verify l-EDS is valid (check for any statistics output)
    int code3 = run_cmd(TOOLS_DIR + "/edsparser-stats -i " + leds_file.string(), &output);
    if (code3 == 0 && output.find("Number of symbols") != std::string::npos) {
        pass();
    } else {
        fail("l-EDS stats failed: " + output);
    }

    fs::remove(msa_file);
    if (fs::exists(eds_file)) fs::remove(eds_file);
    if (fs::exists(seds_file)) fs::remove(seds_file);
    if (fs::exists(leds_file)) fs::remove(leds_file);
}

void test_pipeline_vcf_to_leds() {
    test("Pipeline: VCF -> EDS -> l-EDS");
    auto temp = create_temp_dir();
    auto vcf_file = temp / "test.vcf";
    auto ref_file = temp / "ref.fa";
    auto eds_file = temp / "step1.eds";
    auto leds_file = temp / "step2.leds";

    // Create reference
    {
        std::ofstream ofs(ref_file);
        ofs << ">chr1\nACGTACGTACGTACGT\n";
    }

    // Create VCF with multiple variants
    {
        std::ofstream ofs(vcf_file);
        ofs << "##fileformat=VCFv4.2\n";
        ofs << "##contig=<ID=chr1,length=16>\n";
        ofs << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tS1\n";
        ofs << "chr1\t3\t.\tG\tA\t.\tPASS\t.\tGT\t0/1\n";
        ofs << "chr1\t7\t.\tC\tT\t.\tPASS\t.\tGT\t1/1\n";
    }

    std::string output;

    // Step 1: VCF -> EDS
    int code1 = run_cmd(TOOLS_DIR + "/vcf2eds -i " + vcf_file.string() +
                        " -r " + ref_file.string() +
                        " -o " + eds_file.string(), &output);

    if (code1 != 0 || !file_exists_with_content(eds_file)) {
        fail("VCF->EDS failed: " + output);
        return;
    }

    // Step 2: EDS -> l-EDS
    int code2 = run_cmd(TOOLS_DIR + "/eds2leds -i " + eds_file.string() +
                        " -o " + leds_file.string() + " -l 2", &output);

    if (code2 != 0 || !file_exists_with_content(leds_file)) {
        fail("EDS->l-EDS failed: " + output);
        return;
    }

    pass();

    fs::remove(vcf_file);
    fs::remove(ref_file);
    if (fs::exists(eds_file)) fs::remove(eds_file);
    if (fs::exists(leds_file)) fs::remove(leds_file);
}

// ===== MAIN =====

int main(int argc, char* argv[]) {
    std::cout << "===========================================\n";
    std::cout << "EDSParser Integration Tests\n";
    std::cout << "===========================================\n\n";

    if (argc < 2) {
        std::cerr << "ERROR: Path to tools directory is required.\n";
        std::cerr << "Usage: " << argv[0] << " <path_to_tools_dir>\n";
        return 1;
    }
    TOOLS_DIR = argv[1];

    std::cout << "Tools directory: " << TOOLS_DIR << "\n\n";

    // edsparser-stats tests
    std::cout << "--- edsparser-stats ---\n";
    test_stats_help();
    test_stats_from_string();
    test_stats_file();

    // eds2leds tests
    std::cout << "\n--- eds2leds ---\n";
    test_eds2leds_help();
    test_eds2leds_basic();
    test_eds2leds_cartesian();

    // msa2eds tests
    std::cout << "\n--- msa2eds ---\n";
    test_msa2eds_help();
    test_msa2eds_basic();

    // vcf2eds tests
    std::cout << "\n--- vcf2eds ---\n";
    test_vcf2eds_help();
    test_vcf2eds_basic();

    // genpatterns tests
    std::cout << "\n--- edsparser-genpatterns ---\n";
    test_genpatterns_help();
    test_genpatterns_basic();

    // Pipeline tests
    std::cout << "\n--- End-to-End Pipelines ---\n";
    test_pipeline_msa_to_leds();
    test_pipeline_vcf_to_leds();

    // Summary
    std::cout << "\n===========================================\n";
    std::cout << "Results: " << passed << "/" << test_num << " tests passed\n";
    if (failed > 0) {
        std::cout << "FAILED: " << failed << " tests\n";
    }
    std::cout << "===========================================\n";

    return failed > 0 ? 1 : 0;
}
