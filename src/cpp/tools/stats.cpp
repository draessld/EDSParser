#include "formats/eds.hpp"
#include "common.hpp"
#include <boost/program_options.hpp>
#include <set>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace po = boost::program_options;
using namespace edsparser;

// Format number with thousands separators
std::string format_number(size_t num) {
    std::stringstream ss;
    ss.imbue(std::locale(""));
    ss << std::fixed << num;
    return ss.str();
}

// Format file size in human-readable format
std::string format_size(uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_idx < 4) {
        size /= 1024.0;
        unit_idx++;
    }

    std::stringstream ss;
    ss << std::fixed << std::setprecision(1) << size << " " << units[unit_idx];
    return ss.str();
}

// Estimate RAM required if all EDS strings were loaded into memory (stream/string construction)
size_t estimate_in_memory_storage(size_t N, size_t m, size_t n) {
    // Rough estimation:
    // - String data: N bytes (actual characters)
    // - String overhead: m * sizeof(std::string) ≈ m * 32 bytes
    // - StringSet overhead: n * sizeof(std::vector) ≈ n * 24 bytes
    // - Pointers and bookkeeping: ~20% overhead

    size_t string_data = N;
    size_t string_overhead = m * 32;
    size_t vector_overhead = n * 24;
    size_t bookkeeping = (string_data + string_overhead + vector_overhead) / 5;

    return string_data + string_overhead + vector_overhead + bookkeeping;
}

// Estimate memory usage for METADATA_ONLY mode
size_t estimate_metadata_memory(size_t m, size_t n) {
    // Metadata structure:
    // - base_positions: n * sizeof(streampos) ≈ n * 8 bytes
    // - symbol_sizes: n * sizeof(Length) ≈ n * 4 bytes
    // - string_lengths: m * sizeof(Length) ≈ m * 4 bytes
    // - cum_set_sizes: n * sizeof(Length) ≈ n * 4 bytes
    // - is_degenerate: n / 8 bytes (bit-packed, but using bool: n * 1)
    // - Statistics: ~64 bytes
    // - Overhead: ~10%

    size_t base_positions = n * 8;
    size_t symbol_sizes = n * 4;
    size_t string_lengths = m * 4;
    size_t cum_set_sizes = n * 4;
    size_t is_degenerate = n * 1;
    size_t statistics = 64;
    size_t total = base_positions + symbol_sizes + string_lengths + cum_set_sizes + is_degenerate + statistics;
    size_t overhead = total / 10;

    return total + overhead;
}

// Source path statistics
struct SourceStats {
    size_t num_paths = 0;
    size_t max_paths_per_string = 0;
    double avg_paths_per_string = 0.0;
};

SourceStats compute_source_stats(const EDS& eds) {
    SourceStats result;
    if (!eds.has_sources()) return result;

    auto sources = eds.get_sources_object();
    size_t cardinality = sources->cardinality();
    if (cardinality == 0) return result;

    std::set<int> all_paths;
    size_t total_set_size = 0;

    for (size_t i = 0; i < cardinality; ++i) {
        auto paths = sources->read_source(i);
        // {0} is the universal marker — treat it as a single counted element
        all_paths.insert(paths.begin(), paths.end());
        if (paths.size() > result.max_paths_per_string)
            result.max_paths_per_string = paths.size();
        total_set_size += paths.size();
    }

    // Exclude universal marker 0 from path count (it means "all paths", not a real genome ID)
    all_paths.erase(0);
    result.num_paths = all_paths.size();
    result.avg_paths_per_string = static_cast<double>(total_set_size) / cardinality;
    return result;
}

// Print statistics in standard format
void print_standard(const EDS& eds, const std::filesystem::path& input_file, bool verbose, bool has_sources_file) {
    const auto& meta = eds.get_metadata();

    // Get file size
    uintmax_t file_size = std::filesystem::file_size(input_file);

    // Calculate memory estimates
    size_t metadata_mem = estimate_metadata_memory(eds.cardinality(), eds.length());
    size_t full_mem = estimate_in_memory_storage(eds.size(), eds.cardinality(), eds.length());
    double reduction_factor = static_cast<double>(full_mem) / static_cast<double>(metadata_mem);

    std::cout << "========================================\n";
    std::cout << "EDS Statistics\n";
    std::cout << "========================================\n";
    std::cout << "File: " << input_file.filename().string() << "\n";
    std::cout << "Size: " << format_size(file_size) << "\n";
    std::cout << "Storage Mode: STREAMING (memory-efficient)\n";
    std::cout << "\n";

    std::cout << "Structure:\n";
    std::cout << "  Number of symbols (n):        " << std::setw(12) << format_number(eds.length()) << "\n";
    std::cout << "  Total characters (N):         " << std::setw(12) << format_number(eds.size()) << "\n";
    std::cout << "  Total strings (m):            " << std::setw(12) << format_number(eds.cardinality()) << "\n";
    std::cout << "  Degenerate symbols:           " << std::setw(12) << format_number(meta.num_degenerate_symbols) << "\n";
    std::cout << "  Regular symbols:              " << std::setw(12) << format_number(eds.length() - meta.num_degenerate_symbols) << "\n";
    std::cout << "\n";

    std::cout << "Context Lengths (non-degenerate symbols):\n";
    std::cout << "  Minimum:                      " << std::setw(12) << meta.min_context_length << "\n";
    std::cout << "  Maximum:                      " << std::setw(12) << meta.max_context_length << "\n";
    std::cout << "  Average:                      " << std::setw(12) << std::fixed << std::setprecision(2) << meta.avg_context_length << "\n";
    std::cout << "\n";

    std::cout << "Variations:\n";
    std::cout << "  Total change size:            " << std::setw(12) << format_number(meta.total_change_size) << "\n";
    std::cout << "  Common characters:            " << std::setw(12) << format_number(meta.num_common_chars) << "\n";
    std::cout << "  Empty strings:                " << std::setw(12) << format_number(meta.num_empty_strings) << "\n";
    std::cout << "\n";

    if (verbose) {
        std::cout << "Detailed Metrics:\n";
        std::cout << "  Avg strings per symbol:       " << std::setw(12) << std::fixed << std::setprecision(2)
                  << (static_cast<double>(eds.cardinality()) / eds.length()) << "\n";
        std::cout << "  Avg chars per string:         " << std::setw(12) << std::fixed << std::setprecision(2)
                  << (static_cast<double>(eds.size()) / eds.cardinality()) << "\n";
        std::cout << "  Degenerate ratio:             " << std::setw(12) << std::fixed << std::setprecision(2)
                  << (100.0 * meta.num_degenerate_symbols / eds.length()) << " %\n";
        std::cout << "\n";
    }

    if (eds.has_sources()) {
        auto src_stats = compute_source_stats(eds);
        std::cout << "Sources (pangenome paths):\n";
        std::cout << "  Strings with source info:     " << std::setw(12) << format_number(eds.get_sources_object()->cardinality()) << "\n";
        std::cout << "  Total paths (genomes):        " << std::setw(12) << format_number(src_stats.num_paths) << "\n";
        std::cout << "  Max paths per string:         " << std::setw(12) << format_number(src_stats.max_paths_per_string) << "\n";
        std::cout << "  Avg paths per string:         " << std::setw(12) << std::fixed << std::setprecision(2) << src_stats.avg_paths_per_string << "\n";
        std::cout << "\n";
    } else if (has_sources_file) {
        std::cout << "Sources: File provided but parsing failed\n";
        std::cout << "\n";
    }

    std::cout << "Memory Usage:\n";
    std::cout << "  Current (STREAMING):          " << std::setw(12) << format_size(metadata_mem) << "\n";
    std::cout << "  Estimated (in-memory load):   " << std::setw(12) << format_size(full_mem) << "\n";
    std::cout << "  Reduction factor:             " << std::setw(12) << std::fixed << std::setprecision(1) << reduction_factor << "x\n";
    std::cout << "\n";

    // Recommendations
    std::cout << "Recommendations:\n";
    if (meta.min_context_length < 5) {
        std::cout << "  ⚠️  Minimum context length (" << meta.min_context_length << ") < typical l-EDS threshold (5)\n";
        std::cout << "  → Transformation to l-EDS may require merging adjacent symbols\n";
        std::cout << "  → Suggested command:\n";
        std::cout << "      edsparser-transform -i " << input_file.filename().string() << " -l 5 --method linear\n";
    } else {
        std::cout << "  ✓ Minimum context length (" << meta.min_context_length << ") ≥ 5\n";
        std::cout << "  → Ready for indexing with l ≤ " << meta.min_context_length << "\n";
    }

    std::cout << "========================================\n";
}

// Print statistics in JSON format
void print_json(const EDS& eds, const std::filesystem::path& input_file, bool has_sources_file,
                double runtime_s, double peak_memory_mb) {
    const auto& meta = eds.get_metadata();
    uintmax_t file_size = std::filesystem::file_size(input_file);

    size_t metadata_mem = estimate_metadata_memory(eds.cardinality(), eds.length());
    size_t full_mem = estimate_in_memory_storage(eds.size(), eds.cardinality(), eds.length());
    double reduction_factor = static_cast<double>(full_mem) / static_cast<double>(metadata_mem);

    std::cout << "{\n";
    std::cout << "  \"file\": {\n";
    std::cout << "    \"path\": \"" << input_file.string() << "\",\n";
    std::cout << "    \"size_bytes\": " << file_size << "\n";
    std::cout << "  },\n";
    std::cout << "  \"structure\": {\n";
    std::cout << "    \"n_symbols\": " << eds.length() << ",\n";
    std::cout << "    \"N_characters\": " << eds.size() << ",\n";
    std::cout << "    \"m_strings\": " << eds.cardinality() << ",\n";
    std::cout << "    \"degenerate_symbols\": " << meta.num_degenerate_symbols << ",\n";
    std::cout << "    \"regular_symbols\": " << (eds.length() - meta.num_degenerate_symbols) << "\n";
    std::cout << "  },\n";
    std::cout << "  \"context_lengths\": {\n";
    std::cout << "    \"min\": " << meta.min_context_length << ",\n";
    std::cout << "    \"max\": " << meta.max_context_length << ",\n";
    std::cout << "    \"avg\": " << std::fixed << std::setprecision(2) << meta.avg_context_length << "\n";
    std::cout << "  },\n";
    std::cout << "  \"variations\": {\n";
    std::cout << "    \"total_change_size\": " << meta.total_change_size << ",\n";
    std::cout << "    \"common_characters\": " << meta.num_common_chars << ",\n";
    std::cout << "    \"empty_strings\": " << meta.num_empty_strings << "\n";
    std::cout << "  },\n";
    std::cout << "  \"memory\": {\n";
    std::cout << "    \"current_bytes\": " << metadata_mem << ",\n";
    std::cout << "    \"current_mb\": " << std::fixed << std::setprecision(1) << (metadata_mem / 1024.0 / 1024.0) << ",\n";
    std::cout << "    \"estimated_full_bytes\": " << full_mem << ",\n";
    std::cout << "    \"estimated_full_mb\": " << std::fixed << std::setprecision(1) << (full_mem / 1024.0 / 1024.0) << ",\n";
    std::cout << "    \"reduction_factor\": " << std::fixed << std::setprecision(1) << reduction_factor << "\n";
    std::cout << "  },\n";
    auto src_stats = compute_source_stats(eds);
    std::cout << "  \"sources\": {\n";
    std::cout << "    \"loaded\": " << (eds.has_sources() ? "true" : "false") << ",\n";
    std::cout << "    \"file_provided\": " << (has_sources_file ? "true" : "false") << ",\n";
    std::cout << "    \"num_paths\": " << src_stats.num_paths << ",\n";
    std::cout << "    \"max_paths_per_string\": " << src_stats.max_paths_per_string << ",\n";
    std::cout << "    \"avg_paths_per_string\": " << std::fixed << std::setprecision(2) << src_stats.avg_paths_per_string << "\n";
    std::cout << "  },\n";
    std::cout << "  \"recommendations\": {\n";
    std::cout << "    \"needs_transformation\": " << (meta.min_context_length < 5 ? "true" : "false") << ",\n";
    std::cout << "    \"ready_for_indexing\": " << (meta.min_context_length >= 5 ? "true" : "false") << ",\n";
    std::cout << "    \"min_context_length\": " << meta.min_context_length << ",\n";
    std::cout << "    \"suggested_command\": \"" << (meta.min_context_length < 5
                  ? "edsparser-transform -i " + input_file.filename().string() + " -l 5"
                  : "ready for indexing") << "\"\n";
    std::cout << "  },\n";
    std::cout << "  \"performance\": {\n";
    std::cout << "    \"runtime_s\": " << std::fixed << std::setprecision(2) << runtime_s << ",\n";
    std::cout << "    \"peak_memory_mb\": " << std::fixed << std::setprecision(1) << peak_memory_mb << "\n";
    std::cout << "  }\n";
    std::cout << "}\n";
}

// Print statistics in CSV format
void print_csv(const EDS& eds, const std::filesystem::path& input_file, bool has_sources_file,
               double runtime_s, double peak_memory_mb) {
    const auto& meta = eds.get_metadata();
    uintmax_t file_size = std::filesystem::file_size(input_file);

    size_t metadata_mem = estimate_metadata_memory(eds.cardinality(), eds.length());
    size_t full_mem = estimate_in_memory_storage(eds.size(), eds.cardinality(), eds.length());
    double reduction_factor = static_cast<double>(full_mem) / static_cast<double>(metadata_mem);

    auto src_stats = compute_source_stats(eds);

    // Header
    std::cout << "file,file_size_bytes"
              << ",n_symbols,N_characters,m_strings,degenerate_symbols,regular_symbols"
              << ",context_min,context_max,context_avg"
              << ",total_change_size,common_characters,empty_strings"
              << ",memory_current_bytes,memory_estimated_full_bytes,memory_reduction_factor"
              << ",sources_loaded,num_paths,max_paths_per_string,avg_paths_per_string"
              << ",runtime_s,peak_memory_mb"
              << "\n";

    // Data row
    std::cout << input_file.string() << "," << file_size
              << "," << eds.length()
              << "," << eds.size()
              << "," << eds.cardinality()
              << "," << meta.num_degenerate_symbols
              << "," << (eds.length() - meta.num_degenerate_symbols)
              << "," << meta.min_context_length
              << "," << meta.max_context_length
              << "," << std::fixed << std::setprecision(2) << meta.avg_context_length
              << "," << meta.total_change_size
              << "," << meta.num_common_chars
              << "," << meta.num_empty_strings
              << "," << metadata_mem
              << "," << full_mem
              << "," << std::fixed << std::setprecision(1) << reduction_factor
              << "," << (eds.has_sources() ? "true" : "false")
              << "," << src_stats.num_paths
              << "," << src_stats.max_paths_per_string
              << "," << std::fixed << std::setprecision(2) << src_stats.avg_paths_per_string
              << "," << std::fixed << std::setprecision(2) << runtime_s
              << "," << std::fixed << std::setprecision(1) << peak_memory_mb
              << "\n";
    (void)has_sources_file;
}

int main(int argc, char** argv) {
    // Start performance tracking
    Timer timer;
    timer.start();

    // Print performance to stderr (used only in non-structured output modes and on error)
    auto print_perf_stderr = [&timer]() {
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
        std::filesystem::path sources_file;
        bool json_output = false;
        bool csv_output = false;
        bool verbose = false;

        po::options_description desc("Display statistics for EDS/l-EDS file");
        desc.add_options()
            ("help,h", "Show help message")
            ("input,i", po::value<std::filesystem::path>(&input_file)->required(), "Input EDS file")
            ("sources,s", po::value<std::filesystem::path>(&sources_file), "Source file (.seds) - optional")
            ("json,j", po::bool_switch(&json_output), "Output in JSON format")
            ("csv,c", po::bool_switch(&csv_output), "Output in CSV format")
            ("verbose,v", po::bool_switch(&verbose), "Show detailed statistics");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << "edsparser-stats - Display EDS statistics\n\n";
            std::cout << desc << "\n";
            std::cout << "Examples:\n";
            std::cout << "  # Show statistics for EDS file (memory-efficient):\n";
            std::cout << "  edsparser-stats -i data.eds\n\n";
            std::cout << "  # Show statistics with sources:\n";
            std::cout << "  edsparser-stats -i data.eds -s data.seds\n\n";
            std::cout << "  # Show statistics in JSON format:\n";
            std::cout << "  edsparser-stats -i data.eds --json\n\n";
            std::cout << "  # Show statistics in CSV format:\n";
            std::cout << "  edsparser-stats -i data.eds --csv\n\n";
            std::cout << "  # Show verbose statistics:\n";
            std::cout << "  edsparser-stats -i data.eds --verbose\n\n";
            std::cout << "Note:\n";
            std::cout << "  Uses streaming (metadata-only) mode for memory-efficient access to large files.\n";
            std::cout << "  JSON and CSV output include performance metrics inline.\n";
            print_perf_stderr();
            return 0;
        }

        po::notify(vm);

        if (json_output && csv_output) {
            std::cerr << "Error: --json and --csv are mutually exclusive\n";
            return 1;
        }

        // Check if input file exists
        if (!std::filesystem::exists(input_file)) {
            std::cerr << "Error: Input file '" << input_file << "' not found\n";
            print_perf_stderr();
            return 1;
        }

        // Load EDS (always uses streaming mode)
        EDS eds;
        bool has_sources = vm.count("sources") > 0;
        if (has_sources) {
            if (!std::filesystem::exists(sources_file)) {
                std::cerr << "Error: Source file '" << sources_file << "' not found\n";
                print_perf_stderr();
                return 1;
            }
            eds = EDS::load(input_file, sources_file);
        } else {
            eds = EDS::load(input_file);
        }

        // Stop timer before output so performance data is captured accurately
        timer.stop();
        double runtime = timer.elapsed_seconds();
        double memory_mb = get_peak_memory_mb();

        // Output statistics
        if (json_output) {
            print_json(eds, input_file, has_sources, runtime, memory_mb);
        } else if (csv_output) {
            print_csv(eds, input_file, has_sources, runtime, memory_mb);
        } else {
            print_standard(eds, input_file, verbose, has_sources);
            std::cerr << "[Performance] Runtime: " << std::fixed << std::setprecision(2) << runtime << "s";
            if (memory_mb > 0.0) {
                std::cerr << " | Peak Memory: " << std::fixed << std::setprecision(1) << memory_mb << " MB";
            }
            std::cerr << "\n";
        }

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        print_perf_stderr();
        return 1;
    }
}
