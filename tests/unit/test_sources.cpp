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
#include <set>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// EDZ test helper — write a minimal EDZ file with given source sets.
// Mirrors the binary layout described in sources.cpp so the tests are
// independent of the implementation under test.
// ─────────────────────────────────────────────────────────────────────────────
static void write_test_edz(const std::filesystem::path& path,
                             const std::vector<std::set<int>>& sets) {
    auto varint = [](uint64_t v, std::vector<uint8_t>& out) {
        do {
            uint8_t b = v & 0x7F;
            v >>= 7;
            if (v) b |= 0x80;
            out.push_back(b);
        } while (v);
    };
    auto w64 = [](std::ostream& os, uint64_t v) {
        for (int i = 0; i < 8; ++i, v >>= 8) {
            char b = static_cast<char>(v & 0xFF);
            os.write(&b, 1);
        }
    };
    auto w32 = [](std::ostream& os, uint32_t v) {
        for (int i = 0; i < 4; ++i, v >>= 8) {
            char b = static_cast<char>(v & 0xFF);
            os.write(&b, 1);
        }
    };
    auto w16 = [](std::ostream& os, uint16_t v) {
        char lo = static_cast<char>(v & 0xFF);
        char hi = static_cast<char>(v >> 8);
        os.write(&lo, 1);
        os.write(&hi, 1);
    };

    const uint64_t card = sets.size();
    const uint64_t data_start = 16 + card * 12;  // header(16) + index(card*12)

    std::vector<std::vector<uint8_t>> blobs(card);
    for (uint64_t i = 0; i < card; ++i) {
        varint(static_cast<uint64_t>(sets[i].size()), blobs[i]);
        for (int id : sets[i]) varint(static_cast<uint64_t>(id), blobs[i]);
    }

    std::ofstream os(path, std::ios::binary);
    // Header
    os.write("EDZ", 3);
    char nul = '\0';
    os.write(&nul, 1);
    w16(os, 0);    // flags: uncompressed
    w16(os, 0);    // reserved
    w64(os, card);
    // Index
    uint64_t off = data_start;
    for (uint64_t i = 0; i < card; ++i) {
        w64(os, off);
        w32(os, static_cast<uint32_t>(blobs[i].size()));
        off += blobs[i].size();
    }
    // Data
    for (uint64_t i = 0; i < card; ++i) {
        if (!blobs[i].empty())
            os.write(reinterpret_cast<const char*>(blobs[i].data()),
                     static_cast<std::streamsize>(blobs[i].size()));
    }
}

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

// ─────────────────────────────────────────────────────────────────────────────
// EDZ tests
// ─────────────────────────────────────────────────────────────────────────────

void test_edz_load_basic() {
    std::cout << "EDZ Test 1: Load basic EDZ file... ";

    std::vector<std::set<int>> expected = {
        {0},        // universal marker
        {1},
        {2},
        {1, 2},
        {3, 4, 5}
    };

    auto path = std::filesystem::temp_directory_path() / "test_edz_basic.edz";
    write_test_edz(path, expected);

    auto sources = Sources::load(path, Sources::Format::EDZ);
    assert(sources->cardinality() == 5);

    for (size_t i = 0; i < expected.size(); ++i) {
        assert(sources->read_source(i) == expected[i]);
    }

    std::filesystem::remove(path);
    std::cout << "PASSED\n";
}

void test_edz_auto_detect() {
    std::cout << "EDZ Test 2: Auto-detect EDZ format from magic bytes... ";

    std::vector<std::set<int>> sets = {{1}, {2}, {1, 2}};
    auto path = std::filesystem::temp_directory_path() / "test_edz_detect.edz";
    write_test_edz(path, sets);

    assert(Sources::detect_format(path) == Sources::Format::EDZ);

    auto sources = Sources::load(path);  // auto-detect
    assert(sources->cardinality() == 3);
    assert(sources->read_source(0) == (std::set<int>{1}));
    assert(sources->read_source(1) == (std::set<int>{2}));
    assert(sources->read_source(2) == (std::set<int>{1, 2}));

    std::filesystem::remove(path);
    std::cout << "PASSED\n";
}

void test_edz_save_load_roundtrip() {
    std::cout << "EDZ Test 3: Save EDZ and reload (roundtrip)... ";

    std::vector<std::set<int>> original = {{1}, {2}, {3}, {1, 2}, {0}};
    auto path1 = std::filesystem::temp_directory_path() / "test_edz_rt1.edz";
    auto path2 = std::filesystem::temp_directory_path() / "test_edz_rt2.edz";

    // Write, load, save again (save() dispatches to save_edz since format_ == EDZ)
    write_test_edz(path1, original);
    auto sources1 = Sources::load(path1, Sources::Format::EDZ);
    sources1->save(path2);

    auto sources2 = Sources::load(path2, Sources::Format::EDZ);
    assert(sources2->cardinality() == original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        assert(sources2->read_source(i) == original[i]);
    }

    std::filesystem::remove(path1);
    std::filesystem::remove(path2);
    std::cout << "PASSED\n";
}

void test_edz_large_path_ids() {
    std::cout << "EDZ Test 4: Large path IDs requiring multi-byte varints... ";

    // Each entry exercises a different varint byte-count tier
    std::vector<std::set<int>> sets = {
        {127},      // 7-bit: one-byte varint
        {128},      // 8-bit: two-byte varint starts here
        {16383},    // 14-bit max: two-byte varint
        {16384},    // 15-bit: three-byte varint starts here
        {1000000}   // 20-bit: three-byte varint
    };

    auto path = std::filesystem::temp_directory_path() / "test_edz_large.edz";
    write_test_edz(path, sets);

    auto sources = Sources::load(path, Sources::Format::EDZ);
    for (size_t i = 0; i < sets.size(); ++i) {
        assert(sources->read_source(i) == sets[i]);
    }

    std::filesystem::remove(path);
    std::cout << "PASSED\n";
}

void test_edz_lru_cache() {
    std::cout << "EDZ Test 5: LRU cache eviction with EDZ backend... ";

    std::vector<std::set<int>> sets;
    for (int i = 1; i <= 20; ++i) sets.push_back({i});

    auto path = std::filesystem::temp_directory_path() / "test_edz_cache.edz";
    write_test_edz(path, sets);

    auto sources = Sources::load(path, Sources::Format::EDZ);
    sources->set_cache_capacity(5);  // cache smaller than dataset

    // Forward pass — fills and evicts cache
    for (size_t i = 0; i < 20; ++i) {
        assert(sources->read_source(i) == sets[i]);
    }
    // Reverse pass — all cache misses, verifies seeks still work after eviction
    for (int i = 19; i >= 0; --i) {
        assert(sources->read_source(static_cast<size_t>(i)) == sets[static_cast<size_t>(i)]);
    }

    std::filesystem::remove(path);
    std::cout << "PASSED\n";
}

void test_edz_nonexistent_file() {
    std::cout << "EDZ Test 6: Load non-existent EDZ file (should throw)... ";

    bool caught = false;
    try {
        Sources::load("/nonexistent/path/to/file.edz", Sources::Format::EDZ);
    } catch (const std::runtime_error& e) {
        caught = true;
    }
    assert(caught);

    std::cout << "PASSED\n";
}

int main() {
    std::cout << "Running sEDS (source) parsing tests...\n\n";
    std::cout << "NOTE: Most original tests disabled - they require in-memory source\n";
    std::cout << "construction which has been removed in favor of file-based streaming.\n\n";

    try {
        test_load_sources_from_file();
        test_load_sources_nonexistent_file();
        test_save_without_sources();

        std::cout << "\n--- EDZ format tests ---\n";
        test_edz_load_basic();
        test_edz_auto_detect();
        test_edz_save_load_roundtrip();
        test_edz_large_path_ids();
        test_edz_lru_cache();
        test_edz_nonexistent_file();

        std::cout << "\nAll source tests passed!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed: " << e.what() << "\n";
        return 1;
    }
}
