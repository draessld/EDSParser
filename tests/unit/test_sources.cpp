// sEDS (source) parsing tests - MOSTLYDISABLED
// Most tests disabled because they require in-memory source construction
// which has been removed in favor of file-based streaming.

#include "formats/eds.hpp"
#include "formats/sources.hpp"
#include <sstream>
#include <iostream>
#include <cassert>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
#include <set>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// EDZ test helper — write a minimal EDZ file with given source sets.
// Mirrors the binary layout described in sources.cpp so the tests are
// independent of the implementation under test.
// ─────────────────────────────────────────────────────────────────────────────
static void write_test_edz(const std::filesystem::path& path,
                             const std::vector<std::vector<int>>& sets) {
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
    assert(std::binary_search(src0.begin(), src0.end(), 0));
    assert(std::binary_search(src1.begin(), src1.end(), 1));
    assert(std::binary_search(src2.begin(), src2.end(), 2));
    assert(std::binary_search(src3.begin(), src3.end(), 3));
    assert(std::binary_search(src4.begin(), src4.end(), 0));

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

    std::vector<std::vector<int>> expected = {
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

    std::vector<std::vector<int>> sets = {{1}, {2}, {1, 2}};
    auto path = std::filesystem::temp_directory_path() / "test_edz_detect.edz";
    write_test_edz(path, sets);

    assert(Sources::detect_format(path) == Sources::Format::EDZ);

    auto sources = Sources::load(path);  // auto-detect
    assert(sources->cardinality() == 3);
    assert(sources->read_source(0) == (std::vector<int>{1}));
    assert(sources->read_source(1) == (std::vector<int>{2}));
    assert(sources->read_source(2) == (std::vector<int>{1, 2}));

    std::filesystem::remove(path);
    std::cout << "PASSED\n";
}

void test_edz_save_load_roundtrip() {
    std::cout << "EDZ Test 3: Save EDZ and reload (roundtrip)... ";

    std::vector<std::vector<int>> original = {{1}, {2}, {3}, {1, 2}, {0}};
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
    std::vector<std::vector<int>> sets = {
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

    std::vector<std::vector<int>> sets;
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

// ─────────────────────────────────────────────────────────────────────────────
// EDZ *bitset* format tests (flags=0x0002) — tests for the new streaming write
// API (write_edz_header / write_edz_entry / write_edz_finalize) and the
// corresponding parser path in parse_edz() that detects flags & 0x0002.
// ─────────────────────────────────────────────────────────────────────────────

// Write an EDZ bitset file via the public streaming API and return the path.
static std::filesystem::path write_edz_bitset(
        const std::filesystem::path& path,
        const std::vector<PathSet>& sets,
        size_t num_paths)
{
    std::ofstream os(path, std::ios::binary);
    assert(os.is_open());
    Sources::write_edz_header(os, num_paths);
    for (const auto& ps : sets)
        Sources::write_edz_entry(os, ps, num_paths);
    Sources::write_edz_finalize(os, sets.size());
    return path;
}

void test_edz_bitset_roundtrip() {
    std::cout << "EDZ Bitset Test 1: Streaming write API round-trip... ";

    // Covers explicit sets, universal ({0}), and multi-path sets.
    // num_paths=6 → bpe=1.
    const size_t np = 6;
    std::vector<PathSet> sets = {
        {0},          // universal → all-ones (0x3F for 6 bits)
        {1},          // bit 0
        {6},          // bit 5
        {1, 2, 3},    // bits 0-2
        {2, 4, 6},    // bits 1,3,5
        {1,2,3,4,5,6},// all explicit → NOT complement (just explicit)
    };

    auto path = std::filesystem::temp_directory_path() / "test_edz_bitset_rt.edz";
    write_edz_bitset(path, sets, np);

    // Verify flags byte
    {
        std::ifstream in(path, std::ios::binary);
        char buf[6]; in.read(buf, 6);
        uint16_t flags = static_cast<uint8_t>(buf[4]) | (static_cast<uint8_t>(buf[5]) << 8);
        assert(flags == 0x0002 && "EDZ bitset header must have flags=0x0002");
    }

    auto src = Sources::load(path);
    assert(src->cardinality() == sets.size());

    // {1,2,3,4,5,6} with np=6 → all bits set → decoded as {0} (universal)
    std::vector<PathSet> expected = sets;
    expected.back() = {0};  // all-explicit-all-paths normalises to universal

    for (size_t i = 0; i < expected.size(); ++i) {
        PathSet got = src->read_source(i);
        if (got != expected[i]) {
            std::cerr << "\nEntry " << i << " mismatch: got={";
            for (int p : got) std::cerr << p << ",";
            std::cerr << "} expected={";
            for (int p : expected[i]) std::cerr << p << ",";
            std::cerr << "}\n";
            assert(false && "EDZ bitset round-trip mismatch");
        }
    }

    std::filesystem::remove(path);
    std::cout << "PASSED\n";
}

void test_edz_bitset_multibyte() {
    std::cout << "EDZ Bitset Test 2: Multi-byte bitset (>8 paths)... ";

    // 16 paths → bpe=2; 64 paths → bpe=8; tests byte-boundary correctness
    for (size_t np : {size_t(16), size_t(64)}) {
        std::vector<PathSet> sets = {
            {0},            // universal
            {1},            // first path (bit 0, byte 0)
            {static_cast<int>(np)},   // last path (bit np-1, last byte)
            {1, static_cast<int>(np / 2), static_cast<int>(np)},  // spread across bytes
        };

        std::string fname = "test_edz_bitset_" + std::to_string(np) + "p.edz";
        auto path = std::filesystem::temp_directory_path() / fname;
        write_edz_bitset(path, sets, np);

        auto src = Sources::load(path);
        assert(src->cardinality() == sets.size());
        for (size_t i = 0; i < sets.size(); ++i) {
            assert(src->read_source(i) == sets[i]);
        }
        std::filesystem::remove(path);
    }

    std::cout << "PASSED\n";
}

void test_edz_bitset_complement_encoding() {
    std::cout << "EDZ Bitset Test 3: Complement-encoded sets survive SEDS→EDZ→SEDS... ";

    // Write SEDS with complement form: {0,3} means all-except-3.
    // write_seds_entry_msa used by msa_transforms writes {0,exceptions}.
    // Here we directly test that SEDS complement sets become correct bitsets
    // when loaded as a Sources and re-read.

    // Build a SEDS file with complement and range notation, load it, re-write EDZ.
    const auto tmp = std::filesystem::temp_directory_path();
    const auto seds_path = tmp / "test_complement.seds";
    const auto edz_path  = tmp / "test_complement.edz";

    // 5 paths. Entries:
    //   {0}       → all paths
    //   {0,3}     → paths 1,2,4,5  (all except 3)
    //   {0,1,5}   → paths 2,3,4    (all except 1 and 5)
    //   {1-3}     → paths 1,2,3    (range)
    //   {2,4}     → paths 2 and 4
    { std::ofstream f(seds_path); f << "{0}{0,3}{0,1,5}{1-3}{2,4}"; }

    auto seds_src = Sources::load(seds_path);
    assert(seds_src->cardinality() == 5);

    // Expected decoded PathSets (SEDS {0,x} = universal excluding x)
    std::vector<PathSet> expected = {
        {0},
        {0, 3},       // complement of {3}
        {0, 1, 5},    // complement of {1,5}
        {1, 2, 3},    // range 1-3
        {2, 4},       // explicit
    };
    for (size_t i = 0; i < expected.size(); ++i)
        assert(seds_src->read_source(i) == expected[i]);

    // Save as EDZ bitset using static streaming API.
    // save() dispatches on the loaded format (SEDS here), so we use the API directly.
    {
        std::ofstream edz_file(edz_path, std::ios::binary);
        Sources::write_edz_header(edz_file, 5);
        for (size_t i = 0; i < seds_src->cardinality(); ++i)
            Sources::write_edz_entry(edz_file, seds_src->read_source(i), 5);
        Sources::write_edz_finalize(edz_file, seds_src->cardinality());
    }

    // Reload EDZ and compare semantically (EDZ returns explicit form; SEDS may use complement).
    auto edz_src = Sources::load(edz_path);
    assert(edz_src->cardinality() == 5);
    size_t np = edz_src->num_paths();
    // Expand {0, e1, e2} complement form to explicit sorted list for comparison.
    auto expand = [&](const PathSet& ps) -> PathSet {
        if (ps.empty() || ps[0] != 0) return ps;
        std::set<int> excl(ps.begin() + 1, ps.end());
        PathSet r;
        for (int id = 1; id <= static_cast<int>(np); ++id)
            if (!excl.count(id)) r.push_back(id);
        return r;
    };
    for (size_t i = 0; i < expected.size(); ++i) {
        PathSet got      = expand(edz_src->read_source(i));
        PathSet want     = expand(expected[i]);
        if (got != want) {
            std::cerr << "\nEntry " << i << ": got(expanded)={";
            for (int p : got)  std::cerr << p << ",";
            std::cerr << "} expected(expanded)={";
            for (int p : want) std::cerr << p << ",";
            std::cerr << "}\n";
            assert(false && "complement/range entry mismatch after SEDS→EDZ→load");
        }
    }

    std::filesystem::remove(seds_path);
    std::filesystem::remove(edz_path);
    std::cout << "PASSED\n";
}

void test_edz_bitset_intersection() {
    std::cout << "EDZ Bitset Test 4: merge_adjacent_sources on EDZ bitset backend... ";

    // symbol1: [{1,2}, {3,4}]  symbol2: [{2,3}, {4,5}]
    // num_paths = 5
    // Expected intersections (non-empty pairs):
    //   {1,2} ∩ {2,3} = {2}
    //   {1,2} ∩ {4,5} = {}  (empty, skipped)
    //   {3,4} ∩ {2,3} = {3}
    //   {3,4} ∩ {4,5} = {4}
    // Result: [{2}, {3}, {4}]

    const size_t np = 5;
    std::vector<PathSet> sets = {{1,2}, {3,4}, {2,3}, {4,5}};

    auto path = std::filesystem::temp_directory_path() / "test_edz_bitset_isect.edz";
    write_edz_bitset(path, sets, np);

    auto src = Sources::load(path);
    // symbol1 starts at index 0 (size 2), symbol2 at index 2 (size 2)
    auto merged = src->merge_adjacent_sources(0, 2, 2, 2);

    std::vector<PathSet> expected_merged = {{2}, {3}, {4}};
    assert(merged.size() == expected_merged.size());
    for (size_t i = 0; i < expected_merged.size(); ++i)
        assert(merged[i] == expected_merged[i]);

    // Also test universal set intersection: {0} ∩ {1,2} = {1,2}
    const size_t np2 = 4;
    std::vector<PathSet> sets2 = {{0}, {1,2}, {3,4}, {2,3}};
    auto path2 = std::filesystem::temp_directory_path() / "test_edz_bitset_isect2.edz";
    write_edz_bitset(path2, sets2, np2);

    auto src2 = Sources::load(path2);
    auto merged2 = src2->merge_adjacent_sources(0, 2, 2, 2);
    // {0} ∩ {3,4} = {3,4}; {0} ∩ {2,3} = {2,3}; {1,2} ∩ {3,4} = {}; {1,2} ∩ {2,3} = {2}
    std::vector<PathSet> expected2 = {{3,4}, {2,3}, {2}};
    assert(merged2.size() == expected2.size());
    for (size_t i = 0; i < expected2.size(); ++i)
        assert(merged2[i] == expected2[i]);

    std::filesystem::remove(path);
    std::filesystem::remove(path2);
    std::cout << "PASSED\n";
}

void test_edz_bitset_copy_range() {
    std::cout << "EDZ Bitset Test 5: copy_range_to_stream re-serialises to SEDS text... ";

    // Write 4 entries as EDZ bitset, then copy_range_to_stream and check SEDS output.
    const size_t np = 4;
    std::vector<PathSet> sets = {{0}, {1,3}, {2,4}, {0,2}};
    auto path = std::filesystem::temp_directory_path() / "test_edz_copy_range.edz";
    write_edz_bitset(path, sets, np);

    auto src = Sources::load(path);

    // copy_range_to_stream should produce valid SEDS text parseable back to the same sets
    std::ostringstream oss;
    src->copy_range_to_stream(0, 4, oss);
    std::string seds_text = oss.str();

    std::cout << "\n    SEDS text: " << seds_text;

    // Parse the SEDS text back by writing it to a temp file and loading
    auto seds_path = std::filesystem::temp_directory_path() / "test_edz_copy_range.seds";
    { std::ofstream f(seds_path); f << seds_text; }

    auto seds_src = Sources::load(seds_path);
    assert(seds_src->cardinality() == 4);
    for (size_t i = 0; i < sets.size(); ++i) {
        assert(seds_src->read_source(i) == sets[i]);
    }

    std::filesystem::remove(path);
    std::filesystem::remove(seds_path);
    std::cout << "\n    PASSED\n";
}

// ── Helper: build presence bitvec for a given set of "present" (non-universal) indices ──
static std::vector<uint8_t> make_bitvec(size_t total, const std::vector<size_t>& present) {
    size_t sz = (total + 7) / 8;
    std::vector<uint8_t> bv(sz, 0);
    for (size_t idx : present) bv[idx / 8] |= uint8_t{1} << (idx % 8);
    return bv;
}

void test_edz_sparse_roundtrip() {
    std::cout << "EDZ Sparse Test 1: write + read EDZ_SPARSE; universal entries return {0}... ";

    // 8 strings, num_paths=4. Strings 0,2,4,6 are universal; 1,3,5,7 are non-universal.
    const size_t np = 4;
    const size_t total = 8;
    std::vector<PathSet> non_univ = {{1,2}, {3,4}, {1,3}, {2,4}};  // indices 1,3,5,7
    std::vector<size_t> present_idx = {1, 3, 5, 7};

    auto bv = make_bitvec(total, present_idx);
    auto path = std::filesystem::temp_directory_path() / "test_edz_sparse.edz";

    {
        std::ofstream edz(path, std::ios::binary);
        Sources::write_edz_sparse_header(edz, np);
        for (const auto& ps : non_univ)
            Sources::write_edz_entry(edz, ps, np);
        Sources::write_edz_sparse_finalize(edz, total, np, non_univ.size(), bv);
    }

    auto src = Sources::load(path);
    assert(src->cardinality() == total);
    assert(src->is_sparse());
    assert(src->m_degenerate() == non_univ.size());

    for (size_t i = 0; i < total; ++i) {
        PathSet got = src->read_source(i);
        bool should_univ = (i % 2 == 0);
        if (should_univ) {
            assert(got == PathSet{0} && "expected universal {0}");
        } else {
            assert(got == non_univ[i / 2]);
        }
    }

    std::filesystem::remove(path);
    std::cout << "PASSED\n";
}

void test_seds_sparse_roundtrip() {
    std::cout << "EDZ Sparse Test 2: write + read SEDS_SPARSE; universal entries return {0}... ";

    // 6 strings, strings 1 and 4 are non-universal.
    const size_t total = 6;
    std::vector<PathSet> non_univ = {{1,3}, {2,4}};
    std::vector<size_t> present_idx = {1, 4};

    auto bv = make_bitvec(total, present_idx);
    auto path = std::filesystem::temp_directory_path() / "test_seds_sparse.seds";

    {
        std::ofstream seds(path, std::ios::binary);
        // Write text entries for non-universal strings only
        seds << "{1,3}{2,4}";
        Sources::write_seds_sparse_finalize(seds, total, non_univ.size(), bv);
    }

    auto src = Sources::load(path);
    assert(src->cardinality() == total);
    assert(src->is_sparse());
    assert(src->m_degenerate() == non_univ.size());

    for (size_t i = 0; i < total; ++i) {
        PathSet got = src->read_source(i);
        if (i == 1) {
            assert(got == non_univ[0]);
        } else if (i == 4) {
            assert(got == non_univ[1]);
        } else {
            assert(got == PathSet{0} && "expected universal {0}");
        }
    }

    std::filesystem::remove(path);
    std::cout << "PASSED\n";
}

void test_sparse_vs_dense_agreement() {
    std::cout << "EDZ Sparse Test 3: sparse and dense EDZ agree on all reads... ";

    // Build an identical set of sources in both dense EDZ and sparse EDZ, then compare.
    const size_t np = 5;
    // 10 strings: even indices are universal, odd have explicit paths
    std::vector<PathSet> all_sets;
    std::vector<PathSet> non_univ_sets;
    std::vector<size_t> present_idx;
    for (size_t i = 0; i < 10; ++i) {
        if (i % 2 == 0) {
            all_sets.push_back({0});
        } else {
            PathSet ps = {static_cast<int>(i % np + 1)};
            all_sets.push_back(ps);
            non_univ_sets.push_back(ps);
            present_idx.push_back(i);
        }
    }

    auto dense_path  = std::filesystem::temp_directory_path() / "test_dense_agree.edz";
    auto sparse_path = std::filesystem::temp_directory_path() / "test_sparse_agree.edz";
    auto bv = make_bitvec(all_sets.size(), present_idx);

    // Write dense
    {
        std::ofstream f(dense_path, std::ios::binary);
        Sources::write_edz_header(f, np);
        for (const auto& ps : all_sets)
            Sources::write_edz_entry(f, ps, np);
        Sources::write_edz_finalize(f, all_sets.size());
    }
    // Write sparse
    {
        std::ofstream f(sparse_path, std::ios::binary);
        Sources::write_edz_sparse_header(f, np);
        for (const auto& ps : non_univ_sets)
            Sources::write_edz_entry(f, ps, np);
        Sources::write_edz_sparse_finalize(f, all_sets.size(), np, non_univ_sets.size(), bv);
    }

    auto dense  = Sources::load(dense_path);
    auto sparse = Sources::load(sparse_path);

    assert(dense->cardinality()  == all_sets.size());
    assert(sparse->cardinality() == all_sets.size());
    assert(sparse->is_sparse());

    // EDZ returns explicit form; universal {0} maps to all bits set → bitset_to_pathset
    // returns {0} for dense, and sparse also returns {0} from the bitvec check.
    for (size_t i = 0; i < all_sets.size(); ++i) {
        PathSet d = dense->read_source(i);
        PathSet s = sparse->read_source(i);
        assert(d == s && "dense and sparse EDZ disagree");
    }

    // copy_range_to_stream should produce identical SEDS text
    std::ostringstream oss_d, oss_s;
    dense->copy_range_to_stream(0, all_sets.size(), oss_d);
    sparse->copy_range_to_stream(0, all_sets.size(), oss_s);
    assert(oss_d.str() == oss_s.str() && "copy_range_to_stream dense vs sparse mismatch");

    std::filesystem::remove(dense_path);
    std::filesystem::remove(sparse_path);
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

        std::cout << "\n--- EDZ varint format tests (old format, flags=0x0000) ---\n";
        test_edz_load_basic();
        test_edz_auto_detect();
        test_edz_save_load_roundtrip();
        test_edz_large_path_ids();
        test_edz_lru_cache();
        test_edz_nonexistent_file();

        std::cout << "\n--- EDZ bitset format tests (new format, flags=0x0002) ---\n";
        test_edz_bitset_roundtrip();
        test_edz_bitset_multibyte();
        test_edz_bitset_complement_encoding();
        test_edz_bitset_intersection();
        test_edz_bitset_copy_range();

        std::cout << "\n--- Sparse format tests (EDZ_SPARSE flags=0x0006, SEDS_SPARSE) ---\n";
        test_edz_sparse_roundtrip();
        test_seds_sparse_roundtrip();
        test_sparse_vs_dense_agreement();

        std::cout << "\nAll source tests passed!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed: " << e.what() << "\n";
        return 1;
    }
}
