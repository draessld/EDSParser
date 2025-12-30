// sEDS (source) parsing tests
#include "formats/eds.hpp"
#include "formats/sources.hpp"
#include <sstream>
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>

void test_simple_sources() {
    std::cout << "Test 1: Simple sEDS parsing... ";

    // EDS: {ACGT}{A,ACA}{CGT}{T,TG}
    // String IDs: 0=ACGT, 1=A, 2=ACA, 3=CGT, 4=T, 5=TG
    std::stringstream eds_ss("{ACGT}{A,ACA}{CGT}{T,TG}");
    std::stringstream seds_ss("{0}{1,3}{2}{0}{1}{2,3}");

    edsparser::EDS eds(eds_ss, seds_ss);

    assert(eds.has_sources());
    assert(eds.cardinality() == 6);

    // str0 "ACGT": {0} = all paths
    auto src0 = eds.read_source(0);
    assert(src0.size() == 1);
    assert(src0.count(0) == 1);

    // str1 "A": {1,3}
    auto src1 = eds.read_source(1);
    assert(src1.size() == 2);
    assert(src1.count(1) == 1);
    assert(src1.count(3) == 1);

    // str2 "ACA": {2}
    auto src2 = eds.read_source(2);
    assert(src2.size() == 1);
    assert(src2.count(2) == 1);

    // str3 "CGT": {0} = all paths
    auto src3 = eds.read_source(3);
    assert(src3.size() == 1);
    assert(src3.count(0) == 1);

    // str4 "T": {1}
    auto src4 = eds.read_source(4);
    assert(src4.size() == 1);
    assert(src4.count(1) == 1);

    // str5 "TG": {2,3}
    auto src5 = eds.read_source(5);
    assert(src5.size() == 2);
    assert(src5.count(2) == 1);
    assert(src5.count(3) == 1);

    std::cout << "PASSED\n";
}

void test_load_sources_separately() {
    std::cout << "Test 2: Load sources separately... ";

    std::stringstream eds_ss("{AC}{,A,T}{GT}");
    edsparser::EDS eds(eds_ss);

    assert(!eds.has_sources());
    assert(eds.cardinality() == 5);

    // Now load sources - create Sources object manually
    auto sources = std::make_shared<Sources>(5, Sources::Format::SEDS, Sources::StoringMode::FULL);
    sources->set_source(0, {0});  // AC: {0}
    sources->set_source(1, {1});  // "": {1}
    sources->set_source(2, {2});  // A: {2}
    sources->set_source(3, {3});  // T: {3}
    sources->set_source(4, {0});  // GT: {0}
    eds.set_sources_object(sources);

    assert(eds.has_sources());
    auto src0 = eds.read_source(0);
    auto src1 = eds.read_source(1);
    auto src2 = eds.read_source(2);
    auto src3 = eds.read_source(3);
    auto src4 = eds.read_source(4);

    assert(src0.count(0) == 1);  // AC: {0}
    assert(src1.count(1) == 1);  // "": {1}
    assert(src2.count(2) == 1);  // A: {2}
    assert(src3.count(3) == 1);  // T: {3}
    assert(src4.count(0) == 1);  // GT: {0}

    std::cout << "PASSED\n";
}

void test_save_sources() {
    std::cout << "Test 3: Save sources... ";

    std::stringstream eds_ss("{A}{B,C}");
    std::stringstream seds_ss("{1}{2}{1,2}");

    edsparser::EDS eds(eds_ss, seds_ss);

    // Save sources using Sources object
    std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "test_seds_save_temp.seds";
    eds.get_sources_object()->save(temp_path);

    // Read back to verify
    std::ifstream ifs(temp_path);
    std::string result;
    std::getline(ifs, result);
    ifs.close();

    // Should output: {1}{2}{1,2}
    assert(result == "{1}{2}{1,2}");

    // Clean up
    std::filesystem::remove(temp_path);

    std::cout << "PASSED\n";
}

void test_sources_with_whitespace() {
    std::cout << "Test 4: Sources with whitespace... ";

    std::stringstream eds_ss("{A}{B}");
    std::stringstream seds_ss("{ 1 } { 2 , 3 }");

    edsparser::EDS eds(eds_ss, seds_ss);

    auto src0 = eds.read_source(0);
    auto src1 = eds.read_source(1);
    assert(src0.count(1) == 1);
    assert(src1.size() == 2);
    assert(src1.count(2) == 1);
    assert(src1.count(3) == 1);

    std::cout << "PASSED\n";
}

void test_all_paths_marker() {
    std::cout << "Test 5: All paths marker {0}... ";

    std::stringstream eds_ss("{ACGT}{A,T}");
    std::stringstream seds_ss("{0}{1}{2}");

    edsparser::EDS eds(eds_ss, seds_ss);

    // str0 "ACGT": {0} stored as literal 0
    auto src0 = eds.read_source(0);
    assert(src0.size() == 1);
    assert(src0.count(0) == 1);

    // str1 "A": {1}
    auto src1 = eds.read_source(1);
    assert(src1.count(1) == 1);

    // str2 "T": {2}
    auto src2 = eds.read_source(2);
    assert(src2.count(2) == 1);

    std::cout << "PASSED\n";
}

void test_invalid_cardinality_mismatch() {
    std::cout << "Test 6: Invalid - cardinality mismatch... ";

    std::stringstream eds_ss("{A}{B,C}");  // Cardinality = 3
    std::stringstream seds_ss("{1}{2}");   // Only 2 source sets

    bool caught = false;
    try {
        edsparser::EDS eds(eds_ss, seds_ss);
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        caught = (msg.find("cardinality") != std::string::npos);
    }

    assert(caught);
    std::cout << "PASSED\n";
}

void test_invalid_empty_path_set() {
    std::cout << "Test 7: Invalid - empty path set... ";

    std::stringstream eds_ss("{A}");
    std::stringstream seds_ss("{}");  // Empty path set

    bool caught = false;
    try {
        edsparser::EDS eds(eds_ss, seds_ss);
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        caught = (msg.find("Empty path set") != std::string::npos);
    }

    assert(caught);
    std::cout << "PASSED\n";
}

void test_invalid_negative_path_id() {
    std::cout << "Test 8: Invalid - negative path ID... ";

    std::stringstream eds_ss("{A}");
    std::stringstream seds_ss("{-1}");  // Negative path ID (will be caught as invalid character)

    bool caught = false;
    try {
        edsparser::EDS eds(eds_ss, seds_ss);
    } catch (const std::runtime_error& e) {
        // Will catch as "Invalid character" since '-' is not a digit
        caught = true;
    }

    assert(caught);
    std::cout << "PASSED\n";
}

void test_invalid_missing_bracket() {
    std::cout << "Test 9: Invalid - missing bracket... ";

    std::stringstream eds_ss("{A}{B}");
    std::stringstream seds_ss("{1}2}");  // Missing '{'

    bool caught = false;
    try {
        edsparser::EDS eds(eds_ss, seds_ss);
    } catch (const std::runtime_error& e) {
        caught = true;
    }

    assert(caught);
    std::cout << "PASSED\n";
}

void test_save_without_sources() {
    std::cout << "Test 10: Save without sources (should fail)... ";

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

void test_roundtrip() {
    std::cout << "Test 11: Roundtrip (parse → save → parse)... ";

    std::stringstream eds_ss("{ACGT}{A,ACA}{CGT}");
    std::stringstream seds_ss("{0}{1,2}{3}{0}");

    edsparser::EDS eds1(eds_ss, seds_ss);

    // Save sources to temp file
    std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "test_roundtrip_temp.seds";
    eds1.get_sources_object()->save(temp_path);

    // Parse again
    std::stringstream eds_ss2("{ACGT}{A,ACA}{CGT}");
    edsparser::EDS eds2(eds_ss2);

    // Load sources from temp file
    auto sources2 = Sources::load(temp_path, Sources::StoringMode::FULL);
    eds2.set_sources_object(sources2);

    // Compare - read sources one by one
    assert(eds1.cardinality() == eds2.cardinality());
    for (size_t i = 0; i < eds1.cardinality(); i++) {
        auto src1 = eds1.read_source(i);
        auto src2 = eds2.read_source(i);
        assert(src1 == src2);
    }

    // Clean up
    std::filesystem::remove(temp_path);

    std::cout << "PASSED\n";
}

void test_save_sources_to_file() {
    std::cout << "Test 12: Save sources to file... ";

    // Create EDS with sources
    std::stringstream eds_ss("{A}{B,C}");
    std::stringstream seds_ss("{1}{2}{1,2}");
    edsparser::EDS eds(eds_ss, seds_ss);

    // Save to file using Sources object
    std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "test_seds_save.seds";
    eds.get_sources_object()->save(temp_path);

    // Verify file exists and read it back
    assert(std::filesystem::exists(temp_path));
    std::ifstream ifs(temp_path);
    std::string content;
    std::getline(ifs, content);
    ifs.close();

    assert(content == "{1}{2}{1,2}");

    // Clean up
    std::filesystem::remove(temp_path);

    std::cout << "PASSED\n";
}

void test_load_sources_from_file() {
    std::cout << "Test 13: Load sources from file... ";

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
    auto sources = Sources::load(temp_path, Sources::StoringMode::FULL);
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

void test_roundtrip_sources_file() {
    std::cout << "Test 14: Roundtrip sources (save → load)... ";

    // Create original EDS with sources
    std::stringstream eds_ss1("{ACGT}{A,ACA}{CGT}");
    std::stringstream seds_ss("{0}{1,2}{3}{0}");
    edsparser::EDS eds1(eds_ss1, seds_ss);

    // Save sources to file using Sources object
    std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "test_seds_roundtrip.seds";
    eds1.get_sources_object()->save(temp_path);

    // Create new EDS and load sources from file
    std::stringstream eds_ss2("{ACGT}{A,ACA}{CGT}");
    edsparser::EDS eds2(eds_ss2);
    auto sources2 = Sources::load(temp_path, Sources::StoringMode::FULL);
    eds2.set_sources_object(sources2);

    // Compare - read sources one by one
    assert(eds1.cardinality() == eds2.cardinality());
    for (size_t i = 0; i < eds1.cardinality(); i++) {
        auto src1 = eds1.read_source(i);
        auto src2 = eds2.read_source(i);
        assert(src1 == src2);
    }

    // Clean up
    std::filesystem::remove(temp_path);

    std::cout << "PASSED\n";
}

void test_load_sources_nonexistent_file() {
    std::cout << "Test 15: Load sources from nonexistent file (should fail)... ";

    std::stringstream eds_ss("{A}");
    edsparser::EDS eds(eds_ss);

    std::filesystem::path nonexistent = "/nonexistent/path/to/file.seds";

    bool caught = false;
    try {
        auto sources = Sources::load(nonexistent, Sources::StoringMode::FULL);
        eds.set_sources_object(sources);
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        caught = (msg.find("Failed to open") != std::string::npos);
    }

    assert(caught);
    std::cout << "PASSED\n";
}

int main() {
    std::cout << "Running sEDS (source) parsing tests...\n\n";

    try {
        test_simple_sources();
        test_load_sources_separately();
        test_save_sources();
        test_sources_with_whitespace();
        test_all_paths_marker();
        test_invalid_cardinality_mismatch();
        test_invalid_empty_path_set();
        test_invalid_negative_path_id();
        test_invalid_missing_bracket();
        test_save_without_sources();
        test_roundtrip();
        test_save_sources_to_file();
        test_load_sources_from_file();
        test_roundtrip_sources_file();
        test_load_sources_nonexistent_file();

        std::cout << "\n✓ All source tests passed!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed: " << e.what() << "\n";
        return 1;
    }
}
