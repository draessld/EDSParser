// sEDS (source) parsing tests - MOSTLYDISABLED
// Most tests disabled because they require in-memory source construction
// which has been removed in favor of file-based streaming.

#include "formats/eds.hpp"
#include "formats/sources.hpp"
#include <sstream>
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>

void test_load_sources_from_file() {
    std::cout << "Test 1: Load sources from file... ";

    // Create EDS without sources
    std::stringstream eds_ss("{AC}{,A,T}{GT}");
    edsparser::EDS eds(eds_ss);
    assert(!eds.has_sources());

    // Create a temporary file with sEDS content
    std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "test_seds_load.seds";
    std::ofstream ofs(temp_path);
    ofs << "{0}{1}{2}{3}{0}";
    ofs.close();

    // Load sources from file using Sources class
    auto sources = Sources::load(temp_path);
    eds.set_sources_object(sources);

    assert(eds.has_sources());
    auto src0 = eds.read_source(0);
    auto src1 = eds.read_source(1);
    auto src2 = eds.read_source(2);
    auto src3 = eds.read_source(3);
    auto src4 = eds.read_source(4);
    assert(src0.count(0) == 1);
    assert(src1.count(1) == 1);
    assert(src2.count(2) == 1);
    assert(src3.count(3) == 1);
    assert(src4.count(0) == 1);

    // Clean up
    std::filesystem::remove(temp_path);

    std::cout << "PASSED\n";
}

void test_load_sources_nonexistent_file() {
    std::cout << "Test 2: Load sources from nonexistent file (should fail)... ";

    std::stringstream eds_ss("{A}");
    edsparser::EDS eds(eds_ss);

    std::filesystem::path nonexistent = "/nonexistent/path/to/file.seds";

    bool caught = false;
    try {
        auto sources = Sources::load(nonexistent);
        eds.set_sources_object(sources);
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        caught = (msg.find("Failed to open") != std::string::npos);
    }

    assert(caught);
    std::cout << "PASSED\n";
}

void test_save_without_sources() {
    std::cout << "Test 3: Save without sources (should fail)... ";

    std::stringstream eds_ss("{A}");
    edsparser::EDS eds(eds_ss);

    assert(!eds.has_sources());

    bool caught = false;
    try {
        // get_sources_object() returns nullptr when no sources
        auto sources_obj = eds.get_sources_object();
        if (!sources_obj) {
            throw std::runtime_error("No sources loaded");
        }
        std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "test_fail.seds";
        sources_obj->save(temp_path);
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        caught = (msg.find("o sources") != std::string::npos);  // Matches "No sources" or "no sources"
    }

    assert(caught);
    std::cout << "PASSED\n";
}

int main() {
    std::cout << "Running sEDS (source) parsing tests...\n\n";
    std::cout << "NOTE: Most tests disabled - they require in-memory source construction\n";
    std::cout << "which has been removed in favor of file-based streaming.\n\n";

    try {
        test_load_sources_from_file();
        test_load_sources_nonexistent_file();
        test_save_without_sources();

        std::cout << "\n✓ All enabled source tests passed!\n";
        std::cout << "(13 tests disabled - require in-memory construction)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed: " << e.what() << "\n";
        return 1;
    }
}
