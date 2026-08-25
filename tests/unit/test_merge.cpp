#include "formats/eds.hpp"
#include "transforms/eds_transforms.hpp"
#include <iostream>
#include <cassert>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

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
    test("Merge via l-EDS: {G,C}{T} at l=2 is already an l-EDS (boundary exemption)");

    // The l-EDS constraint is on *internal* common segments: a segment at the
    // start or end of the string has a degenerate neighbour on one side only,
    // so it is never ambiguous. needs_merge() encodes that as `i > 0 && i < n-1`.
    //
    // {G,C}{T} therefore has no constrained context at all — {T} is the last
    // symbol — and the transform correctly converges in 0 iterations, leaving
    // the input untouched. This test used to expect the merge to {GT,CT}.
    EDS merged = transform_to_leds("{G,C}{T}", 2);

    assert(merged.length() == 2);
    assert(merged.cardinality() == 3);

    auto set0 = merged.read_symbol(0);
    assert(set0.size() == 2);
    assert(set0[0] == "G");
    assert(set0[1] == "C");

    auto set1 = merged.read_symbol(1);
    assert(set1.size() == 1);
    assert(set1[0] == "T");

    pass();
}

void test_merge_short_interior_context_via_leds() {
    test("Merge via l-EDS: a short *interior* context does merge");

    // The counterpart to the exemption above: {A,C}{T}{G,A} at l=2 has {T} in
    // the interior, one character against a required two, so the chain
    // collapses into a single symbol. Without a case like this the suite has no
    // coverage that short contexts merge at all.
    EDS merged = transform_to_leds("{A,C}{T}{G,A}", 2);

    assert(merged.length() == 1);

    auto set0 = merged.read_symbol(0);
    assert(set0.size() == 4);   // cartesian: no sources supplied
    std::set<std::string> got(set0.begin(), set0.end());
    assert(got == (std::set<std::string>{"ATG", "ATA", "CTG", "CTA"}));

    pass();
}

void test_merge_degenerate_nondegenerate_via_leds() {
    test("Merge via l-EDS: interior chain merges, boundary contexts survive");

    // This used to be a second copy of the {G,C}{T} case above, so it tested
    // the boundary exemption twice and the merge itself not at all. Give it the
    // case that pins both behaviours at once: the interior {T} is one character
    // against a required two, so it and both degenerate neighbours collapse
    // into one symbol, while the leading and trailing {AAA} — long enough
    // anyway — stay exactly where they are.
    EDS merged = transform_to_leds("{AAA}{A,C}{T}{G,A}{AAA}", 2);

    assert(merged.length() == 3);
    assert(merged.cardinality() == 6);   // 1 + 4 + 1

    auto set0 = merged.read_symbol(0);
    assert(set0.size() == 1 && set0[0] == "AAA");

    auto set1 = merged.read_symbol(1);
    assert(set1.size() == 4);
    std::set<std::string> got(set1.begin(), set1.end());
    assert(got == (std::set<std::string>{"ATG", "ATA", "CTG", "CTA"}));

    auto set2 = merged.read_symbol(2);
    assert(set2.size() == 1 && set2[0] == "AAA");

    pass();
}

void test_merge_nondegenerate_degenerate_via_leds() {
    test("Merge via l-EDS: {T}{A,C,G} at l=2 — the exemption from the other side");

    // Mirror image of test_merge_two_degenerate_via_leds: here the short common
    // symbol is leading rather than trailing. It is exempt for the same reason
    // (a degenerate neighbour on one side only), so nothing merges.
    EDS merged = transform_to_leds("{T}{A,C,G}", 2);

    assert(merged.length() == 2);
    assert(merged.cardinality() == 4);

    auto set0 = merged.read_symbol(0);
    assert(set0.size() == 1 && set0[0] == "T");

    auto set1 = merged.read_symbol(1);
    assert(set1.size() == 3);
    assert(set1[0] == "A");
    assert(set1[1] == "C");
    assert(set1[2] == "G");

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
    test("Merge via l-EDS: empty alternatives concatenate correctly");

    // {,A}{T} at l=2 is two symbols and so entirely exempt (see
    // test_merge_two_degenerate_via_leds), which is why this never merged.
    // Put the short common symbol in the interior so the merge actually runs,
    // and keep what the test is really about: the empty alternative has to
    // concatenate as the empty string rather than being dropped or duplicated.
    EDS merged = transform_to_leds("{,A}{T}{G,A}", 2);

    assert(merged.length() == 1);
    assert(merged.cardinality() == 4);

    auto set0 = merged.read_symbol(0);
    std::set<std::string> got(set0.begin(), set0.end());
    // "" + T + G, "" + T + A, "A" + T + G, "A" + T + A
    assert(got == (std::set<std::string>{"TG", "TA", "ATG", "ATA"}));

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

    // {ACGT}{G,C}{T} at l=2 merges nothing — the trailing {T} is exempt — so it
    // could not demonstrate a partial merge. Here {ACGT} is long enough to be
    // left alone while the short interior {T} pulls both its degenerate
    // neighbours into one symbol.
    EDS transformed = transform_to_leds("{ACGT}{A,C}{T}{G,A}{ACGT}", 2);

    assert(transformed.length() == 3);

    auto set0 = transformed.read_symbol(0);
    auto set1 = transformed.read_symbol(1);
    auto set2 = transformed.read_symbol(2);

    assert(set0.size() == 1 && set0[0] == "ACGT");   // untouched
    assert(set1.size() == 4);                        // merged
    assert(set2.size() == 1 && set2[0] == "ACGT");   // untouched

    pass();
}

// ===== TESTS WITH SOURCES (LINEAR MERGE) =====

void test_merge_with_sources_valid_intersections() {
    test("Merge with sources - valid intersections");

    // The old input {G,C}{T} merged nothing at l=2: the trailing common symbol
    // is boundary-exempt, so the test asserted intersections that were never
    // computed. Two adjacent degenerate symbols merge regardless of position
    // (ADJACENT_DEGENERATE is not subject to the exemption), which is the
    // smallest input that actually exercises the intersection.
    //
    // 4 paths, each taking exactly one combination:
    //   G:{1,2} C:{3,4} | T:{1,3} A:{2,4}
    //   GT={1} GA={2} CT={3} CA={4}
    EDS merged = transform_to_leds_with_sources(
        "{G,C}{T,A}",
        "{1,2}{3,4}{1,3}{2,4}",
        2
    );

    assert(merged.length() == 1);
    assert(merged.cardinality() == 4);
    assert(merged.has_sources());

    auto set0 = merged.read_symbol(0);
    std::set<std::string> got(set0.begin(), set0.end());
    assert(got == (std::set<std::string>{"GT", "GA", "CT", "CA"}));

    // Every combination is carried by exactly one path, so the path-count bound
    // is tight here rather than merely satisfied.
    assert(set0.size() == 4);

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

    // Same correction as above — {A,B}{C} is boundary-exempt and never merged.
    //   A:{0} (universal)  B:{2} | C:{1}  D:{3}
    //   AC = {0}∩{1} = {1}   ✓ universal intersects every path
    //   AD = {0}∩{3} = {3}   ✓
    //   BC = {2}∩{1} = {}    ✗ pruned
    //   BD = {2}∩{3} = {}    ✗ pruned
    EDS merged = transform_to_leds_with_sources(
        "{A,B}{C,D}",
        "{0}{2}{1}{3}",
        2
    );

    assert(merged.length() == 1);
    assert(merged.cardinality() == 2);

    auto set0 = merged.read_symbol(0);
    std::set<std::string> got(set0.begin(), set0.end());
    assert(got == (std::set<std::string>{"AC", "AD"}));

    pass();
}

// ===== PATH-COUNT INVARIANT (complement-source regression, TODO 0b) =====

// A linear merge can never produce more strings in one output symbol than there
// are paths: each path contributes exactly one alternative per input symbol, and
// surviving combinations carry disjoint path sets. Nothing in the suite asserted
// this, which is how the complement-source bug (20d8ff1) survived every unit and
// e2e test while producing 6,444,274 strings in one symbol against a bound of 50.
//
// The setup targets the exact code path that broke: the bitset fast path in
// compute_merge_metadata(), which engages only when every path ID fits in [1,63].
// Six adjacent degenerate symbols would be 2^6 = 64 combinations under a
// cartesian merge; correct source pruning leaves exactly one per path.
//
// Sources use the complement spelling — {0,p} means "every path except p", which
// is how vcf2eds and msa2eds encode a reference allele. Reading {0,p} as
// universal (the bug) makes every intersection non-empty, so nothing prunes.
void test_linear_merge_respects_path_count_bound() {
    test("Linear merge cannot exceed num_paths strings per symbol");

    // 3 paths, 6 adjacent binary symbols. Alternative T belongs to path 1 at
    // symbols 0-1, path 2 at symbols 2-3, path 3 at symbols 4-5; the A allele at
    // each symbol is carried by the other two paths, written as a complement.
    EDS merged = transform_to_leds_with_sources(
        "{A,T}{A,T}{A,T}{A,T}{A,T}{A,T}",
        "{0,1}{1}{0,1}{1}{0,2}{2}{0,2}{2}{0,3}{3}{0,3}{3}",
        2
    );

    // The whole chain collapses to a single symbol. (length() is the symbol
    // count; size() is the total character count — 18 here, three 6-character
    // walks — so asserting size() == 1 could never hold.)
    assert(merged.length() == 1);

    auto set0 = merged.read_symbol(0);

    // The invariant itself. Cartesian would give 64 here.
    const size_t num_paths = 3;
    assert(set0.size() <= num_paths);

    // And the exact expected walk for each path.
    assert(set0.size() == 3);
    std::set<std::string> got(set0.begin(), set0.end());
    assert(got.count("TTAAAA") == 1);  // path 1
    assert(got.count("AATTAA") == 1);  // path 2
    assert(got.count("AAAATT") == 1);  // path 3

    pass();
}

// The same invariant with the universal marker mixed in, so a genuinely
// universal {0} is not "fixed" into behaving like a complement.
void test_universal_marker_still_matches_every_path() {
    test("Universal {0} survives intersection with every path");

    // Symbol 0: A is universal, T belongs to path 1.
    // Symbol 1: A is every path but 2, T belongs to path 2.
    EDS merged = transform_to_leds_with_sources(
        "{A,T}{A,T}",
        "{0}{1}{0,2}{2}",
        2
    );

    auto set0 = merged.read_symbol(0);
    assert(set0.size() <= 3);

    std::set<std::string> got(set0.begin(), set0.end());
    // AA: {0} ∩ (all but 2) — non-empty, paths 1 and 3.
    assert(got.count("AA") == 1);
    // AT: {0} ∩ {2} = {2} — survives.
    assert(got.count("AT") == 1);
    // TA: {1} ∩ (all but 2) = {1} — survives.
    assert(got.count("TA") == 1);
    // TT: {1} ∩ {2} = {} — must be pruned.
    assert(got.count("TT") == 0);

    pass();
}

// ===== CONTEXT LENGTH VALIDATION TESTS =====

void test_context_length_1() {
    test("Adjacent common symbols concatenate regardless of l");

    // Even at l=1, where every symbol is already long enough, {A}{B}{C} does
    // not survive as three symbols: needs_merge() treats two adjacent
    // non-degenerate symbols as ADJACENT_COMMON and concatenates them, because
    // an EDS has no reason to split a deterministic run. So the whole thing
    // collapses to one common symbol. The old expectation of 3 read the
    // constraint as the only thing that drives merging.
    EDS transformed = transform_to_leds("{A}{B}{C}", 1);

    assert(transformed.length() == 1);

    auto set0 = transformed.read_symbol(0);
    assert(set0.size() == 1 && set0[0] == "ABC");

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

    // {AC}{G,C}{T} at l=2 does not merge at all — {T} is the last symbol and so
    // exempt — which made the old min_context_length == 2 wrong (it is 1, from
    // that untouched {T}) and the test blind to whether statistics survive a
    // merge. Use an input that actually merges.
    EDS transformed = transform_to_leds("{ACGT}{A,C}{T}{G,A}{ACGT}", 2);

    const auto& meta = transformed.get_metadata();

    // {ACGT} + merged{ATG,ATA,CTG,CTA} + {ACGT}
    assert(transformed.length() == 3);
    assert(meta.num_degenerate_symbols == 1);
    assert(meta.min_context_length == 4);   // both remaining contexts are ACGT
    assert(meta.max_context_length == 4);
    assert(meta.num_common_chars == 8);

    pass();
}

void test_metadata_consistency() {
    test("Verify metadata consistency after transform");

    // Same correction as test_statistics_after_transform: the old input
    // {ACGT}{G,C}{T} is left untouched at l=2, so it checked the metadata of an
    // EDS that never went through a merge.
    EDS transformed = transform_to_leds("{ACGT}{A,C}{T}{G,A}{ACGT}", 2);

    assert(transformed.length() == 3);
    assert(transformed.cardinality() == 6);   // 1 + 4 + 1

    const auto& is_deg = transformed.get_metadata().is_degenerate;
    assert(is_deg.size() == 3);
    assert(is_deg[0] == false);   // {ACGT}
    assert(is_deg[1] == true);    // the merged symbol
    assert(is_deg[2] == false);   // {ACGT}

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

    // {A}{B,C}{D} at l=2 merges nothing: both {A} and {D} are boundary symbols
    // and so exempt from the context constraint. Flank it with long contexts so
    // the short common symbol is genuinely interior.
    EDS transformed = transform_to_leds("{AAA}{B,C}{D}{E,F}{AAA}", 2);

    assert(transformed.length() == 3);

    auto set1 = transformed.read_symbol(1);
    std::set<std::string> got(set1.begin(), set1.end());
    assert(got == (std::set<std::string>{"BDE", "BDF", "CDE", "CDF"}));

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
    test_merge_short_interior_context_via_leds();
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

    // Path-count invariant (complement-source regression)
    test_linear_merge_respects_path_count_bound();
    test_universal_marker_still_matches_every_path();

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
