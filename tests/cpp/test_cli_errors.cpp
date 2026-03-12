/**
 * Integration tests for EDSParser CLI tool error handling.
 * Verifies that tools fail gracefully with non-zero exit codes and
 * informative error messages on invalid input.
 */

#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Test counter
int test_num = 0;
int passed = 0;
int failed = 0;

// Tool paths
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

// Execute command and return exit code and output
int run_cmd(const std::string& cmd, std::string& output) {
    std::string full_cmd = cmd + " 2>&1";
    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) return -1;

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        result += buffer;
    }

    int status = pclose(pipe);
    output = result;
    return WEXITSTATUS(status);
}

void test_vcf2eds_missing_reference() {
    test("vcf2eds fails without a reference file");
    std::string output;
    int code = run_cmd(TOOLS_DIR + "/vcf2eds -i non_existent.vcf", output);
    if (code != 0 && output.find("reference") != std::string::npos) {
        pass();
    } else {
        fail("Expected non-zero exit code and error message. Got code=" + std::to_string(code));
    }
}

void test_eds2leds_invalid_context() {
    test("eds2leds fails with context length < 1");
    std::string output;
    int code = run_cmd(TOOLS_DIR + "/eds2leds -i non_existent.eds -l 0", output);
    if (code != 0 && output.find("context_length") != std::string::npos) {
        pass();
    } else {
        fail("Expected non-zero exit code for l=0. Got code=" + std::to_string(code));
    }
}

void test_file_not_found() {
    test("Tools fail gracefully when input file not found");
    std::string output;
    int code = run_cmd(TOOLS_DIR + "/edsparser-stats -i non_existent_file.eds", output);
    if (code != 0 && (output.find("Failed to open") != std::string::npos || output.find("does not exist") != std::string::npos)) {
        pass();
    } else {
        fail("Expected file not found error. Got: " + output);
    }
}

int main(int argc, char* argv[]) {
    // Add check for argc to prevent segfault on argv[1]
    if (argc < 2 || std::string(argv[1]).empty()) {
        std::cerr << "Usage: " << argv[0] << " <path_to_tools_dir>\n";
        return 1;
    }
    TOOLS_DIR = argv[1];

    std::cout << "===========================================\n";
    std::cout << "EDSParser CLI Error Handling Tests\n";
    std::cout << "===========================================\n\n";

    test_vcf2eds_missing_reference();
    test_eds2leds_invalid_context();
    test_file_not_found();

    std::cout << "\nResults: " << passed << "/" << test_num << " tests passed.\n";
    return failed > 0 ? 1 : 0;
}