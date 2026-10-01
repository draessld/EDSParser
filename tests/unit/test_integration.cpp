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
#include <cctype>
#include <vector>
#include <algorithm>
#include <unistd.h>

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

// Per-run scratch directory, made by mkdtemp in main() and removed at exit. It
// used to be the fixed /tmp/edsparser_integration_test, shared by every
// concurrent run on the machine and never cleaned up.
fs::path TEMP_DIR;

fs::path create_temp_dir() {
    return TEMP_DIR;
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

// Strip trailing whitespace/newlines (tools may or may not end with '\n').
std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

// Parse the integer on an edsparser-stats line such as
// "  Number of symbols (n):                   4". Returns -1 if the label is
// absent or not followed by an integer, so a missing line can never compare
// equal to an expected count.
long stat_value(const std::string& output, const std::string& label) {
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        auto at = line.find(label);
        if (at == std::string::npos) continue;
        auto colon = line.find(':', at + label.size());
        if (colon == std::string::npos) continue;
        try {
            size_t used = 0;
            std::string rest = line.substr(colon + 1);
            long v = std::stol(rest, &used);
            if (trimmed(rest.substr(used)).empty()) return v;
        } catch (const std::exception&) {}
        return -1;
    }
    return -1;
}

// Check n / m / N of an EDS file via edsparser-stats; "" on success, else why.
std::string check_stats(const fs::path& eds, long n, long m, long N) {
    std::string output;
    int code = run_cmd(TOOLS_DIR + "/edsparser-stats -i " + eds.string(), &output);
    if (code != 0) return "edsparser-stats exited " + std::to_string(code) + ": " + output;
    long got_n = stat_value(output, "Number of symbols (n)");
    long got_m = stat_value(output, "Total strings (m)");
    long got_N = stat_value(output, "Total characters (N)");
    if (got_n != n || got_m != m || got_N != N) {
        return "expected n=" + std::to_string(n) + " m=" + std::to_string(m) +
               " N=" + std::to_string(N) + ", got n=" + std::to_string(got_n) +
               " m=" + std::to_string(got_m) + " N=" + std::to_string(got_N);
    }
    return "";
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
    // {A,G}{C}{T,A}: 3 symbols, 5 strings, 5 characters.
    std::string why = check_stats(eds_file, 3, 5, 5);
    if (why.empty()) {
        pass();
    } else {
        fail(why);
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

    // n=4 symbols; m=1+3+1+3=8 strings; N=4+3+2+9=18 characters. This used to
    // check output.find("4"), which any '4' anywhere in the report satisfied.
    std::string why = check_stats(eds_file, 4, 8, 18);
    if (why.empty()) {
        pass();
    } else {
        fail(why);
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
        // The internal {T} is shorter than l=2, so {G,C}{T}{A,G} merges
        // (cartesian, no sources); the leading {A} is a boundary segment and is
        // exempt. Compact output.
        std::string content = trimmed(read_file(leds_file));
        const std::string expected = "A{GTA,GTG,CTA,CTG}";
        if (content == expected) {
            pass();
        } else {
            fail("expected " + expected + ", got " + content);
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
        // Cartesian of {A,G}x{C,T} = {AC,AT,GC,GT}: all four, nothing else. The
        // old check accepted any output containing "AC" *or* "GC".
        std::string content = trimmed(read_file(leds_file));
        const std::string expected = "{AC,AT,GC,GT}";
        if (content == expected) {
            pass();
        } else {
            fail("cartesian merge: expected " + expected + ", got " + content);
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
        // Columns 1-2 vary (CG / GG / C-), so they form one degenerate symbol
        // with one alternative per distinct sequence, gap removed.
        std::string content = trimmed(read_file(eds_file));
        const std::string expected = "{A}{CG,GG,C}{T}";
        if (content == expected) {
            pass();
        } else {
            fail("expected " + expected + ", got " + content);
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
        // SNP G>A at 1-based position 3 of ACGTACGTACGT, heterozygous.
        std::string content = trimmed(read_file(eds_file));
        const std::string expected = "{AC}{G,A}{TACGTACGT}";
        if (content == expected) {
            pass();
        } else {
            fail("expected " + expected + ", got " + content);
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
        // Exactly 5 patterns, each of length 4 and each a substring of one of
        // the EDS's two paths — not merely five non-empty lines.
        const std::vector<std::string> paths = {"ACGTACGTATTTTTTTT", "ACGTACGTGTTTTTTTT"};
        std::ifstream ifs(patterns_file);
        int lines = 0;
        std::string line, bad;
        while (std::getline(ifs, line)) {
            if (line.empty()) continue;
            lines++;
            bool occurs = std::any_of(paths.begin(), paths.end(),
                [&](const std::string& p) { return p.find(line) != std::string::npos; });
            if (line.size() != 4 || !occurs) bad += " '" + line + "'";
        }
        if (lines == 5 && bad.empty()) {
            pass();
        } else {
            fail("expected 5 length-4 patterns occurring in the EDS, got " +
                 std::to_string(lines) + (bad.empty() ? "" : "; invalid:" + bad));
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

    // Both variable sites are flanked by >= 3 common characters, so l=3 merges
    // nothing; the l-EDS is the EDS in compact form, and it must reload with
    // the same shape: 5 symbols, 1+2+1+2+1 = 7 strings, 1+2+5+1+4 = 13 chars.
    std::string content = trimmed(read_file(leds_file));
    const std::string expected = "A{C,G}GTACG{T,}ACGT";
    if (content != expected) {
        fail("l-EDS: expected " + expected + ", got " + content);
        return;
    }
    std::string why = check_stats(leds_file, 5, 7, 13);
    if (why.empty()) {
        pass();
    } else {
        fail("l-EDS reload: " + why);
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

    // S1 is 1/1 at position 7, so every path carries T there and it is
    // applied to the common sequence; only the 0/1 SNP at 3 stays degenerate.
    std::string content = trimmed(read_file(leds_file));
    const std::string expected = "AC{G,A}TACTTACGTACGT";
    if (content != expected) {
        fail("l-EDS: expected " + expected + ", got " + content);
        return;
    }
    std::string why = check_stats(leds_file, 3, 4, 17);
    if (why.empty()) {
        pass();
    } else {
        fail("l-EDS reload: " + why);
    }

    fs::remove(vcf_file);
    fs::remove(ref_file);
    if (fs::exists(eds_file)) fs::remove(eds_file);
    if (fs::exists(leds_file)) fs::remove(leds_file);
}

// Regression: vcf2eds -l (linear merge, with sources, default compact output)
// must produce an l-EDS whose cardinality matches its .seds, so that the pair
// loads back cleanly. The bug: adjacent common symbols left by the merge lose
// their brackets in compact output and re-parse as a single symbol, dropping the
// EDS cardinality below the SEDS cardinality (EDS::load then throws).
void test_vcf2leds_cardinality_consistency() {
    test("vcf2eds -l output loads back (EDS/SEDS cardinality match)");
    auto temp = create_temp_dir();
    auto vcf_file = temp / "card.vcf";
    auto ref_file = temp / "card_ref.fa";
    auto leds_file = temp / "card.leds";
    // vcf2eds derives the sources path from the VCF stem: <stem>_l<N>.seds
    auto seds_file = temp / "card_l2.seds";

    {
        std::ofstream ofs(ref_file);
        ofs << ">chr1\nACGTACGTACGTACGTACGTACGT\n";
    }
    // Two close variants leave a short common block between/around them, which
    // the linear merge collapses — the scenario that produced adjacent commons.
    {
        std::ofstream ofs(vcf_file);
        ofs << "##fileformat=VCFv4.2\n";
        ofs << "##contig=<ID=chr1,length=24>\n";
        ofs << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tS1\tS2\n";
        ofs << "chr1\t3\t.\tG\tA\t.\tPASS\t.\tGT\t0/1\t1/1\n";
        ofs << "chr1\t5\t.\tA\tT\t.\tPASS\t.\tGT\t1/1\t0/1\n";
        ofs << "chr1\t6\t.\tC\tG\t.\tPASS\t.\tGT\t0/1\t1/1\n";
    }

    std::string output;
    int code = run_cmd(TOOLS_DIR + "/vcf2eds -i " + vcf_file.string() +
                       " -r " + ref_file.string() +
                       " -l 2 -o " + leds_file.string(), &output);
    if (code != 0 || !file_exists_with_content(leds_file)) {
        fail("vcf2eds -l failed: " + output);
        return;
    }
    if (!file_exists_with_content(seds_file)) {
        fail("expected sources file not produced: " + seds_file.string());
        return;
    }

    // edsparser-stats with -s exercises EDS::load(eds, seds), which validates
    // that Sources::cardinality() == EDS::m_ and exits non-zero on mismatch.
    std::string stats_out;
    int stats_code = run_cmd(TOOLS_DIR + "/edsparser-stats -i " + leds_file.string() +
                             " -s " + seds_file.string(), &stats_out);
    if (stats_code != 0 ||
        stats_out.find("does not match") != std::string::npos) {
        fail("l-EDS/SEDS cardinality mismatch: " + stats_out);
        return;
    }

    pass();

    fs::remove(vcf_file);
    fs::remove(ref_file);
    if (fs::exists(leds_file)) fs::remove(leds_file);
    if (fs::exists(seds_file)) fs::remove(seds_file);
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

    std::string tmpl = (fs::temp_directory_path() / "edsparser_integration_XXXXXX").string();
    if (!mkdtemp(tmpl.data())) {
        std::cerr << "ERROR: mkdtemp failed for " << tmpl << "\n";
        return 1;
    }
    TEMP_DIR = tmpl;

    std::cout << "Tools directory: " << TOOLS_DIR << "\n";
    std::cout << "Scratch directory: " << TEMP_DIR << "\n\n";

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
    test_vcf2leds_cardinality_consistency();

    // Summary
    std::cout << "\n===========================================\n";
    std::cout << "Results: " << passed << "/" << test_num << " tests passed\n";
    if (failed > 0) {
        std::cout << "FAILED: " << failed << " tests\n";
    }
    std::cout << "===========================================\n";

    std::error_code ec;
    fs::remove_all(TEMP_DIR, ec);

    return failed > 0 ? 1 : 0;
}
