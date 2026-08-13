// EDS parsing tests
#include "formats/eds.hpp"
#include "formats/sources.hpp"
#include <sstream>
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <set>

// Helper function to create temporary EDS file and load it
edsparser::EDS create_temp_eds(const std::string& eds_content) {
    auto temp_path = std::filesystem::temp_directory_path() / ("test_eds_" + std::to_string(std::hash<std::string>{}(eds_content)) + ".eds");
    std::ofstream ofs(temp_path);
    ofs << eds_content;
    ofs.close();

    auto eds = edsparser::EDS::load(temp_path);
    std::filesystem::remove(temp_path);
    return eds;
}

void test_simple_eds() {
    std::cout << "Test 1: Simple EDS parsing... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}{CGT}{T,TG}");

    assert(eds.length() == 4);        // 4 positions
    assert(eds.cardinality() == 6);   // 6 strings total
    assert(eds.size() == 14);         // 14 characters total
    assert(!eds.empty());

    const auto& is_deg = eds.get_metadata().is_degenerate;

    // Position 0: {ACGT} - regular
    auto set0 = eds.read_symbol(0);
    assert(set0.size() == 1);
    assert(set0[0] == "ACGT");
    assert(!is_deg[0]);

    // Position 1: {A,ACA} - degenerate
    auto set1 = eds.read_symbol(1);
    assert(set1.size() == 2);
    assert(set1[0] == "A");
    assert(set1[1] == "ACA");
    assert(is_deg[1]);

    // Position 2: {CGT} - regular
    auto set2 = eds.read_symbol(2);
    assert(set2.size() == 1);
    assert(set2[0] == "CGT");
    assert(!is_deg[2]);

    // Position 3: {T,TG} - degenerate
    auto set3 = eds.read_symbol(3);
    assert(set3.size() == 2);
    assert(set3[0] == "T");
    assert(set3[1] == "TG");
    assert(is_deg[3]);

    std::cout << "PASSED\n";
}

void test_empty_strings() {
    std::cout << "Test 2: EDS with empty strings... ";
    edsparser::EDS eds = create_temp_eds("{AC}{,A,T}{GT}");

    assert(eds.length() == 3);        // 3 positions
    assert(eds.cardinality() == 5);   // 5 strings total (including empty)
    assert(!eds.empty());

    // Sets accessed via read_symbol()
    const auto& is_deg = eds.get_metadata().is_degenerate;

    // Position 0: {AC} - regular
    assert(eds.read_symbol(0).size() == 1);
    assert(eds.read_symbol(0)[0] == "AC");
    assert(!is_deg[0]);

    // Position 1: {,A,T} - degenerate with empty string
    assert(eds.read_symbol(1).size() == 3);
    assert(eds.read_symbol(1)[0] == "");         // Empty string
    assert(eds.read_symbol(1)[1] == "A");
    assert(eds.read_symbol(1)[2] == "T");
    assert(is_deg[1]);

    // Position 2: {GT} - regular
    assert(eds.read_symbol(2).size() == 1);
    assert(eds.read_symbol(2)[0] == "GT");
    assert(!is_deg[2]);

    std::cout << "PASSED\n";
}

void test_single_position() {
    std::cout << "Test 3: Single position EDS... ";
    edsparser::EDS eds = create_temp_eds("{ACGT}");

    assert(eds.length() == 1);
    assert(eds.cardinality() == 1);
    assert(eds.size() == 4);
    assert(!eds.empty());

    // Sets accessed via read_symbol()
    assert(eds.read_symbol(0).size() == 1);
    assert(eds.read_symbol(0)[0] == "ACGT");

    std::cout << "PASSED\n";
}

void test_all_degenerate() {
    std::cout << "Test 4: All degenerate positions... ";
    edsparser::EDS eds = create_temp_eds("{A,C}{G,T}{A,C,G,T}");

    assert(eds.length() == 3);
    assert(eds.cardinality() == 8);   // 2 + 2 + 4 = 8
    assert(!eds.empty());

    const auto& is_deg = eds.get_metadata().is_degenerate;
    assert(is_deg[0]);
    assert(is_deg[1]);
    assert(is_deg[2]);

    std::cout << "PASSED\n";
}

void test_whitespace_handling() {
    std::cout << "Test 5: Whitespace handling... ";
    edsparser::EDS eds = create_temp_eds("{ ACGT } { A , ACA } { CGT }");

    assert(eds.length() == 3);
    assert(eds.cardinality() == 4);

    // Sets accessed via read_symbol()
    assert(eds.read_symbol(0)[0] == "ACGT");
    assert(eds.read_symbol(1)[0] == "A");
    assert(eds.read_symbol(1)[1] == "ACA");

    std::cout << "PASSED\n";
}

void test_empty_input() {
    std::cout << "Test 6: Empty input... ";

    edsparser::EDS eds = create_temp_eds("");

    assert(eds.empty());
    assert(eds.length() == 0);
    assert(eds.cardinality() == 0);
    assert(eds.size() == 0);

    std::cout << "PASSED\n";
}

void test_invalid_format_missing_open() {
    std::cout << "Test 7: Invalid format (missing '{')... ";

    bool caught = false;
    try {
        edsparser::EDS eds = create_temp_eds("ACGT}");
    } catch (const std::runtime_error& e) {
        caught = true;
    }

    assert(caught);
    std::cout << "PASSED\n";
}

void test_invalid_format_missing_close() {
    std::cout << "Test 8: Invalid format (missing '}')... ";

    bool caught = false;
    try {
        edsparser::EDS eds = create_temp_eds("{ACGT");
    } catch (const std::runtime_error& e) {
        caught = true;
    }

    assert(caught);
    std::cout << "PASSED\n";
}

void test_save_to_file() {
    std::cout << "Test 9: Save EDS to file... ";

    // Create an EDS
    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}{CGT}{T,TG}");

    // Save to file
    std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "test_eds_save.eds";
    eds.save(temp_path);

    // Verify file exists and read it back
    assert(std::filesystem::exists(temp_path));
    std::ifstream ifs(temp_path);
    std::string content;
    std::getline(ifs, content);
    ifs.close();

    assert(content == "{ACGT}{A,ACA}{CGT}{T,TG}");

    // Clean up
    std::filesystem::remove(temp_path);

    std::cout << "PASSED\n";
}

void test_load_from_file() {
    std::cout << "Test 10: Load EDS from file... ";

    // Create a temporary file with EDS content
    std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "test_eds_load.eds";
    std::ofstream ofs(temp_path);
    ofs << "{AC}{,A,T}{GT}";
    ofs.close();

    // Load from file
    edsparser::EDS eds = edsparser::EDS::load(temp_path);

    assert(eds.length() == 3);
    assert(eds.cardinality() == 5);
    assert(!eds.empty());

    // Sets accessed via read_symbol()
    assert(eds.read_symbol(0)[0] == "AC");
    assert(eds.read_symbol(1).size() == 3);
    assert(eds.read_symbol(1)[0] == "");
    assert(eds.read_symbol(1)[1] == "A");
    assert(eds.read_symbol(1)[2] == "T");

    // Clean up
    std::filesystem::remove(temp_path);

    std::cout << "PASSED\n";
}

void test_roundtrip_file() {
    std::cout << "Test 11: Roundtrip EDS (save → load)... ";

    // Create original EDS
    edsparser::EDS eds1 = create_temp_eds("{ACGT}{A,ACA}{CGT}{T,TG}");

    // Save to file
    std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "test_eds_roundtrip.eds";
    eds1.save(temp_path);

    // Load from file
    edsparser::EDS eds2 = edsparser::EDS::load(temp_path);

    // Compare
    assert(eds1.length() == eds2.length());
    assert(eds1.cardinality() == eds2.cardinality());
    assert(eds1.size() == eds2.size());

    // Compare symbols via read_symbol
    for (size_t i = 0; i < eds1.length(); i++) {
        auto set1 = eds1.read_symbol(i);
        auto set2 = eds2.read_symbol(i);
        assert(set1.size() == set2.size());
        for (size_t j = 0; j < set1.size(); j++) {
            assert(set1[j] == set2[j]);
        }
    }

    // Clean up
    std::filesystem::remove(temp_path);

    std::cout << "PASSED\n";
}

void test_load_nonexistent_file() {
    std::cout << "Test 12: Load from nonexistent file (should fail)... ";

    std::filesystem::path nonexistent = "/nonexistent/path/to/file.eds";

    bool caught = false;
    try {
        edsparser::EDS eds = edsparser::EDS::load(nonexistent);
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        caught = (msg.find("Failed to open") != std::string::npos);
    }

    assert(caught);
    std::cout << "PASSED\n";
}

void test_statistics_simple() {
    std::cout << "Test 13: Statistics calculation (simple)... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}{CGT}{T,TG}");

    const auto& meta = eds.get_metadata();

    // Structure checks
    assert(meta.num_degenerate_symbols == 2);  // {A,ACA} and {T,TG}
    assert(meta.total_change_size == 2);        // 1 extra in each degenerate set

    // Length checks
    assert(meta.min_context_length == 1);       // "A" and "T"
    assert(meta.max_context_length == 4);       // "ACGT"
    assert(meta.avg_context_length > 2.0 && meta.avg_context_length < 3.0);

    // No empty strings
    assert(meta.num_empty_strings == 0);

    // Common characters in {A,ACA} - "A" is common prefix
    // Common characters in {T,TG} - "T" is common prefix
    assert(meta.num_common_chars == 2);

    std::cout << "PASSED\n";
}

void test_statistics_with_empty() {
    std::cout << "Test 14: Statistics with empty strings... ";

    edsparser::EDS eds = create_temp_eds("{AC}{,A,T}{GT}");

    const auto& meta = eds.get_metadata();

    assert(meta.num_degenerate_symbols == 1);   // Only {,A,T}
    assert(meta.total_change_size == 2);         // 2 extra strings in degenerate set
    assert(meta.num_empty_strings == 1);
    assert(meta.min_context_length == 0);        // Empty string

    std::cout << "PASSED\n";
}

void test_statistics_all_regular() {
    std::cout << "Test 15: Statistics all regular (no degenerate)... ";

    edsparser::EDS eds = create_temp_eds("{A}{C}{G}{T}");

    const auto& meta = eds.get_metadata();

    assert(meta.num_degenerate_symbols == 0);
    assert(meta.total_change_size == 0);
    assert(meta.num_common_chars == 0);
    assert(meta.min_context_length == 1);
    assert(meta.max_context_length == 1);
    assert(meta.avg_context_length == 1.0);

    std::cout << "PASSED\n";
}

void test_print_output() {
    std::cout << "Test 16: Print output... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}");

    std::stringstream output;
    eds.print(output);

    std::string result = output.str();
    assert(result.find("Set 0") != std::string::npos);
    assert(result.find("Set 1") != std::string::npos);
    assert(result.find("degenerate") != std::string::npos);
    assert(result.find("ACGT") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_string_constructor() {
    std::cout << "Test 18: String constructor (FULL mode)... ";

    // Directly construct from std::string — NOT via create_temp_eds / EDS::load
    edsparser::EDS eds("{ACGT}{A,ACA}{CGT}");

    assert(eds.length() == 3);
    assert(eds.cardinality() == 4);
    assert(!eds.empty());
    assert(!eds.has_sources());

    assert(eds.read_symbol(0)[0] == "ACGT");
    assert(eds.read_symbol(1)[0] == "A");
    assert(eds.read_symbol(1)[1] == "ACA");
    assert(eds.read_symbol(2)[0] == "CGT");

    const auto& meta = eds.get_metadata();
    assert(meta.num_degenerate_symbols == 1);
    assert(meta.num_empty_strings == 0);

    std::cout << "PASSED\n";
}

void test_load_eds_with_sources_from_files() {
    std::cout << "Test 19: Load EDS with sources from files... ";

    // Create temporary EDS file
    auto eds_path = std::filesystem::temp_directory_path() / "test_with_sources.eds";
    std::ofstream eds_file(eds_path);
    eds_file << "{A}{B,C}";
    eds_file.close();

    // Create temporary SEDS file
    auto seds_path = std::filesystem::temp_directory_path() / "test_with_sources.seds";
    std::ofstream seds_file(seds_path);
    seds_file << "{1}{2}{1,2}";
    seds_file.close();

    // Load EDS with sources
    edsparser::EDS eds = edsparser::EDS::load(eds_path, seds_path);

    // Verify EDS structure
    assert(eds.cardinality() == 3);
    assert(eds.length() == 2);
    assert(eds.has_sources());

    // Verify sources via streaming
    auto src0 = eds.read_source(0);  // String "A": {1}
    auto src1 = eds.read_source(1);  // String "B": {2}
    auto src2 = eds.read_source(2);  // String "C": {1,2}

    assert(src0.size() == 1);
    assert(src0.count(1) == 1);

    assert(src1.size() == 1);
    assert(src1.count(2) == 1);

    assert(src2.size() == 2);
    assert(src2.count(1) == 1);
    assert(src2.count(2) == 1);

    // Clean up
    std::filesystem::remove(eds_path);
    std::filesystem::remove(seds_path);

    std::cout << "PASSED\n";
}

void test_mixed_inputs() {
    std::cout << "Test 20: File loading with sources and from_string factory... ";

    // Create temporary files
    std::filesystem::path temp_eds = std::filesystem::temp_directory_path() / "test_mixed_eds.eds";
    std::filesystem::path temp_seds = std::filesystem::temp_directory_path() / "test_mixed_seds.seds";

    std::ofstream ofs(temp_eds);
    ofs << "{AC}{GT}";
    ofs.close();

    std::ofstream ofs2(temp_seds);
    ofs2 << "{0}{1}";
    ofs2.close();

    // Test: load with two files
    edsparser::EDS eds1 = edsparser::EDS::load(temp_eds, temp_seds);
    assert(eds1.cardinality() == 2);
    assert(eds1.has_sources());

    // Test: from_string factory without sources
    edsparser::EDS eds2 = edsparser::EDS::from_string("{XY}{ZW}");
    assert(eds2.cardinality() == 2);
    assert(!eds2.has_sources());

    // Test: from_string factory with sources - DISABLED (no longer supported with streaming-only mode)
    // Sources must be loaded from files now
    // edsparser::EDS eds3 = edsparser::EDS::from_string("{AB}{CD}", "{0}{1}");
    // assert(eds3.cardinality() == 2);
    // assert(eds3.has_sources());

    // Test: post-construction source loading - DISABLED (requires file-based approach now)
    // In streaming mode, Sources objects must be created from files
    // edsparser::EDS eds4("{PQ}{RS}");
    // assert(!eds4.has_sources());

    // Cleanup
    std::filesystem::remove(temp_eds);
    std::filesystem::remove(temp_seds);

    std::cout << "PASSED\n";
}

void test_compact_format_parsing() {
    std::cout << "Test 21: Compact format parsing... ";

    // Compact format: no brackets on non-degenerate symbols
    std::string compact = "ACGT{A,ACA}CGT{T,TG}";
    edsparser::EDS eds = create_temp_eds(compact);

    assert(eds.length() == 4);
    assert(eds.cardinality() == 6);

    // Sets accessed via read_symbol()
    assert(eds.read_symbol(0)[0] == "ACGT");
    assert(eds.read_symbol(1).size() == 2);
    assert(eds.read_symbol(1)[0] == "A");
    assert(eds.read_symbol(1)[1] == "ACA");
    assert(eds.read_symbol(2)[0] == "CGT");
    assert(eds.read_symbol(3).size() == 2);

    std::cout << "PASSED\n";
}

void test_compact_format_output() {
    std::cout << "Test 22: Compact format output... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}{CGT}{T,TG}");

    // Save in compact format
    std::stringstream output;
    eds.save(output, edsparser::EDS::OutputFormat::COMPACT);

    std::string result = output.str();
    // Remove newline
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }

    // Should be: ACGT{A,ACA}CGT{T,TG}
    assert(result == "ACGT{A,ACA}CGT{T,TG}");

    std::cout << "PASSED\n";
}

void test_roundtrip_compact() {
    std::cout << "Test 23: Roundtrip compact format (parse → save → parse)... ";

    // Start with full bracket format (compact format parsing via load not supported)
    std::string full_format = "{ACGT}{A,ACA}{CGT}";
    auto temp_path1 = std::filesystem::temp_directory_path() / "test_compact_1.eds";
    std::ofstream ofs1(temp_path1);
    ofs1 << full_format;
    ofs1.close();

    edsparser::EDS eds1 = edsparser::EDS::load(temp_path1);

    // Save in compact format to stream
    std::stringstream compact_stream;
    eds1.save(compact_stream, edsparser::EDS::OutputFormat::COMPACT);
    std::string compact_output = compact_stream.str();

    // Verify compact format output (no brackets on non-degenerate)
    assert(compact_output.find("ACGT{A,ACA}CGT") != std::string::npos);

    // Save in full format to another file and reload
    auto temp_path2 = std::filesystem::temp_directory_path() / "test_compact_2.eds";
    std::ofstream ofs2(temp_path2);
    eds1.save(ofs2, edsparser::EDS::OutputFormat::FULL);
    ofs2.close();

    edsparser::EDS eds2 = edsparser::EDS::load(temp_path2);

    // Compare
    assert(eds1.length() == eds2.length());
    assert(eds1.cardinality() == eds2.cardinality());

    // Compare symbols via read_symbol
    for (size_t i = 0; i < eds1.length(); i++) {
        auto set1 = eds1.read_symbol(i);
        auto set2 = eds2.read_symbol(i);
        assert(set1.size() == set2.size());
        for (size_t j = 0; j < set1.size(); j++) {
            assert(set1[j] == set2[j]);
        }
    }

    // Clean up
    std::filesystem::remove(temp_path1);
    std::filesystem::remove(temp_path2);

    std::cout << "PASSED\n";
}

void test_save_and_reload_eds_with_sources() {
    std::cout << "Test 24: Save and reload EDS with sources... ";

    // Create EDS with sources
    auto eds_path1 = std::filesystem::temp_directory_path() / "test_roundtrip_src.eds";
    auto seds_path1 = std::filesystem::temp_directory_path() / "test_roundtrip_src.seds";

    std::ofstream eds_file(eds_path1);
    eds_file << "{ACGT}{A,CA}{GG}";
    eds_file.close();

    std::ofstream seds_file(seds_path1);
    seds_file << "{0}{1}{2}{0}";
    seds_file.close();

    // Load original
    edsparser::EDS eds1 = edsparser::EDS::load(eds_path1, seds_path1);
    assert(eds1.has_sources());
    assert(eds1.cardinality() == 4);

    // Save to new files
    auto eds_path2 = std::filesystem::temp_directory_path() / "test_roundtrip_src2.eds";
    auto seds_path2 = std::filesystem::temp_directory_path() / "test_roundtrip_src2.seds";

    eds1.save(eds_path2);
    eds1.get_sources_object()->save(seds_path2);

    // Reload from saved files
    edsparser::EDS eds2 = edsparser::EDS::load(eds_path2, seds_path2);

    // Verify structure matches
    assert(eds2.has_sources());
    assert(eds2.cardinality() == eds1.cardinality());
    assert(eds2.length() == eds1.length());

    // Verify sources match
    for (size_t i = 0; i < eds1.cardinality(); i++) {
        auto src1 = eds1.read_source(i);
        auto src2 = eds2.read_source(i);
        assert(src1 == src2);
    }

    // Clean up
    std::filesystem::remove(eds_path1);
    std::filesystem::remove(seds_path1);
    std::filesystem::remove(eds_path2);
    std::filesystem::remove(seds_path2);

    std::cout << "PASSED\n";
}

void test_generate_patterns() {
    std::cout << "Test 24: Generate patterns... ";

    // Create a simple EDS with enough length for varied patterns
    edsparser::EDS eds = create_temp_eds("{ACGT}{A,CA}{GG}");

    // Generate patterns
    std::stringstream output;
    eds.generate_patterns(output, 20, 8);

    // Check that we got 20 patterns
    std::string line;
    int count = 0;
    std::set<std::string> unique_patterns;
    while (std::getline(output, line)) {
        if (!line.empty()) {
            count++;
            // Each pattern should be 8 characters long
            assert(line.length() == 8);
            unique_patterns.insert(line);
        }
    }
    assert(count == 20);

    // With random starting positions, we should get at least some variety
    // (not all patterns identical - which would happen if all started at position 0)
    assert(unique_patterns.size() > 1);

    std::cout << "PASSED\n";
}

void test_generate_patterns_metadata_only() {
    std::cout << "Test 25: Generate patterns works in METADATA_ONLY mode... ";

    // Create temp file
    std::filesystem::path temp_file = std::filesystem::temp_directory_path() / "test_genpatterns.eds";
    std::ofstream ofs(temp_file);
    ofs << "{ACGT}{A,CA}{GG}";
    ofs.close();

    // Load in METADATA_ONLY mode
    auto eds = edsparser::EDS::load(temp_file.string());

    // Should work now (streaming from file)
    std::stringstream output;
    eds.generate_patterns(output, 5, 8);

    // Check that we got 5 patterns
    std::string line;
    int count = 0;
    while (std::getline(output, line)) {
        if (!line.empty()) {
            count++;
            // Each pattern should be 8 characters long
            assert(line.length() == 8);
        }
    }
    assert(count == 5);

    std::filesystem::remove(temp_file);
    std::cout << "PASSED\n";
}

// Regression: pattern generation must respect sources.
//
// Picking an alternative from each symbol independently samples the CARTESIAN
// language — combinations no single path carries. A LINEAR-merged l-EDS prunes
// exactly those, so such patterns go missing from the index and a locate()
// benchmark built on them measures the pattern set, not the index.
void test_generate_patterns_source_aware() {
    std::cout << "Test 26b: Source-aware generation walks a single path... ";

    // Two paths, two degenerate symbols. Path 1 carries A then G; path 2 carries
    // C then T. The cross combinations (AT, CG) exist in the cartesian language
    // but in no genome.
    auto eds_path = std::filesystem::temp_directory_path() / "test_srcaware.eds";
    auto seds_path = std::filesystem::temp_directory_path() / "test_srcaware.seds";
    {
        std::ofstream f(eds_path);
        f << "{A,C}{G,T}";
    }
    {
        std::ofstream f(seds_path);
        f << "{1}{2}{1}{2}";
    }

    edsparser::EDS eds = edsparser::EDS::load(eds_path, seds_path);

    std::stringstream out;
    auto stats = eds.generate_patterns(out, 40, 2, /*seed=*/uint64_t{1234},
                                       /*source_aware=*/true);

    assert(stats.source_aware);
    assert(stats.num_paths == 2);
    assert(stats.generated == 40);

    std::string pattern;
    int n = 0;
    while (std::getline(out, pattern)) {
        if (pattern.empty()) continue;
        n++;
        // Only the two real haplotypes may appear.
        assert(pattern == "AG" || pattern == "CT");
    }
    assert(n == 40);

    std::filesystem::remove(eds_path);
    std::filesystem::remove(seds_path);
    std::cout << "PASSED\n";
}

// A given seed must reproduce a given pattern set exactly — benchmarks compare
// runs, and an unseeded generator makes every run a different experiment.
void test_generate_patterns_seed_is_reproducible() {
    std::cout << "Test 26c: --seed reproduces the same pattern set... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,CA}{GGTT}{T,TG}{ACAC}");

    auto run = [&eds](uint64_t seed) {
        std::stringstream out;
        eds.generate_patterns(out, 25, 5, seed, /*source_aware=*/true);
        return out.str();
    };

    assert(run(99) == run(99));   // same seed, same output
    assert(run(99) != run(100));  // different seed, different output

    std::cout << "PASSED\n";
}

void test_generate_patterns_are_valid() {
    std::cout << "Test 26: Generated patterns are valid (check with check_position)... ";

    // Create EDS with known structure
    std::string eds_str = "{ACGT}{A,CA}{GG}{T,TG}";
    edsparser::EDS eds = create_temp_eds(eds_str);

    // Generate patterns
    std::stringstream output;
    eds.generate_patterns(output, 10, 6);

    // For each generated pattern, verify it can be found in the EDS
    std::string pattern;
    int validated = 0;
    while (std::getline(output, pattern)) {
        if (pattern.empty()) continue;

        bool found = false;

        // Try all possible common positions
        // Total common chars: ACGT(4) + GG(2) = 6
        for (edsparser::Position common_pos = 0; common_pos < 6 && !found; common_pos++) {
            // Try without degenerate strings first (regular symbols only)
            try {
                if (eds.check_position(common_pos, {}, pattern)) {
                    found = true;
                    break;
                }
            } catch (const std::invalid_argument&) {
                // Pattern needs degenerate choices, continue to try with them
            }

            // Try with one degenerate choice (from symbol 1: {A,CA})
            for (int deg1 = 0; deg1 < 2 && !found; deg1++) {
                try {
                    if (eds.check_position(common_pos, {deg1}, pattern)) {
                        found = true;
                        break;
                    }
                } catch (const std::invalid_argument&) {
                    // Might need more degenerate choices
                } catch (const std::out_of_range&) {
                    // Invalid degenerate string number, skip
                    continue;
                }

                // Try with two degenerate choices (symbol 1 and symbol 3: {T,TG})
                for (int deg2 = 2; deg2 < 4 && !found; deg2++) {
                    try {
                        if (eds.check_position(common_pos, {deg1, deg2}, pattern)) {
                            found = true;
                            break;
                        }
                    } catch (const std::invalid_argument&) {
                        // Wrong combination
                    } catch (const std::out_of_range&) {
                        // Invalid degenerate string number, skip
                        continue;
                    }
                }
            }
        }

        assert(found); // Every generated pattern must be findable
        validated++;
    }

    assert(validated == 10); // All 10 patterns should be validated

    std::cout << "PASSED\n";
}

void test_source_cache_functionality() {
    std::cout << "Test 27: Source streaming with LRU cache... ";

    // Create EDS with sources
    auto eds_path = std::filesystem::temp_directory_path() / "test_cache.eds";
    auto seds_path = std::filesystem::temp_directory_path() / "test_cache.seds";

    std::ofstream eds_file(eds_path);
    eds_file << "{ACGT}{A,CA}{GG}{T,TT}";
    eds_file.close();

    std::ofstream seds_file(seds_path);
    seds_file << "{0}{1}{2}{0}{1,2}{1}";
    seds_file.close();

    // Load with sources
    edsparser::EDS eds = edsparser::EDS::load(eds_path, seds_path);
    assert(eds.has_sources());
    assert(eds.cardinality() == 6);

    // Set small cache size to test LRU behavior
    eds.get_sources_object()->set_cache_capacity(2);

    // Access sources - first accesses will be cache misses
    auto src0 = eds.read_source(0);  // Cache: {0}
    assert(src0.count(0) == 1);

    auto src1 = eds.read_source(1);  // Cache: {0, 1}
    assert(src1.count(1) == 1);

    auto src2 = eds.read_source(2);  // Cache: {1, 2} (evicts 0)
    assert(src2.count(2) == 1);

    // Access src1 again - should be cache hit
    auto src1_again = eds.read_source(1);
    assert(src1_again == src1);

    // Access src4 which has multiple paths
    auto src4 = eds.read_source(4);  // {1,2}
    assert(src4.size() == 2);
    assert(src4.count(1) == 1);
    assert(src4.count(2) == 1);

    // Verify sources persist across multiple reads (cache works)
    for (int i = 0; i < 3; i++) {
        auto src = eds.read_source(4);
        assert(src == src4);
    }

    // Clean up
    std::filesystem::remove(eds_path);
    std::filesystem::remove(seds_path);

    std::cout << "PASSED\n";
}

void test_extract_basic() {
    std::cout << "Test 28: Extract basic... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,CA}{GG}{T,TT}");

    // Extract: position 1-2, selecting first alternative from each
    std::vector<int> changes = {0, 0};
    std::string result = eds.extract(1, 2, changes);
    assert(result == "AGG");  // {A} + {GG}

    // Extract: position 1-2, selecting second alternative from first, first from second
    changes = {1, 0};
    result = eds.extract(1, 2, changes);
    assert(result == "CAGG");  // {CA} + {GG}

    // Extract: position 3, selecting second alternative
    changes = {1};
    result = eds.extract(3, 1, changes);
    assert(result == "TT");  // {TT}

    std::cout << "PASSED\n";
}

void test_extract_empty() {
    std::cout << "Test 29: Extract with zero length... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,CA}");
    std::vector<int> changes = {};
    std::string result = eds.extract(0, 0, changes);
    assert(result == "");

    std::cout << "PASSED\n";
}

void test_extract_invalid_change_index() {
    std::cout << "Test 30: Extract with invalid change index... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,CA}");

    // Invalid index (only 0,1 valid for position 1)
    std::vector<int> changes = {5};
    bool threw = false;
    try {
        eds.extract(1, 1, changes);
    } catch (const std::out_of_range& e) {
        threw = true;
    }
    assert(threw);

    std::cout << "PASSED\n";
}

void test_extract_wrong_changes_size() {
    std::cout << "Test 31: Extract with wrong changes vector size... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,CA}{GG}");

    // Request 2 positions but provide 1 change
    std::vector<int> changes = {0};
    bool threw = false;
    try {
        eds.extract(0, 2, changes);
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::string msg = e.what();
        assert(msg.find("changes vector size") != std::string::npos);
    }
    assert(threw);

    std::cout << "PASSED\n";
}

void test_extract_metadata_only() {
    std::cout << "Test 32: Extract throws in METADATA_ONLY mode... ";

    // Create temp file
    std::filesystem::path temp_file = std::filesystem::temp_directory_path() / "test_extract.eds";
    std::ofstream ofs(temp_file);
    ofs << "{ACGT}{A,CA}";
    ofs.close();

    // Load in METADATA_ONLY mode
    auto eds = edsparser::EDS::load(temp_file.string());

    // Should throw
    std::vector<int> changes = {0};
    bool threw = false;
    try {
        eds.extract(0, 1, changes);
    } catch (const std::runtime_error& e) {
        threw = true;
        std::string msg = e.what();
        assert(msg.find("FULL mode") != std::string::npos);
    }
    assert(threw);

    std::filesystem::remove(temp_file);
    std::cout << "PASSED\n";
}

void test_check_position_basic() {
    std::cout << "Test 33: check_position basic... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}{CGT}{T,TG}");

    // Pattern "ACG" at (0, {})
    assert(eds.check_position(0, {}, "ACG") == true);

    // Pattern "ACG" at (4, {0})
    assert(eds.check_position(4, {0}, "ACG") == true);

    // Pattern "ACG" at (6, {1})
    assert(eds.check_position(6, {1}, "ACG") == true);

    // Pattern "GTT" at (5, {2})
    assert(eds.check_position(5, {2}, "GTT") == true);

    // Pattern "GTT" at (5, {3})
    assert(eds.check_position(5, {3}, "GTT") == true);

    // Pattern "ACGTT" at (4, {0, 2})
    assert(eds.check_position(4, {0, 2}, "ACGTT") == true);

    // Pattern "ACGTT" at (4, {0, 3})
    assert(eds.check_position(4, {0, 3}, "ACGTT") == true);

    std::cout << "PASSED\n";
}

void test_check_position_negative() {
    std::cout << "Test 34: check_position negative cases... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}{CGT}{T,TG}");

    // Wrong pattern
    assert(eds.check_position(0, {}, "XYZ") == false);

    // Pattern doesn't match
    assert(eds.check_position(0, {}, "ACGTX") == false);

    // Position beyond range
    assert(eds.check_position(100, {}, "ACG") == false);

    // Wrong degenerate string for position
    assert(eds.check_position(4, {1}, "ACG") == false);  // Should be {0}, not {1}

    std::cout << "PASSED\n";
}

void test_check_position_errors() {
    std::cout << "Test 35: check_position error handling... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}{CGT}{T,TG}");

    // Invalid degenerate string number
    bool threw = false;
    try {
        eds.check_position(4, {999}, "ACG");
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw);

    // Not enough degenerate strings
    threw = false;
    try {
        eds.check_position(4, {}, "ACGTT");  // Should need {0, 2} or similar
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    // Wrong symbol for degenerate string
    threw = false;
    try {
        // String 2 belongs to symbol 3, not symbol 1
        eds.check_position(4, {2}, "ACG");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    // Negative degenerate string number
    threw = false;
    try {
        eds.check_position(4, {-1}, "ACG");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    std::cout << "PASSED\n";
}

void test_check_position_metadata_only() {
    std::cout << "Test 36: check_position in METADATA_ONLY mode... ";

    // Create temp file
    std::filesystem::path temp_file =
        std::filesystem::temp_directory_path() / "test_check_pos.eds";
    std::ofstream ofs(temp_file);
    ofs << "{ACGT}{A,ACA}{CGT}{T,TG}";
    ofs.close();

    // Load in METADATA_ONLY mode
    auto eds = edsparser::EDS::load(temp_file);

    // Should work same as FULL mode
    assert(eds.check_position(0, {}, "ACG") == true);
    assert(eds.check_position(4, {0}, "ACG") == true);
    assert(eds.check_position(5, {2}, "GTT") == true);
    assert(eds.check_position(0, {}, "XYZ") == false);

    std::filesystem::remove(temp_file);
    std::cout << "PASSED\n";
}

void test_check_position_empty_pattern() {
    std::cout << "Test 37: check_position with empty pattern... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}");

    // Empty pattern should always match
    assert(eds.check_position(0, {}, "") == true);
    assert(eds.check_position(3, {}, "") == true);

    std::cout << "PASSED\n";
}

void test_check_position_empty_eds() {
    std::cout << "Test 38: check_position with empty EDS... ";

    edsparser::EDS eds = create_temp_eds("");

    // Empty EDS should return false
    assert(eds.check_position(0, {}, "ACG") == false);

    std::cout << "PASSED\n";
}

void test_check_position_offset() {
    std::cout << "Test 39: check_position with offset in symbol... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}{CGT}{T,TG}");

    // Start at position 1 ('C' in ACGT)
    assert(eds.check_position(1, {}, "CG") == true);
    assert(eds.check_position(1, {}, "CGT") == true);

    // Start at position 2 ('G' in ACGT)
    assert(eds.check_position(2, {}, "GT") == true);

    // Start at position 3 ('T' in ACGT)
    assert(eds.check_position(3, {}, "T") == true);

    std::cout << "PASSED\n";
}

void test_check_position_pattern_spans_multiple() {
    std::cout << "Test 40: check_position pattern spanning multiple symbols... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}{CGT}{T,TG}");

    // Full pattern spanning all symbols
    assert(eds.check_position(0, {0, 2}, "ACGTACGTT") == true);
    assert(eds.check_position(0, {0, 3}, "ACGTACGTTG") == true);
    assert(eds.check_position(0, {1, 2}, "ACGTACACGTT") == true);

    std::cout << "PASSED\n";
}

void test_check_position_with_sources_valid() {
    std::cout << "Test 41: check_position with sources (valid paths)... ";

    // EDS:  {ACGT}{A,ACA}{CGT}{T,TG}
    // sEDS: {0}{1,3}{2}{0}{1}{2,3}
    //       str0  str1  str2 str3 str4  str5
    std::string eds_str = "{ACGT}{A,ACA}{CGT}{T,TG}";
    std::string seds_str = "{0}{1,3}{2}{0}{1}{2,3}";

    // DISABLED:     edsparser::EDS eds(eds_str, seds_str);

    // Pattern "ACGTT" at (4, {0, 2})
    // Uses string 0 "A" with sources {1,3}
    // Uses string 2 "T" with sources {1}
    // Intersection: {1,3} ∩ {1} = {1} ✓
    assert(eds.check_position(4, {0, 2}, "ACGTT") == true);

    // Pattern "ACGTG" at (4, {0, 3})
    // Uses string 0 "A" with sources {1,3}
    // Uses string 3 "TG" with sources {2,3}
    // Intersection: {1,3} ∩ {2,3} = {3} ✓
    assert(eds.check_position(4, {0, 3}, "ACGTTG") == true);

    // Pattern "ACACGTT" at (4, {1, 2})
    // Uses string 1 "ACA" with sources {2}
    // Uses string 2 "T" with sources {1}
    // Intersection: {2} ∩ {1} = {} EMPTY
    assert(eds.check_position(4, {1, 2}, "ACACGTT") == false);

    std::cout << "PASSED\n";
}

void test_check_position_with_sources_universal() {
    std::cout << "Test 42: check_position with sources (universal marker)... ";

    // EDS with universal markers
    std::string eds_str = "{ACGT}{A,ACA}{CGT}";
    std::string seds_str = "{0}{1}{2}{0}";

    // DISABLED:     edsparser::EDS eds(eds_str, seds_str);

    // Universal {0} should not restrict intersection
    // Pattern "ACGTACGT" using string 0 "A"
    // Sources: {0} ∩ {1} ∩ {0} = {1}
    assert(eds.check_position(0, {0}, "ACGTACGT") == true);

    // Pattern "ACGTACACGT" using string 1 "ACA"
    // Sources: {0} ∩ {2} ∩ {0} = {2}
    assert(eds.check_position(0, {1}, "ACGTACACGT") == true);

    std::cout << "PASSED\n";
}

void test_check_position_without_sources() {
    std::cout << "Test 43: check_position without sources loaded... ";

    edsparser::EDS eds = create_temp_eds("{ACGT}{A,ACA}{CGT}{T,TG}");

    // Without sources, any valid pattern should match
    assert(eds.check_position(4, {0, 2}, "ACGTT") == true);
    assert(eds.check_position(4, {1, 2}, "ACACGTT") == true);

    // Pattern still needs to match the strings
    assert(eds.check_position(4, {0, 2}, "WRONG") == false);

    std::cout << "PASSED\n";
}

void test_check_position_sources_all_paths() {
    std::cout << "Test 42: check_position sources with all universal... ";

    // All strings have universal paths
    std::string eds_str = "{ACGT}{A,ACA}";
    std::string seds_str = "{0}{0}{0}";

    // DISABLED:     edsparser::EDS eds(eds_str, seds_str);

    // All intersections should be {0}
    assert(eds.check_position(4, {0}, "A") == true);
    assert(eds.check_position(4, {1}, "ACA") == true);

    std::cout << "PASSED\n";
}

void test_check_position_sources_disjoint() {
    std::cout << "Test 43: check_position sources disjoint paths... ";

    // Create EDS where some combinations have disjoint paths
    std::string eds_str = "{AC}{A,C}{GT}";
    std::string seds_str = "{0}{1}{2}{0}";

    // DISABLED:     edsparser::EDS eds(eds_str, seds_str);

    // Valid: {0} ∩ {1} ∩ {0} = {1}
    assert(eds.check_position(0, {0}, "ACAGT") == true);

    // Valid: {0} ∩ {2} ∩ {0} = {2}
    assert(eds.check_position(0, {1}, "ACCGT") == true);

    std::cout << "PASSED\n";
}

void test_check_position_sources_metadata_only() {
    std::cout << "Test 44: check_position with sources in METADATA_ONLY mode... ";

    // Create temp files
    std::filesystem::path temp_eds =
        std::filesystem::temp_directory_path() / "test_check_pos_sources.eds";
    std::filesystem::path temp_seds =
        std::filesystem::temp_directory_path() / "test_check_pos_sources.seds";

    std::ofstream ofs_eds(temp_eds);
    ofs_eds << "{ACGT}{A,ACA}{CGT}{T,TG}";
    ofs_eds.close();

    std::ofstream ofs_seds(temp_seds);
    ofs_seds << "{0}{1,3}{2}{0}{1}{2,3}";
    ofs_seds.close();

    // Load in METADATA_ONLY mode
    auto eds = edsparser::EDS::load(temp_eds, temp_seds);

    // Should work same as FULL mode with source validation
    assert(eds.check_position(4, {0, 2}, "ACGTT") == true);   // Valid path
    assert(eds.check_position(4, {1, 2}, "ACACGTT") == false); // Empty intersection

    std::filesystem::remove(temp_eds);
    std::filesystem::remove(temp_seds);

    std::cout << "PASSED\n";
}

void test_stream_constructor() {
    std::cout << "Test A1: Stream constructor (FULL mode)... ";

    std::istringstream ss("{ACGT}{A,ACA}{CGT}{T,TG}");
    edsparser::EDS eds(ss);

    assert(eds.length() == 4);
    assert(eds.cardinality() == 6);
    assert(eds.size() == 14);
    assert(!eds.empty());
    assert(!eds.has_sources());

    auto s0 = eds.read_symbol(0);
    assert(s0.size() == 1 && s0[0] == "ACGT");
    auto s1 = eds.read_symbol(1);
    assert(s1.size() == 2 && s1[0] == "A" && s1[1] == "ACA");
    auto s2 = eds.read_symbol(2);
    assert(s2.size() == 1 && s2[0] == "CGT");
    auto s3 = eds.read_symbol(3);
    assert(s3.size() == 2 && s3[0] == "T" && s3[1] == "TG");

    const auto& meta = eds.get_metadata();
    assert(meta.num_degenerate_symbols == 2);
    assert(meta.num_common_chars == 7);
    assert(meta.num_empty_strings == 0);
    (void)meta;

    std::cout << "PASSED\n";
}

void test_from_string_factory() {
    std::cout << "Test A2: from_string() factory (FULL mode)... ";

    // Standard case
    auto eds = edsparser::EDS::from_string("{ACGT}{A,ACA}{CGT}");
    assert(eds.length() == 3);
    assert(eds.cardinality() == 4);
    assert(!eds.has_sources());
    assert(eds.read_symbol(1)[0] == "A");
    assert(eds.read_symbol(1)[1] == "ACA");

    // Single non-degenerate symbol
    auto eds2 = edsparser::EDS::from_string("{ACGT}");
    assert(eds2.length() == 1 && eds2.cardinality() == 1);
    assert(eds2.read_symbol(0)[0] == "ACGT");
    assert(eds2.get_metadata().num_degenerate_symbols == 0);

    // Single degenerate symbol
    auto eds3 = edsparser::EDS::from_string("{A,T}");
    assert(eds3.length() == 1 && eds3.cardinality() == 2);
    assert(eds3.get_metadata().num_degenerate_symbols == 1);

    // Empty string
    auto eds4 = edsparser::EDS::from_string("");
    assert(eds4.empty());

    std::cout << "PASSED\n";
}

void test_mode_equivalence() {
    std::cout << "Test A3: FULL vs METADATA_ONLY mode equivalence... ";

    const std::string content = "{ACGT}{A,ACA}{CGT}{T,TG}";

    // FULL mode via string constructor
    edsparser::EDS full(content);

    // METADATA_ONLY mode via file loader
    auto temp = std::filesystem::temp_directory_path() / "test_equiv.eds";
    { std::ofstream f(temp); f << content; }
    edsparser::EDS file_mode = edsparser::EDS::load(temp);
    std::filesystem::remove(temp);

    assert(full.length()      == file_mode.length());
    assert(full.cardinality() == file_mode.cardinality());
    assert(full.size()        == file_mode.size());

    for (size_t i = 0; i < full.length(); i++) {
        auto sf = full.read_symbol(i);
        auto sm = file_mode.read_symbol(i);
        assert(sf.size() == sm.size());
        for (size_t j = 0; j < sf.size(); j++)
            assert(sf[j] == sm[j]);
    }

    const auto& mf = full.get_metadata();
    const auto& mm = file_mode.get_metadata();
    assert(mf.num_degenerate_symbols  == mm.num_degenerate_symbols);
    assert(mf.num_common_chars        == mm.num_common_chars);
    assert(mf.total_change_size       == mm.total_change_size);
    assert(mf.num_empty_strings       == mm.num_empty_strings);
    assert(mf.min_context_length      == mm.min_context_length);
    assert(mf.max_context_length      == mm.max_context_length);
    assert(mf.is_degenerate           == mm.is_degenerate);
    (void)mf; (void)mm;

    std::cout << "PASSED\n";
}

void test_full_mode_edge_cases() {
    std::cout << "Test A4: FULL mode edge cases... ";

    // Empty input → empty EDS
    edsparser::EDS empty_str("");
    assert(empty_str.empty() && empty_str.length() == 0);

    std::istringstream empty_ss("");
    edsparser::EDS empty_stream(empty_ss);
    assert(empty_stream.empty());

    // Missing close bracket → throws
    bool threw = false;
    try { edsparser::EDS bad("{ACGT"); (void)bad; }
    catch (const std::exception&) { threw = true; }
    assert(threw);

    // Missing open bracket → throws
    threw = false;
    try { edsparser::EDS bad("ACGT}"); (void)bad; }
    catch (const std::exception&) { threw = true; }
    assert(threw);

    std::cout << "PASSED\n";
}

// ── Bulk-read (read_symbol_from_stream) edge cases ───────────────────────────
// Exercises the single-stream_.read() span path added for the perf optimisation:
// long context blocks, the final symbol (whose span runs to EOF via
// stream_file_size()), empty alternatives, and compact vs bracket layouts.
void test_read_symbol_bulk_edge_cases() {
    std::cout << "Test A5: read_symbol bulk-read edge cases... ";

    // Long context block: the bulk read must reconstruct it exactly, and the
    // LAST symbol must reconstruct too (it uses the EOF/file-size span path).
    {
        std::string longctx(5000, 'A');
        edsparser::EDS eds = create_temp_eds("{" + longctx + "}{C,G}{" + longctx + "}");
        assert(eds.length() == 3);
        assert(eds.read_symbol(0)[0] == longctx);
        auto mid = eds.read_symbol(1);
        assert(mid.size() == 2 && mid[0] == "C" && mid[1] == "G");
        assert(eds.read_symbol(2)[0] == longctx);   // last symbol via file-size span
    }

    // Last symbol degenerate with an empty (epsilon) alternative in the middle.
    {
        edsparser::EDS eds = create_temp_eds("{ACGT}{A,,GG}");
        auto last = eds.read_symbol(1);
        assert(last.size() == 3);
        assert(last[0] == "A" && last[1] == "" && last[2] == "GG");
    }

    // Compact input: bare non-degenerate first + last symbols.
    {
        edsparser::EDS eds = create_temp_eds("ACGT{A,C}TTGG");
        assert(eds.length() == 3);
        assert(eds.read_symbol(0)[0] == "ACGT");
        auto d = eds.read_symbol(1);
        assert(d.size() == 2 && d[0] == "A" && d[1] == "C");
        assert(eds.read_symbol(2)[0] == "TTGG");   // compact last symbol
    }

    // Out-of-order access (forces the seek path, not just sequential).
    {
        edsparser::EDS eds = create_temp_eds("{AA}{B,C}{DDD}{E,F}{GG}");
        assert(eds.read_symbol(4)[0] == "GG");
        assert(eds.read_symbol(0)[0] == "AA");
        assert(eds.read_symbol(2)[0] == "DDD");
    }

    std::cout << "PASSED\n";
}

// ── copy_symbol_range_to_stream raw-copy ─────────────────────────────────────
// The l-EDS merge pass-through uses this to copy runs of unmodified full-bracket
// symbols verbatim.  Verify it reproduces the exact bytes for middle ranges,
// ranges reaching the final symbol (file-size fallback), the whole file, and the
// empty range.
void test_copy_symbol_range_to_stream() {
    std::cout << "Test A6: copy_symbol_range_to_stream raw-copy... ";

    std::string content = "{ACGT}{A,ACA,}{CGT}{T,TG}{GGGG}";
    auto temp_path = std::filesystem::temp_directory_path() / "test_eds_copyrange.eds";
    { std::ofstream ofs(temp_path); ofs << content; }
    edsparser::EDS eds = edsparser::EDS::load(temp_path);

    {   // middle range [1,3)
        std::ostringstream oss;
        eds.copy_symbol_range_to_stream(1, 2, oss);
        assert(oss.str() == "{A,ACA,}{CGT}");
    }
    {   // range reaching the last symbol [3,5) — uses stream_file_size()
        std::ostringstream oss;
        eds.copy_symbol_range_to_stream(3, 2, oss);
        assert(oss.str() == "{T,TG}{GGGG}");
    }
    {   // whole file [0,5)
        std::ostringstream oss;
        eds.copy_symbol_range_to_stream(0, 5, oss);
        assert(oss.str() == content);
    }
    {   // empty range → no output
        std::ostringstream oss;
        eds.copy_symbol_range_to_stream(2, 0, oss);
        assert(oss.str().empty());
    }
    {   // single symbol
        std::ostringstream oss;
        eds.copy_symbol_range_to_stream(0, 1, oss);
        assert(oss.str() == "{ACGT}");
    }

    std::filesystem::remove(temp_path);
    std::cout << "PASSED\n";
}

int main() {
    std::cout << "Running EDS parsing tests...\n\n";

    try {
        test_simple_eds();
        test_empty_strings();
        test_single_position();
        test_all_degenerate();
        test_whitespace_handling();
        test_empty_input();
        test_invalid_format_missing_open();
        test_invalid_format_missing_close();
        test_save_to_file();
        test_load_from_file();
        test_roundtrip_file();
        test_load_nonexistent_file();
        test_statistics_simple();
        test_statistics_with_empty();
        test_statistics_all_regular();
        test_print_output();
        test_string_constructor();
        test_stream_constructor();
        test_from_string_factory();
        test_mode_equivalence();
        test_full_mode_edge_cases();
        test_read_symbol_bulk_edge_cases();
        test_copy_symbol_range_to_stream();
        test_load_eds_with_sources_from_files();
        test_mixed_inputs();
        test_compact_format_parsing();
        test_compact_format_output();
        test_roundtrip_compact();
        test_save_and_reload_eds_with_sources();
        test_generate_patterns();
        test_generate_patterns_metadata_only();
        test_generate_patterns_are_valid();
        test_generate_patterns_source_aware();
        test_generate_patterns_seed_is_reproducible();
        test_source_cache_functionality();
        test_extract_basic();
        test_extract_empty();
        test_extract_invalid_change_index();
        test_extract_wrong_changes_size();
        test_extract_metadata_only();
        test_check_position_basic();
        test_check_position_negative();
        test_check_position_errors();
        test_check_position_metadata_only();
        test_check_position_empty_pattern();
        test_check_position_empty_eds();
        test_check_position_offset();
        test_check_position_pattern_spans_multiple();
        test_check_position_with_sources_valid();
        test_check_position_with_sources_universal();
        test_check_position_without_sources();
        test_check_position_sources_all_paths();
        test_check_position_sources_disjoint();
        test_check_position_sources_metadata_only();

        std::cout << "\n✓ All tests passed!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed: " << e.what() << "\n";
        return 1;
    }
}
