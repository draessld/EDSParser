// Statistics tests - Testing EDS statistics calculation and source statistics
#include "formats/eds.hpp"
#include <sstream>
#include <iostream>
#include <cassert>
#include <cmath>
#include <set>
#include <filesystem>
#include <fstream>

using namespace edsparser;

// Helper function to load EDS from stringstreams (writes to temp files)
EDS load_eds_from_streams(std::stringstream& eds_ss, std::stringstream& seds_ss) {
    auto temp_dir = std::filesystem::temp_directory_path();
    auto eds_path = temp_dir / "test_eds.tmp";
    auto seds_path = temp_dir / "test_seds.tmp";

    // Write EDS stream to file
    {
        std::ofstream eds_file(eds_path);
        eds_file << eds_ss.str();
    }

    // Write SEDS stream to file
    {
        std::ofstream seds_file(seds_path);
        seds_file << seds_ss.str();
    }

    // Load and return
    auto eds = EDS::load(eds_path, seds_path);

    // Clean up temp files
    std::filesystem::remove(eds_path);
    std::filesystem::remove(seds_path);

    return eds;
}

// Local helper: compute source statistics directly from the Sources object.
struct SrcStats {
    size_t num_paths = 0;
    size_t max_paths_per_string = 0;
    double avg_paths_per_string = 0.0;
};

SrcStats compute_src_stats(const EDS& eds) {
    SrcStats result;
    if (!eds.has_sources()) return result;
    auto sources = eds.get_sources_object();
    size_t cardinality = sources->cardinality();
    if (cardinality == 0) return result;
    std::set<int> all_paths;
    size_t total_set_size = 0;
    for (size_t i = 0; i < cardinality; ++i) {
        auto paths = sources->read_source(i);
        all_paths.insert(paths.begin(), paths.end());
        if (paths.size() > result.max_paths_per_string)
            result.max_paths_per_string = paths.size();
        total_set_size += paths.size();
    }
    all_paths.erase(0);
    result.num_paths = all_paths.size();
    result.avg_paths_per_string = static_cast<double>(total_set_size) / cardinality;
    return result;
}

void test_basic_statistics() {
    std::cout << "Test 1: Basic statistics calculation... ";

    // EDS: {ACGT}{A,ACA}{CGT}{T,TG}
    // Context lengths: 4 (ACGT), 3 (CGT) -> min=3, max=4, avg=3.5
    std::stringstream ss("{ACGT}{A,ACA}{CGT}{T,TG}");
    EDS eds(ss);

    const auto& meta = eds.get_metadata();

    assert(meta.min_context_length == 3);
    assert(meta.max_context_length == 4);
    assert(std::abs(meta.avg_context_length - 3.5) < 0.01);
    assert(meta.num_degenerate_symbols == 2);  // positions 1 and 3
    assert(meta.num_common_chars == 7);        // ACGT (4) + CGT (3)
    assert(meta.total_change_size == 7);       // A(1) + ACA(3) + T(1) + TG(2)
    assert(meta.num_empty_strings == 0);

    std::cout << "PASSED\n";
}

void test_empty_string_statistics() {
    std::cout << "Test 2: Statistics with empty strings... ";

    // EDS: {AC}{,A,T}{GT}
    // Empty string counts toward statistics
    std::stringstream ss("{AC}{,A,T}{GT}");
    EDS eds(ss);

    const auto& meta = eds.get_metadata();

    assert(meta.num_empty_strings == 1);
    assert(meta.min_context_length == 2);  // Both AC and GT have length 2
    assert(meta.max_context_length == 2);
    assert(meta.num_degenerate_symbols == 1);

    std::cout << "PASSED\n";
}

void test_all_degenerate() {
    std::cout << "Test 3: Statistics with all degenerate symbols... ";

    // EDS: {A,T}{C,G}{A,T}
    // No common blocks, context should be 0
    std::stringstream ss("{A,T}{C,G}{A,T}");
    EDS eds(ss);

    const auto& meta = eds.get_metadata();

    assert(meta.min_context_length == 0);
    assert(meta.max_context_length == 0);
    assert(meta.avg_context_length == 0.0);
    assert(meta.num_degenerate_symbols == 3);
    assert(meta.num_common_chars == 0);

    std::cout << "PASSED\n";
}

void test_metadata_statistics() {
    std::cout << "Test 4: Statistics from metadata (METADATA_ONLY mode)... ";

    // Create temporary file
    std::filesystem::path temp_file = "test_metadata_stats.eds";
    {
        std::ofstream ofs(temp_file);
        ofs << "{AAAA}{G,GG}{TTTT}{C,CC}";
    }

    // Load (always uses METADATA_ONLY mode)
    EDS eds = EDS::load(temp_file);
    const auto& meta = eds.get_metadata();

    // Context lengths: AAAA (4), TTTT (4) -> min=4, max=4, avg=4.0
    assert(meta.min_context_length == 4);
    assert(meta.max_context_length == 4);
    assert(std::abs(meta.avg_context_length - 4.0) < 0.01);
    assert(meta.num_degenerate_symbols == 2);
    assert(meta.num_common_chars == 8);  // AAAA (4) + TTTT (4)

    // Cleanup
    std::filesystem::remove(temp_file);

    std::cout << "PASSED\n";
}

void test_source_statistics_basic() {
    std::cout << "Test 5: Basic source statistics... ";

    // EDS: {ACGT}{A,ACA}{CGT}{T,TG}
    // sEDS: {0}{1,3}{2}{4,5}  - strings: ACGT, A, ACA, CGT, T, TG
    std::stringstream eds_ss("{ACGT}{A,ACA}{CGT}{T,TG}");
    std::stringstream seds_ss("{0}{1,3}{2}{4,5}{6}{7}");

    EDS eds = load_eds_from_streams(eds_ss, seds_ss);
    auto src = compute_src_stats(eds);

    // 7 distinct paths: 1..7. The leading {0} is the universal marker, not a
    // path id, and compute_src_stats above erases it deliberately — so counting
    // it as an eighth path was double-counting the marker.
    assert(src.num_paths == 7);

    // Max paths per string: {1,3} has 2 paths
    assert(src.max_paths_per_string == 2);

    // Average: (1+2+1+2+1+1)/6 = 8/6 = 1.33...
    assert(std::abs(src.avg_paths_per_string - 1.333) < 0.01);

    std::cout << "PASSED\n";
}

void test_source_statistics_all_universal() {
    std::cout << "Test 6: Source statistics with universal paths... ";

    // All strings have {0} (universal)
    std::stringstream eds_ss("{AC}{GT}");
    std::stringstream seds_ss("{0}{0}");

    EDS eds = load_eds_from_streams(eds_ss, seds_ss);
    auto src = compute_src_stats(eds);

    // Only path 0 is used, but 0 is the universal marker and is erased
    assert(src.num_paths == 0);
    assert(src.max_paths_per_string == 1);
    assert(std::abs(src.avg_paths_per_string - 1.0) < 0.01);

    std::cout << "PASSED\n";
}

void test_source_statistics_single_string_multi_paths() {
    std::cout << "Test 7: Source statistics with single string having multiple paths... ";

    // One string with many paths
    std::stringstream eds_ss("{ACGT}");
    std::stringstream seds_ss("{1,2,3,4,5}");

    EDS eds = load_eds_from_streams(eds_ss, seds_ss);
    auto src = compute_src_stats(eds);

    assert(src.num_paths == 5);
    assert(src.max_paths_per_string == 5);
    assert(std::abs(src.avg_paths_per_string - 5.0) < 0.01);

    std::cout << "PASSED\n";
}

void test_source_statistics_file_mode() {
    std::cout << "Test 8: Source statistics from file (FULL mode)... ";

    // Create temporary files
    std::filesystem::path eds_file = "test_source_stats.eds";
    std::filesystem::path seds_file = "test_source_stats.seds";

    {
        std::ofstream eds_ofs(eds_file);
        eds_ofs << "{ACGT}{A,ACA}{CGT}";

        std::ofstream seds_ofs(seds_file);
        seds_ofs << "{0}{1,2}{3}{4,5}";
    }

    // Load with sources (always uses METADATA_ONLY mode)
    EDS eds = EDS::load(eds_file, seds_file);
    auto src = compute_src_stats(eds);

    // Paths: 0, 1, 2, 3, 4, 5 = 6 distinct paths (0 is universal, erased → 5 remain)
    // Actually 0 is erased: {0,1,2,3,4,5} → erase(0) → {1,2,3,4,5} → 5 paths
    assert(src.num_paths == 5);

    // Max: {1,2} has 2, {4,5} has 2
    assert(src.max_paths_per_string == 2);

    // Average: the EDS has four strings (ACGT, A, ACA, CGT) and the SEDS four
    // entries, so (1+2+1+2)/4 = 1.5. The old 7/5 counted a fifth entry that
    // neither file has.
    assert(std::abs(src.avg_paths_per_string - 1.5) < 0.01);

    // Cleanup
    std::filesystem::remove(eds_file);
    std::filesystem::remove(seds_file);

    std::cout << "PASSED\n";
}

void test_statistics_without_sources() {
    std::cout << "Test 9: Statistics without sources should have zero source stats... ";

    std::stringstream ss("{ACGT}{A,ACA}{CGT}");
    EDS eds(ss);

    assert(!eds.has_sources());
    auto src = compute_src_stats(eds);
    assert(src.num_paths == 0);
    assert(src.max_paths_per_string == 0);
    assert(src.avg_paths_per_string == 0.0);

    std::cout << "PASSED\n";
}

void test_metadata_preservation() {
    std::cout << "Test 10: Metadata contains all statistics fields... ";

    std::stringstream eds_ss("{ACGT}{A,T}{GGG}");
    std::stringstream seds_ss("{0}{1,2}{3}{4}");

    EDS eds = load_eds_from_streams(eds_ss, seds_ss);
    const auto& meta = eds.get_metadata();

    assert(meta.min_context_length > 0);
    assert(meta.max_context_length > 0);
    assert(meta.avg_context_length > 0);
    assert(meta.num_degenerate_symbols >= 0);
    assert(meta.num_common_chars > 0);
    assert(meta.total_change_size >= 0);
    assert(meta.num_empty_strings >= 0);

    auto src = compute_src_stats(eds);
    assert(src.num_paths > 0);
    assert(src.max_paths_per_string > 0);
    assert(src.avg_paths_per_string > 0);

    std::cout << "PASSED\n";
}

void test_large_path_numbers() {
    std::cout << "Test 11: Source statistics with large path numbers... ";

    // Test with path numbers like 100, 200, etc.
    std::stringstream eds_ss("{A}{T}");
    std::stringstream seds_ss("{100,200,300}{400,500}");

    EDS eds = load_eds_from_streams(eds_ss, seds_ss);
    auto src = compute_src_stats(eds);

    // 5 distinct paths: 100, 200, 300, 400, 500
    assert(src.num_paths == 5);
    assert(src.max_paths_per_string == 3);  // First string has 3 paths

    // Average: (3+2)/2 = 2.5
    assert(std::abs(src.avg_paths_per_string - 2.5) < 0.01);

    std::cout << "PASSED\n";
}

void test_single_path_coverage() {
    std::cout << "Test 12: Source statistics with overlapping paths... ";

    // Same path appears in multiple strings
    std::stringstream eds_ss("{A}{T}{G}");
    std::stringstream seds_ss("{1}{1,2}{1}");

    EDS eds = load_eds_from_streams(eds_ss, seds_ss);
    auto src = compute_src_stats(eds);

    // Only 2 distinct paths even though path 1 appears 3 times
    assert(src.num_paths == 2);
    assert(src.max_paths_per_string == 2);

    // Average: (1+2+1)/3 = 4/3 = 1.33...
    assert(std::abs(src.avg_paths_per_string - 1.333) < 0.01);

    std::cout << "PASSED\n";
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "Running Statistics Tests\n";
    std::cout << "===========================================\n\n";

    try {
        // Basic statistics tests
        test_basic_statistics();
        test_empty_string_statistics();
        test_all_degenerate();
        test_metadata_statistics();

        // Source statistics tests
        test_source_statistics_basic();
        test_source_statistics_all_universal();
        test_source_statistics_single_string_multi_paths();
        test_source_statistics_file_mode();
        test_statistics_without_sources();

        // Integration tests
        test_metadata_preservation();
        test_large_path_numbers();
        test_single_path_coverage();

        std::cout << "\n===========================================\n";
        std::cout << "All statistics tests PASSED! ✓\n";
        std::cout << "===========================================\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n\nFATAL ERROR: " << e.what() << "\n";
        return 1;
    }
}
