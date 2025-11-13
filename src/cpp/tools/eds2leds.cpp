#include "transforms/eds_transforms.hpp"
#include "common.hpp"
#include <boost/program_options.hpp>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <filesystem>

namespace po = boost::program_options;
using namespace edsparser;

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
        Length context_length;
        int num_threads;
        bool compact_mode = true;  // Default to compact format
        bool full_mode = false;

        po::options_description desc("Transform EDS to l-EDS (length-constrained EDS)");
        desc.add_options()
            ("help,h", "Show help message")
            ("input,i", po::value<std::filesystem::path>(&input_file)->required(), "Input EDS file (.eds)")
            ("output,o", po::value<std::filesystem::path>(&output_file), "Output l-EDS file (default: <input>_l<N>.leds)")
            ("context-length,l", po::value<Length>(&context_length)->required(), "Minimum context length")
            ("sources,s", po::value<std::filesystem::path>(&sources_file), "Input source file (.seds) for linear (phasing-aware) merging")
            ("full", po::bool_switch(&full_mode), "Use full output format with brackets on all symbols (default: compact)")
            ("threads,t", po::value<int>(&num_threads)->default_value(1), "Number of threads for parallel processing");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

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
            std::cout << "    - Automatically used when --sources/-s is provided\n";
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
            std::cout << "OUTPUT FILES:\n";
            std::cout << "  Default output: <input_base>_l<N>.leds\n";
            std::cout << "  With sources:   <input_base>_l<N>.seds (source tracking preserved)\n";
            std::cout << "  where <N> is the context length value\n\n";
            print_performance();
            return 0;
        }

        po::notify(vm);

        // Handle full mode flag
        if (full_mode) {
            compact_mode = false;
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

        // Handle sources if provided
        std::ifstream* sources_in = nullptr;
        std::ofstream* sources_out = nullptr;

        if (!sources_file.empty()) {
            // Input sources file
            sources_in = new std::ifstream(sources_file);
            if (!*sources_in) {
                delete sources_in;
                throw std::runtime_error("Cannot open sources file: " + sources_file.string());
            }

            // Generate output sources filename
            std::filesystem::path output_sources = output_file;
            output_sources.replace_extension(".seds");

            sources_out = new std::ofstream(output_sources);
            if (!*sources_out) {
                delete sources_in;
                delete sources_out;
                throw std::runtime_error("Cannot create output sources file: " + output_sources.string());
            }

            std::cout << "  Output sources: " << output_sources << "\n";
        }

        try {
            // Call library function based on auto-detected method
            if (!sources_file.empty()) {
                // LINEAR merging: phasing-aware using source information
                edsparser::eds_to_leds_linear(
                    input,
                    output,
                    context_length,
                    sources_in,
                    sources_out,
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

            // Cleanup
            delete sources_in;
            delete sources_out;

            std::cout << "Transformation complete!\n";
            print_performance();
            return 0;

        } catch (...) {
            // Cleanup on exception
            delete sources_in;
            delete sources_out;
            throw;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        print_performance();
        return 1;
    }
}
