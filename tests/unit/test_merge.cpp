#include "formats/eds.hpp"
#include "transforms/eds_transforms.hpp"
#include <iostream>
#include <cassert>
#include <sstream>
#include <filesystem>
#include <fstream>

using namespace edsparser;

// Helper function to transform EDS string to l-EDS and load the result
// Returns an EDS object that can be inspected via read_symbol()
EDS transform_to_leds(const std::string& eds_str, Length context_length) {
    std::istringstream input(eds_str);
    std::ostringstream output;

    eds_to_leds_linear(input, output, context_length, nullptr, nullptr, 1, false);

    // Write to temp file and load (needed for read_symbol() support)
    auto temp_dir = std::filesystem::temp_directory_path();
    auto temp_path = temp_dir / ("test_leds_" + std::to_string(std::rand()) + ".tmp");

    {
        std::ofstream temp_file(temp_path);
        temp_file << output.str();
    }

    auto result = EDS::load(temp_path);
    std::filesystem::remove(temp_path);

    return result;
}

// Helper function to transform EDS with sources to l-EDS and load the result
// Returns EDS object with sources attached
EDS transform_to_leds_with_sources(
    const std::string& eds_str,
    const std::string& seds_str,
    Length context_length
) {
    std::istringstream eds_input(eds_str);
    std::istringstream seds_input(seds_str);
    std::ostringstream leds_output;
    std::ostringstream lseds_output;

    eds_to_leds_linear(eds_input, leds_output, context_length,
                       &seds_input, &lseds_output, 1, false);

    // Write to temp files and load (needed for read_symbol() support)
    auto temp_dir = std::filesystem::temp_directory_path();
    auto temp_leds = temp_dir / ("test_leds_" + std::to_string(std::rand()) + ".tmp");
    auto temp_lseds = temp_dir / ("test_lseds_" + std::to_string(std::rand()) + ".tmp");

    {
        std::ofstream leds_file(temp_leds);
        leds_file << leds_output.str();
    }
    {
        std::ofstream lseds_file(temp_lseds);
        lseds_file << lseds_output.str();
    }

    auto result = EDS::load(temp_leds, temp_lseds);
    std::filesystem::remove(temp_leds);
    std::filesystem::remove(temp_lseds);

    return result;
}

// Test counter
int test_num = 0;

void test(const std::string& description) {
    test_num++;
    std::cout << "Test " << test_num << ": " << description << "... ";
}

void pass() {
    std::cout << "PASSED\n";
}

// ===== BASIC MERGE TESTS (via l-EDS transformation) =====

void test_merge_two_degenerate_via_leds() {
    test("Merge via l-EDS: {G,C}{T} with context_length=2");

    // {G,C}{T} - each symbol has length 1, so with context_length=2
    // adjacent symbols should be merged
    EDS merged = transform_to_leds("{G,C}{T}", 2);

    // Verify structure - should have merged to single symbol
    assert(merged.length() == 1);
    assert(merged.cardinality() == 2);

    auto set0 = merged.read_symbol(0);
    assert(set0.size() == 2);
    assert(set0[0] == "GT");
    assert(set0[1] == "CT");

    pass();
}

void test_merge_degenerate_nondegenerate_via_leds() {
    test("Merge via l-EDS: {G,C}{T} context_length=2");

    EDS merged = transform_to_leds("{G,C}{T}", 2);

    assert(merged.length() == 1);
    assert(merged.cardinality() == 2);

    auto set0 = merged.read_symbol(0);
    assert(set0[0] == "GT");
    assert(set0[1] == "CT");

    pass();
}

void test_merge_nondegenerate_degenerate_via_leds() {
    test("Merge via l-EDS: {T}{A,C,G} context_length=2");

    EDS merged = transform_to_leds("{T}{A,C,G}", 2);

    assert(merged.length() == 1);
    assert(merged.cardinality() == 3);

    auto set0 = merged.read_symbol(0);
    assert(set0.size() == 3);
    assert(set0[0] == "TA");
    assert(set0[1] == "TC");
    assert(set0[2] == "TG");

    pass();
}

void test_merge_multiple_via_leds() {
    test("Merge via l-EDS: {G,C}{T}{A,C} context_length=3");

    // All three need to merge to get context length of 3
    EDS merged = transform_to_leds("{G,C}{T}{A,C}", 3);

    assert(merged.length() == 1);
    assert(merged.cardinality() == 4);  // 2 * 1 * 2 = 4

    auto set0 = merged.read_symbol(0);
    assert(set0[0] == "GTA");
    assert(set0[1] == "GTC");
    assert(set0[2] == "CTA");
    assert(set0[3] == "CTC");

    pass();
}

void test_merge_with_empty_strings_via_leds() {
    test("Merge via l-EDS: {,A}{T} context_length=2");

    EDS merged = transform_to_leds("{,A}{T}", 2);

    assert(merged.cardinality() == 2);

    auto set0 = merged.read_symbol(0);
    assert(set0[0] == "T");   // "" + "T" = "T"
    assert(set0[1] == "AT");  // "A" + "T" = "AT"

    pass();
}

void test_no_merge_needed() {
    test("No merge needed when context already sufficient");

    // {ACGT} has length 4, so context_length=2 requires no merging
    EDS transformed = transform_to_leds("{ACGT}{G,C}{TT}", 2);

    // Structure should be unchanged (context lengths: 4, then variable, then 2)
    // The non-degenerate TT has length 2, which meets the requirement
    assert(transformed.length() == 3);

    auto set0 = transformed.read_symbol(0);
    auto set1 = transformed.read_symbol(1);
    auto set2 = transformed.read_symbol(2);

    assert(set0[0] == "ACGT");
    assert(set1.size() == 2);
    assert(set2[0] == "TT");

    pass();
}

void test_partial_merge() {
    test("Partial merge: some symbols merged, some not");

    // {ACGT}{G,C}{T} - ACGT is long enough, but {G,C}{T} needs merging with context=2
    EDS transformed = transform_to_leds("{ACGT}{G,C}{T}", 2);

    // Should have 2 symbols: ACGT and merged {GT,CT}
    assert(transformed.length() == 2);

    auto set0 = transformed.read_symbol(0);
    auto set1 = transformed.read_symbol(1);

    assert(set0[0] == "ACGT");
    assert(set1.size() == 2);
    assert(set1[0] == "GT");
    assert(set1[1] == "CT");

    pass();
}

// ===== TESTS WITH SOURCES (LINEAR MERGE) =====

void test_merge_with_sources_valid_intersections() {
    test("Merge with sources - valid intersections");

    // {G,C}{T} with sources
    // String 0 (G): {1,2}, String 1 (C): {2,3}, String 2 (T): {2}
    // Expected: GT with {1,2}∩{2}={2}, CT with {2,3}∩{2}={2}
    EDS merged = transform_to_leds_with_sources(
        "{G,C}{T}",
        "{1,2}{2,3}{2}",
        2
    );

    assert(merged.cardinality() == 2);
    assert(merged.has_sources());

    auto set0 = merged.read_symbol(0);
    assert(set0[0] == "GT");
    assert(set0[1] == "CT");

    pass();
}

void test_merge_with_sources_filtered() {
    test("Merge with sources - filter empty intersections");

    // {A,B}{C,D} with sources
    // String 0 (A): {1}, String 1 (B): {2}, String 2 (C): {1}, String 3 (D): {3}
    // Valid: AC ({1}∩{1}={1}), BD ({2}∩{3}={}), etc.
    // Only AC should survive
    EDS merged = transform_to_leds_with_sources(
        "{A,B}{C,D}",
        "{1}{2}{1}{3}",
        2
    );

    assert(merged.cardinality() == 1);  // Only AC

    auto set0 = merged.read_symbol(0);
    assert(set0.size() == 1);
    assert(set0[0] == "AC");

    pass();
}

void test_merge_with_universal_marker() {
    test("Merge with universal marker {0}");

    // {A,B}{C} with sources
    // String 0 (A): {0} (all paths), String 1 (B): {2}, String 2 (C): {1}
    // Expected: AC with {0}∩{1}={1}, BC with {2}∩{1}={} (filtered)
    EDS merged = transform_to_leds_with_sources(
        "{A,B}{C}",
        "{0}{2}{1}",
        2
    );

    assert(merged.cardinality() == 1);  // Only AC

    auto set0 = merged.read_symbol(0);
    assert(set0[0] == "AC");

    pass();
}

// ===== CONTEXT LENGTH VALIDATION TESTS =====

void test_context_length_1() {
    test("Context length 1 - no merging needed");

    EDS transformed = transform_to_leds("{A}{B}{C}", 1);

    // With context_length=1, single char symbols are acceptable
    assert(transformed.length() == 3);

    pass();
}

void test_context_length_forces_full_merge() {
    test("Large context length forces full merge");

    // With context_length=6, {AA}{BB}{CC} (each len 2) needs full merge
    EDS transformed = transform_to_leds("{AA}{BB}{CC}", 6);

    assert(transformed.length() == 1);

    auto set0 = transformed.read_symbol(0);
    assert(set0[0] == "AABBCC");

    pass();
}

// ===== STATISTICS TESTS =====

void test_statistics_after_transform() {
    test("Verify statistics after transformation");

    EDS transformed = transform_to_leds("{AC}{G,C}{T}", 2);

    auto stats = transformed.get_statistics();

    // Should have 1 non-degenerate (AC) and 1 degenerate (GT,CT)
    assert(stats.num_degenerate_symbols == 1);
    assert(stats.min_context_length == 2);  // "AC"
    assert(stats.max_context_length == 2);

    pass();
}

void test_metadata_consistency() {
    test("Verify metadata consistency after transform");

    EDS transformed = transform_to_leds("{ACGT}{G,C}{T}", 2);

    // Check dimensions
    assert(transformed.length() == 2);  // ACGT + merged(G,C)(T)
    assert(transformed.cardinality() == 3);  // 1 + 2

    // Check is_degenerate
    const auto& is_deg = transformed.get_is_degenerate();
    assert(is_deg.size() == 2);
    assert(is_deg[0] == false);  // {ACGT} non-degenerate
    assert(is_deg[1] == true);   // {GT,CT} degenerate

    pass();
}

// ===== EDGE CASES =====

void test_single_symbol_input() {
    test("Single symbol input");

    EDS transformed = transform_to_leds("{ACGT}", 2);

    assert(transformed.length() == 1);
    assert(transformed.cardinality() == 1);

    auto set0 = transformed.read_symbol(0);
    assert(set0[0] == "ACGT");

    pass();
}

void test_all_degenerate_input() {
    test("All degenerate input");

    EDS transformed = transform_to_leds("{A,B}{C,D}{E,F}", 3);

    // All should be merged for context length 3
    assert(transformed.length() == 1);
    assert(transformed.cardinality() == 8);  // 2^3

    pass();
}

void test_alternating_degenerate() {
    test("Alternating degenerate/non-degenerate");

    // Non-deg, deg, non-deg with short contexts
    EDS transformed = transform_to_leds("{A}{B,C}{D}", 2);

    // Should merge to meet context length requirements
    assert(transformed.length() <= 2);  // Some merging needed

    pass();
}

void test_empty_string_alternatives() {
    test("Empty string alternatives preserved");

    EDS transformed = transform_to_leds("{,A}{B}", 1);

    // With context_length=1, no merging needed
    assert(transformed.length() == 2);

    auto set0 = transformed.read_symbol(0);
    assert(set0.size() == 2);
    // First is empty string, second is A

    pass();
}

// ===== FULL OUTPUT FORMAT TESTS =====

// Regression for bug where --full flag was silently ignored when input was already
// l-EDS compliant (zero merging iterations needed). The raw temp file copy would
// preserve the input format instead of applying the requested format.
void test_full_output_already_leds_compliant_linear() {
    test("--full flag honoured when input already satisfies l-EDS (linear path)");

    // Input is already l-EDS compliant for context_length=2 (context "TTTTTT" >= 2).
    // compact=false → full output: every symbol must be wrapped in { }.
    std::string eds_str = "{ACGTTTTTT}{A,C}{TTTTTTTACG}";
    std::istringstream input(eds_str);
    std::ostringstream output;
    eds_to_leds_linear(input, output, 2, nullptr, nullptr, 1, /*compact=*/false);

    std::string result = output.str();
    // Every symbol must start with '{'
    assert(!result.empty());
    assert(result[0] == '{');
    // Non-degenerate symbols must also be wrapped — verify first and last
    // (we know position 0 and position 2 are non-degenerate in this input)
    // Count opening braces: must equal number of symbols (3)
    size_t brace_count = 0;
    for (char c : result) if (c == '{') ++brace_count;
    assert(brace_count == 3);

    pass();
}

void test_full_output_already_leds_compliant_cartesian() {
    test("--full flag honoured when input already satisfies l-EDS (cartesian path)");

    std::string eds_str = "{ACGTTTTTT}{A,C}{TTTTTTTACG}";
    std::istringstream input(eds_str);
    std::ostringstream output;
    eds_to_leds_cartesian(input, output, 2, 1, /*compact=*/false);

    std::string result = output.str();
    assert(!result.empty());
    assert(result[0] == '{');
    size_t brace_count = 0;
    for (char c : result) if (c == '{') ++brace_count;
    assert(brace_count == 3);

    pass();
}

void test_full_output_compact_input_roundtrip() {
    test("full and compact output parse to identical EDS");

    // compact input (no brackets on non-degenerate)
    std::string eds_str = "ACGTTTTT{A,C}TTTTTACGT";
    Length ctx = 2;

    auto run = [&](bool compact) -> std::string {
        std::istringstream in(eds_str);
        std::ostringstream out;
        eds_to_leds_linear(in, out, ctx, nullptr, nullptr, 1, compact);
        return out.str();
    };

    std::string compact_out = run(true);
    std::string full_out    = run(false);

    // Full output must start with '{'
    assert(!full_out.empty() && full_out[0] == '{');

    // Both must parse to the same EDS structure
    auto load = [](const std::string& s) {
        auto p = std::filesystem::temp_directory_path()
                 / ("test_roundtrip_" + std::to_string(std::rand()) + ".tmp");
        { std::ofstream f(p); f << s; }
        auto eds = EDS::load(p);
        std::filesystem::remove(p);
        return eds;
    };

    EDS eds_compact = load(compact_out);
    EDS eds_full    = load(full_out);

    assert(eds_compact.length()      == eds_full.length());
    assert(eds_compact.cardinality() == eds_full.cardinality());
    for (size_t i = 0; i < eds_compact.length(); ++i) {
        auto sc = eds_compact.read_symbol(i);
        auto sf = eds_full.read_symbol(i);
        assert(sc.size() == sf.size());
        for (size_t j = 0; j < sc.size(); ++j)
            assert(sc[j] == sf[j]);
    }

    pass();
}

// ===== MAIN =====

int main() {
    std::cout << "Running EDS merge tests via eds_to_leds_linear()...\n\n";

    // Basic merge tests (CARTESIAN - no sources)
    test_merge_two_degenerate_via_leds();
    test_merge_degenerate_nondegenerate_via_leds();
    test_merge_nondegenerate_degenerate_via_leds();
    test_merge_multiple_via_leds();
    test_merge_with_empty_strings_via_leds();
    test_no_merge_needed();
    test_partial_merge();

    // With sources (LINEAR merge)
    test_merge_with_sources_valid_intersections();
    test_merge_with_sources_filtered();
    test_merge_with_universal_marker();

    // Context length validation
    test_context_length_1();
    test_context_length_forces_full_merge();

    // Statistics and metadata
    test_statistics_after_transform();
    test_metadata_consistency();

    // Edge cases
    test_single_symbol_input();
    test_all_degenerate_input();
    test_alternating_degenerate();
    test_empty_string_alternatives();

    // Full output format (--full flag regression)
    test_full_output_already_leds_compliant_linear();
    test_full_output_already_leds_compliant_cartesian();
    test_full_output_compact_input_roundtrip();

    std::cout << "\n===========================================\n";
    std::cout << "All " << test_num << " tests PASSED!\n";
    std::cout << "===========================================\n";

    return 0;
}
