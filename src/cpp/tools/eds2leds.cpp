#include "transforms/eds_transforms.hpp"
#include "formats/eds.hpp"
#include "formats/sources.hpp"
#include "common.hpp"
#include <boost/program_options.hpp>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <optional>
#include <string>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <unistd.h>

namespace po = boost::program_options;
using namespace edsparser;

// Distinct exit code so orchestrators can tell "would exceed memory budget" apart
// from an ordinary failure (e.g. mark the chromosome too-intensive and move on).
static constexpr int EXIT_MEMORY_EXCEEDED = 3;

namespace {

// Parse a human-readable byte size: "450G", "450GB", "2T", "512M", "1024" (bytes).
// Binary units (1G = 1024^3). Returns 0 for an empty string; throws on garbage.
unsigned long long parse_size(const std::string& s) {
    if (s.empty()) return 0;
    char* end = nullptr;
    double val = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || val < 0)
        throw std::runtime_error("invalid --max-memory value: '" + s + "'");
    std::string suf(end);
    size_t b = suf.find_first_not_of(" \t");
    suf = (b == std::string::npos) ? "" : suf.substr(b);
    unsigned long long mult = 1;
    if (!suf.empty()) {
        switch (std::toupper(static_cast<unsigned char>(suf[0]))) {
            case 'K': mult = 1ULL << 10; break;
            case 'M': mult = 1ULL << 20; break;
            case 'G': mult = 1ULL << 30; break;
            case 'T': mult = 1ULL << 40; break;
            case 'B': mult = 1;          break;  // bare "B"
            default:
                throw std::runtime_error("invalid --max-memory unit in: '" + s + "'");
        }
    }
    return static_cast<unsigned long long>(val * static_cast<double>(mult));
}

// --source-format value → Sources::Format. The merge pipeline itself always
// writes dense text SEDS (it streams entries as it merges and has no binary
// writer); anything else is produced by re-encoding that file once at the end.
// Returns false for an unrecognised name.
bool parse_source_format(const std::string& s, Sources::Format& out) {
    if (s == "seds")           { out = Sources::Format::SEDS;           return true; }
    if (s == "seds-sparse")    { out = Sources::Format::SEDS_SPARSE;    return true; }
    if (s == "edz")            { out = Sources::Format::EDZ;            return true; }
    if (s == "edz-sparse")     { out = Sources::Format::EDZ_SPARSE;     return true; }
    if (s == "edz-compressed") { out = Sources::Format::EDZ_COMPRESSED; return true; }
    return false;
}

// Extension the given source format is stored under. All EDZ variants share
// ".edz" (they are distinguished by header flags, not by name).
const char* source_format_extension(Sources::Format f) {
    switch (f) {
        case Sources::Format::EDZ:
        case Sources::Format::EDZ_SPARSE:
        case Sources::Format::EDZ_COMPRESSED: return ".edz";
        default:                              return ".seds";
    }
}

std::string human_bytes(unsigned long long b) {
    if (b == std::numeric_limits<unsigned long long>::max()) return "unbounded";
    const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = static_cast<double>(b);
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; ++i; }
    std::ostringstream o;
    o << std::fixed << std::setprecision(1) << v << " " << u[i];
    return o.str();
}

// Removes a temp symlink (and its containing directory) created to force
// EDZ format detection on a sources file whose extension isn't ".edz".
// detect_format() dispatches purely on path extension, so this is the
// cheapest way to override it without touching the library's format logic.
struct EdzSymlinkGuard {
    std::filesystem::path link;
    ~EdzSymlinkGuard() {
        if (link.empty()) return;
        std::error_code ec;
        std::filesystem::remove(link, ec);
        std::filesystem::remove(link.parent_path(), ec);
    }
};
}  // namespace

int main(int argc, char** argv) {
    // Start performance tracking
    Timer timer;
    timer.start();

    // Helper to print performance info to stderr
    auto print_performance = [&timer]() {
        timer.stop();
        double runtime = timer.elapsed_seconds();
        double memory_mb = get_peak_memory_mb();
        std::cerr << "[Performance] Runtime: " << std::fixed << std::setprecision(2) << runtime << "s";
        if (memory_mb > 0.0) {
            std::cerr << " | Peak Memory: " << std::fixed << std::setprecision(1) << memory_mb << " MB";
        }
        std::cerr << "\n";
    };

    try {
        std::filesystem::path input_file;
        std::filesystem::path output_file;
        std::filesystem::path sources_file;
        std::filesystem::path sources_edz_file;
        Length context_length;
        int num_threads;
        bool compact_mode = true;  // Default to compact format
        bool full_mode = false;
        std::string max_memory_str;
        std::string source_format_str;
        std::string block_size_str;
        bool estimate_only = false;

        po::options_description desc("Transform EDS to l-EDS (length-constrained EDS)");
        desc.add_options()
            ("help,h", "Show help message")
            ("version,V", "Print version and build provenance (COMMIT, COMMIT_DATE, DIRTY) and exit")
            ("input,i", po::value<std::filesystem::path>(&input_file)->required(), "Input EDS file (.eds)")
            ("output,o", po::value<std::filesystem::path>(&output_file), "Output l-EDS file (default: <input>_l<N>.leds)")
            ("context-length,l", po::value<Length>(&context_length)->required(), "Minimum context length")
            ("seds,s", po::value<std::filesystem::path>(&sources_file), "Input source file (.seds/.edz) for linear (phasing-aware) merging; format auto-detected from extension/content")
            ("edz,z", po::value<std::filesystem::path>(&sources_edz_file), "Input source file for linear merging, explicitly treated as binary EDZ format regardless of its extension (mutually exclusive with -s)")
            ("full", po::bool_switch(&full_mode), "Use full output format with brackets on all symbols (default: compact)")
            ("threads,t", po::value<int>(&num_threads)->default_value(1), "Number of threads for parallel processing")
            ("source-format", po::value<std::string>(&source_format_str)->default_value("seds"),
                "Format for the output sources file: seds (default, dense text), "
                "seds-sparse, edz, edz-sparse, edz-compressed. Non-SEDS formats are "
                "produced by re-encoding the merged sources once at the end; EDZ "
                "variants are written as <output>.edz. edz-compressed needs a "
                "zstd-enabled build and is typically several times smaller than SEDS.")
            ("estimate-memory", po::bool_switch(&estimate_only),
                "Print a machine-readable memory estimate for this transform and exit 0 "
                "without doing any work. Emits KEY=BYTES lines (RECOMMENDED_BUDGET_BYTES "
                "is the one to schedule on). Use it to decide how many runs can safely "
                "share a machine.")
            ("block-size", po::value<std::string>(&block_size_str),
                "Process the input in blocks of about this many EDS bytes (e.g. 200M, 1G) "
                "so peak RAM is bounded by the block instead of the file. Cuts land only "
                "where no merge can cross — a run of common symbols totalling >= -l — so "
                "the output is identical to a whole-file run. Slower (each block is sliced "
                "to a temp file first); falls back to whole-file if the input has no such "
                "run at a block boundary.")
            ("max-memory", po::value<std::string>(&max_memory_str),
                "Pre-flight guard: estimate worst-case merge RAM from metadata and refuse "
                "the transform (exit 3) if it exceeds this size (e.g. 450G, 512M, 2T). "
                "With sources the bound uses num_paths (linear); without, the cartesian product.");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("version")) {
            edsparser::print_version("eds2leds");
            return 0;
        }

        if (vm.count("help")) {
            std::cout << "eds2leds - Transform EDS to l-EDS (length-constrained EDS)\n\n";
            std::cout << desc << "\n";
            std::cout << "DESCRIPTION:\n";
            std::cout << "  Transforms an Elastic-Degenerate String (EDS) to a length-constrained\n";
            std::cout << "  EDS (l-EDS) by merging adjacent symbols to ensure all non-degenerate\n";
            std::cout << "  regions meet the minimum context length requirement.\n\n";
            std::cout << "MERGING METHODS (auto-detected):\n";
            std::cout << "  WITH sources:\n";
            std::cout << "    - Phasing-aware merging using source information\n";
            std::cout << "    - Automatically used when --seds/-s or --edz/-z is provided\n";
            std::cout << "    - Preserves valid haplotype combinations\n";
            std::cout << "    - Use for: Genomic data with known phasing (MSA/VCF-derived)\n\n";
            std::cout << "  WITHOUT sources:\n";
            std::cout << "    - All-combinations merging (cross-product of alternatives)\n";
            std::cout << "    - Automatically used when no source file is provided\n";
            std::cout << "    - Use for: Unknown phasing or when all combinations needed\n\n";
            std::cout << "OUTPUT MODES:\n";
            std::cout << "  Default (compact): Omit brackets on non-degenerate symbols: ACGT{A,ACA}CGT\n";
            std::cout << "  --full: Use brackets on all symbols: {ACGT}{A,ACA}{CGT}\n\n";
            std::cout << "EXAMPLES:\n";
            std::cout << "  # Linear merging (auto-detected with sources, compact output):\n";
            std::cout << "  eds2leds -i data.eds -s data.seds -l 5\n\n";
            std::cout << "  # Cartesian merging (auto-detected without sources):\n";
            std::cout << "  eds2leds -i data.eds -l 5\n\n";
            std::cout << "  # Full output format with brackets on all symbols:\n";
            std::cout << "  eds2leds -i data.eds -s data.seds -l 5 --full\n\n";
            std::cout << "  # Parallel processing with 4 threads:\n";
            std::cout << "  eds2leds -i data.eds -l 5 --threads 4\n\n";
            std::cout << "  # Custom output path:\n";
            std::cout << "  eds2leds -i data.eds -s data.seds -l 10 -o output.leds\n\n";
            std::cout << "  # Explicitly force EDZ (binary) source format regardless of extension:\n";
            std::cout << "  eds2leds -i data.eds -z data.sources -l 5\n\n";
            std::cout << "OUTPUT FILES:\n";
            std::cout << "  Default output: <input_base>_l<N>.leds\n";
            std::cout << "  With sources:   <input_base>_l<N>.seds (source tracking preserved)\n";
            std::cout << "  where <N> is the context length value\n\n";
            std::cout << "DISK SPACE:\n";
            std::cout << "  Requires ~3× the input file size as temporary disk space:\n";
            std::cout << "    1× working copy of the input\n";
            std::cout << "    2× for the previous and current iteration files during each merge pass\n";
            std::cout << "  Temp files live in " << std::filesystem::temp_directory_path().string()
                      << "/edsparser_leds_<pid>/ and are cleaned up on exit.\n";
            std::cout << "  Set $TMPDIR to redirect temp files to a volume with enough space.\n\n";
            print_performance();
            return 0;
        }

        po::notify(vm);

        // Handle full mode flag
        if (full_mode) {
            compact_mode = false;
        }

        if (vm.count("seds") && vm.count("edz")) {
            std::cerr << "Error: --seds/-s and --edz/-z are mutually exclusive\n";
            print_performance();
            return 1;
        }

        // -z forces EDZ interpretation of the sources file regardless of its
        // on-disk extension. detect_format() (used deep in the merge pipeline)
        // dispatches purely on path extension, so when the given file isn't
        // already named ".edz" we point a temp ".edz" symlink at it instead of
        // touching the file itself.
        EdzSymlinkGuard edz_symlink_guard;
        if (vm.count("edz")) {
            if (!std::filesystem::exists(sources_edz_file)) {
                std::cerr << "Error: Cannot open sources file: " << sources_edz_file << "\n";
                print_performance();
                return 1;
            }
            if (sources_edz_file.extension() == ".edz") {
                sources_file = sources_edz_file;
            } else {
                std::filesystem::path link_dir = std::filesystem::temp_directory_path()
                    / ("edsparser_eds2leds_z_" + std::to_string(getpid()));
                std::filesystem::create_directories(link_dir);
                edz_symlink_guard.link = link_dir / "sources.edz";
                std::filesystem::create_symlink(std::filesystem::absolute(sources_edz_file),
                                                 edz_symlink_guard.link);
                sources_file = edz_symlink_guard.link;
            }
        }

        // Validate input file extension
        if (input_file.extension() != ".eds") {
            std::cerr << "Error: Input file must be an EDS file (.eds)\n";
            std::cerr << "Got: " << input_file << "\n";
            print_performance();
            return 1;
        }

        // Validate threads
        if (num_threads < 1) {
            std::cerr << "Error: Number of threads must be >= 1\n";
            print_performance();
            return 1;
        }

        // Validate context length
        if (context_length == 0) {
            std::cerr << "Error: Context length must be > 0\n";
            print_performance();
            return 1;
        }

        // Block size: 0 / absent disables block mode.
        unsigned long long block_bytes = 0;
        if (vm.count("block-size")) {
            block_bytes = parse_size(block_size_str);
            if (block_bytes == 0) {
                std::cerr << "Error: --block-size must be > 0 (got '" << block_size_str << "')\n";
                print_performance();
                return 1;
            }
        }

        // Validate the requested output source format up front — failing after a
        // multi-hour merge because zstd is missing would be cruel.
        Sources::Format out_source_format = Sources::Format::SEDS;
        if (!parse_source_format(source_format_str, out_source_format)) {
            std::cerr << "Error: unknown --source-format '" << source_format_str
                      << "' (expected seds, seds-sparse, edz, edz-sparse, edz-compressed)\n";
            print_performance();
            return 1;
        }
        if (out_source_format == Sources::Format::EDZ_COMPRESSED &&
            !Sources::edz_compressed_available()) {
            std::cerr << "Error: --source-format edz-compressed requires a zstd-enabled build\n";
            print_performance();
            return 1;
        }
        if (source_format_str != "seds" && sources_file.empty()) {
            std::cerr << "Warning: --source-format has no effect without sources "
                         "(-s/-z); a cartesian merge writes no source file\n";
        }

        // Generate output filename if not provided
        if (output_file.empty()) {
            std::string base_name = input_file.stem().string();
            std::string suffix = "_l" + std::to_string(context_length);
            output_file = input_file.parent_path() / (base_name + suffix + ".leds");
        }

        std::cout << "EDS → l-EDS transformation\n";
        std::cout << "  Input: " << input_file << "\n";
        std::cout << "  Output: " << output_file << "\n";
        std::cout << "  Context length: " << context_length << "\n";
        if (!sources_file.empty()) {
            std::cout << "  Sources: " << sources_file << "\n";
        }
        std::cout << "  Output mode: " << (compact_mode ? "compact" : "full") << "\n";
        std::cout << "  Threads: " << num_threads << (num_threads == 1 ? " (sequential)" : " (parallel)") << "\n";
        if (block_bytes > 0)
            std::cout << "  Block size: " << human_bytes(block_bytes)
                      << " of input EDS per block (bounded memory)\n";

        // ===== MEMORY ESTIMATE (shared by --estimate-memory and --max-memory) =====
        // Both consumers need the same metadata-only numbers, so compute once. The
        // work here is a metadata load plus one merge-group scan — no merging, no
        // string reads.
        const bool need_estimate = estimate_only || vm.count("max-memory") > 0;

        unsigned long long budget = 0;
        if (vm.count("max-memory")) {
            budget = parse_size(max_memory_str);
            if (budget == 0) {
                std::cerr << "Error: --max-memory must be > 0 (got '" << max_memory_str << "')\n";
                print_performance();
                return 1;
            }
        }

        if (need_estimate) {
            // A linear (phased) merge can't produce more strings than there are source
            // paths; use that as the cap. Without sources, assume cartesian (no cap).
            unsigned long long path_cap = 0;
            unsigned long long num_strings_sources = 0;
            if (!sources_file.empty()) {
                try {
                    auto src = Sources::load(sources_file);
                    path_cap = static_cast<unsigned long long>(src->num_paths());
                    num_strings_sources = static_cast<unsigned long long>(src->cardinality());
                } catch (const std::exception& e) {
                    std::cerr << "  [memory] warning: could not read source paths ("
                              << e.what() << "); using the cartesian (uncapped) bound\n";
                }
            }

            EDS probe = EDS::load(input_file);  // metadata-only, cheap
            auto est = estimate_worst_case_merge_memory(
                probe, context_length, path_cap);

            // Resident cost of the structures that live for the whole run, alongside
            // the merge metadata the estimate above covers. Measured layout (see
            // CLAUDE.md): per symbol base_positions 8 + symbol_sizes 4 +
            // cum_set_sizes 4 + is_degenerate 1/8; per string string_lengths 4. The
            // merge holds an input and an output metadata at once, hence the ×2.
            const unsigned long long n = probe.length();
            const unsigned long long m = probe.cardinality();
            const unsigned long long eds_index_bytes = 2ULL * (n * 16 + n / 8 + m * 4);
            // Sources keep one 8-byte byte-offset per entry for the whole file.
            const unsigned long long sources_index_bytes = num_strings_sources * 8;

            // The merge-group list is held for a whole iteration: one MergeGroup
            // (start, count, reason) per group.
            const unsigned long long groups_bytes =
                static_cast<unsigned long long>(est.num_groups) * 24;

            // --block-size bounds the per-symbol indices to one block instead of the
            // whole file. Blocks are cut at barriers, so their size is approximately
            // (not exactly) block_bytes — scale the index terms by the block's share
            // of the input. The sources index stays whole-file in linear mode.
            unsigned long long eds_index_scaled = eds_index_bytes;
            if (block_bytes > 0) {
                std::error_code fs_ec;
                const auto file_bytes = std::filesystem::file_size(input_file, fs_ec);
                if (!fs_ec && file_bytes > 0 && block_bytes < file_bytes) {
                    eds_index_scaled = static_cast<unsigned long long>(
                        static_cast<long double>(eds_index_bytes) *
                        static_cast<long double>(block_bytes) /
                        static_cast<long double>(file_bytes));
                }
            }

            unsigned long long recommended;
            if (est.saturated || est.peak_batch_bytes ==
                    std::numeric_limits<unsigned long long>::max()) {
                recommended = std::numeric_limits<unsigned long long>::max();
            } else {
                // 1.15× headroom + 64 MB covers what the component sum misses
                // (allocator overhead, LRU source cache, stream buffers). Calibrated
                // against measured peaks: predicts 2.47 GB for a run that peaked at
                // 2.38 GB, and 216 MB for one that peaked at 159 MB — deliberately
                // on the safe side, since schedulers admit work based on this.
                const unsigned long long sum = eds_index_scaled + sources_index_bytes
                                             + est.peak_batch_bytes + groups_bytes;
                recommended = static_cast<unsigned long long>(
                                  static_cast<long double>(sum) * 1.15L)
                            + (64ULL << 20);
            }

            if (estimate_only) {
                // Machine-readable: stable KEY=BYTES lines for schedulers to parse.
                std::cout << "ESTIMATE_FOR=" << input_file.string() << "\n";
                std::cout << "CONTEXT_LENGTH=" << context_length << "\n";
                std::cout << "MERGE_PEAK_BYTES=" << est.peak_batch_bytes << "\n";
                std::cout << "EDS_INDEX_BYTES=" << eds_index_scaled << "\n";
                std::cout << "MERGE_GROUPS_BYTES=" << groups_bytes << "\n";
                std::cout << "BLOCK_SIZE_BYTES=" << block_bytes << "\n";
                std::cout << "SOURCES_INDEX_BYTES=" << sources_index_bytes << "\n";
                std::cout << "RECOMMENDED_BUDGET_BYTES=" << recommended << "\n";
                std::cout << "MERGE_GROUPS=" << est.num_groups << "\n";
                std::cout << "PATH_CAP=" << path_cap << "\n";
                std::cout << "SATURATED=" << (est.saturated ? 1 : 0) << "\n";
                std::cout << "# human: merge " << human_bytes(est.peak_batch_bytes)
                          << " + indices " << human_bytes(eds_index_scaled + sources_index_bytes)
                          << " → recommend " << human_bytes(recommended) << "\n";
                print_performance();
                return 0;
            }

            // Gate on the FULL predicted peak, not just the merge metadata. The merge
            // term alone is tiny for a linear merge (measured: 295 KiB for a run that
            // peaked at 2.3 GB — the index arrays dominate), so comparing only that
            // made the guard almost unfirable in linear mode and let a run that could
            // not possibly fit start anyway.
            std::cout << "  Memory pre-flight: est. peak " << human_bytes(recommended)
                      << " (merge " << human_bytes(est.peak_batch_bytes)
                      << " + indices " << human_bytes(eds_index_scaled + sources_index_bytes)
                      << "; budget " << human_bytes(budget) << ", "
                      << est.num_groups << " merge groups, "
                      << (path_cap ? "linear cap " + std::to_string(path_cap) + " paths"
                                   : "cartesian bound")
                      << ")\n";

            if (est.saturated || recommended > budget) {
                std::cerr << "Error: estimated peak memory "
                          << human_bytes(recommended)
                          << " exceeds --max-memory " << human_bytes(budget) << ".\n";
                std::cerr << "  Worst merge group: " << est.peak_group_count
                          << " symbols at position " << est.peak_group_start
                          << " → up to " << (est.peak_group_combos ==
                                 std::numeric_limits<unsigned long long>::max()
                                 ? std::string("unbounded")
                                 : std::to_string(est.peak_group_combos))
                          << " output strings (" << human_bytes(est.peak_group_bytes) << ").\n";
                std::cerr << "  Skipping (too intensive). Retry with a larger --max-memory, "
                             "a larger context length, or on a bigger machine.\n";
                print_performance();
                return EXIT_MEMORY_EXCEEDED;
            }
        }

        // Open input file
        std::ifstream input(input_file);
        if (!input) {
            throw std::runtime_error("Cannot open input file: " + input_file.string());
        }

        // Open output file
        std::ofstream output(output_file);
        if (!output) {
            throw std::runtime_error("Cannot open output file: " + output_file.string());
        }

        // Handle sources if provided. RAII-owned so the stream is closed on any
        // exit path (normal return or exception) without manual delete.
        std::optional<std::ofstream> sources_out;

        // Intended final path for the output sources file (used for rename below).
        std::filesystem::path output_sources;
        // Actual path written to during the transform (may be a temp file).
        std::filesystem::path actual_sources_out_path;
        bool rename_sources_after = false;

        if (!sources_file.empty()) {
            // Verify sources file is readable before starting the transform.
            if (!std::filesystem::exists(sources_file)) {
                throw std::runtime_error("Cannot open sources file: " + sources_file.string());
            }

            // Generate output sources filename (same stem as output .leds; the
            // extension follows the requested format — .edz for EDZ variants).
            output_sources = output_file;
            output_sources.replace_extension(source_format_extension(out_source_format));

            // Guard against self-overwrite: if output_sources resolves to the same file as
            // sources_file, opening it for writing (O_TRUNC) would zero out the input before
            // eds_to_leds_linear can copy it to its temp directory.  Detect this and write
            // to a temp path instead, then rename to the final destination afterwards.
            actual_sources_out_path = output_sources;
            // The merge pipeline can only write dense text SEDS, so for any other
            // requested format it writes to a temp file that is re-encoded into
            // output_sources afterwards.
            if (out_source_format != Sources::Format::SEDS) {
                actual_sources_out_path =
                    output_sources.parent_path() /
                    (output_sources.stem().string() + ".eds2leds_tmp.seds");
            }
            try {
                if (std::filesystem::exists(output_sources) &&
                    std::filesystem::equivalent(output_sources, sources_file)) {
                    actual_sources_out_path =
                        output_sources.parent_path() /
                        (output_sources.stem().string() + ".eds2leds_tmp.seds");
                    rename_sources_after = true;
                }
            } catch (const std::filesystem::filesystem_error&) {
                // Files on different filesystems — no conflict possible.
            }

            sources_out.emplace(actual_sources_out_path);
            if (!*sources_out) {
                throw std::runtime_error("Cannot create output sources file: " +
                                         actual_sources_out_path.string());
            }

            std::cout << "  Output sources: " << output_sources << "\n";
        }

        try {
            // Block mode drives the same merge core one block at a time; it needs
            // the input as a path (it slices byte ranges out of it) and handles both
            // linear and cartesian internally.
            if (block_bytes > 0) {
                edsparser::eds_to_leds_blocked(
                    input_file,
                    output,
                    context_length,
                    sources_file.empty() ? nullptr : &sources_file,
                    sources_out ? &*sources_out : nullptr,
                    block_bytes,
                    static_cast<size_t>(num_threads),
                    compact_mode
                );
            } else if (!sources_file.empty()) {
                // LINEAR merging: phasing-aware using source information.
                // Use the path-based overload so detect_format() sees the real extension
                // (.edz or .seds) and picks the correct parser.
                edsparser::eds_to_leds_linear(
                    input_file,
                    output,
                    context_length,
                    &sources_file,
                    sources_out ? &*sources_out : nullptr,
                    static_cast<size_t>(num_threads),
                    compact_mode
                );
            } else {
                // CARTESIAN merging: all combinations
                edsparser::eds_to_leds_cartesian(
                    input,
                    output,
                    context_length,
                    static_cast<size_t>(num_threads),
                    compact_mode
                );
            }

            // Close the stream before rename (file must be closed on some platforms).
            sources_out.reset();

            // Re-encode the merged sources into the requested format, then drop the
            // text file. Conversion is entry-by-entry (Sources::save_as streams via
            // read_source), so it costs one extra pass over the sources — worth it:
            // edz-compressed measured ~4× smaller than the text SEDS it replaces.
            if (out_source_format != Sources::Format::SEDS && !output_sources.empty()) {
                auto text_size = std::filesystem::file_size(actual_sources_out_path);
                auto merged = Sources::load(actual_sources_out_path, Sources::Format::SEDS);
                merged->save_as(output_sources, out_source_format);
                std::filesystem::remove(actual_sources_out_path);

                auto final_size = std::filesystem::file_size(output_sources);
                std::cout << "  Sources re-encoded as " << source_format_str << ": "
                          << human_bytes(text_size) << " → " << human_bytes(final_size);
                if (final_size > 0)
                    std::cout << " (" << std::fixed << std::setprecision(1)
                              << (static_cast<double>(text_size) / static_cast<double>(final_size))
                              << "× smaller)";
                std::cout << "\n";
            } else if (rename_sources_after) {
                // We wrote to a temp file to avoid self-overwrite; rename it now.
                std::filesystem::rename(actual_sources_out_path, output_sources);
            }

            std::cout << "Transformation complete!\n";
            print_performance();
            return 0;

        } catch (...) {
            // Close the stream, then remove the temp file if the rename did not happen.
            sources_out.reset();
            if (rename_sources_after &&
                std::filesystem::exists(actual_sources_out_path)) {
                std::filesystem::remove(actual_sources_out_path);
            }
            throw;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        print_performance();
        return 1;
    }
}
