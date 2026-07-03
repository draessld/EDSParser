#include "transforms/msa_transforms.hpp"
#include "common.hpp"
#include "progress_bar.hpp"
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

        po::options_description desc("Transform MSA (Multiple Sequence Alignment) to EDS/l-EDS");
        desc.add_options()
            ("help,h", "Show help message")
            ("input,i", po::value<std::filesystem::path>(&input_file)->required(), "Input MSA file (.msa) in FASTA format with gaps as '-'")
            ("output,o", po::value<std::filesystem::path>(&output_file), "Output EDS file (default: <input>.eds)")
            ("seds,s", po::value<std::filesystem::path>(&sources_file), "Output source file (default: <output>.seds)")
            ("context-length,l", po::value<Length>(&context_length)->default_value(0), "Create l-EDS with minimum context length (0 = regular EDS)");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << "msa2eds - Transform MSA (Multiple Sequence Alignment) to EDS\n\n";
            std::cout << desc << "\n";
            std::cout << "DESCRIPTION:\n";
            std::cout << "  Transforms a Multiple Sequence Alignment (MSA) in FASTA format to an\n";
            std::cout << "  Elastic-Degenerate String (EDS) with source tracking. Gaps in the MSA\n";
            std::cout << "  (represented as '-') are used to identify variant regions.\n\n";
            std::cout << "  The transformation automatically tracks the source sequence for each\n";
            std::cout << "  alternative string, enabling phasing-aware downstream processing.\n\n";
            std::cout << "INPUT FORMAT:\n";
            std::cout << "  FASTA format with aligned sequences\n";
            std::cout << "  Gap character: '-'\n";
            std::cout << "  Example:\n";
            std::cout << "    >seq1\n";
            std::cout << "    ACGT-TAG\n";
            std::cout << "    >seq2\n";
            std::cout << "    ACGTATAG\n";
            std::cout << "    >seq3\n";
            std::cout << "    ACGT--AG\n\n";
            std::cout << "EXAMPLES:\n";
            std::cout << "  # Basic transformation (MSA → EDS):\n";
            std::cout << "  msa2eds -i alignment.msa\n";
            std::cout << "  # Creates: alignment.eds and alignment.seds\n\n";
            std::cout << "  # Direct transformation to l-EDS:\n";
            std::cout << "  msa2eds -i alignment.msa -l 10\n";
            std::cout << "  # Creates: alignment_l10.leds and alignment_l10.seds\n";
            std::cout << "  # Skips intermediate EDS step for efficiency\n\n";
            std::cout << "  # Custom output paths:\n";
            std::cout << "  msa2eds -i alignment.msa -o output.eds -s output.seds\n\n";
            std::cout << "OUTPUT:\n";
            std::cout << "  Regular EDS:\n";
            std::cout << "    <input_base>.eds   - EDS file\n";
            std::cout << "    <input_base>.seds  - Source tracking file\n\n";
            std::cout << "  l-EDS (with -l):\n";
            std::cout << "    <input_base>_l<N>.leds - Length-constrained EDS\n";
            std::cout << "    <input_base>_l<N>.seds - Source tracking file\n\n";
            std::cout << "IMPLEMENTATION:\n";
            std::cout << "  Uses streaming approach - only reference sequence kept in memory.\n";
            std::cout << "  Efficient for large MSA files.\n\n";
            print_performance();
            return 0;
        }

        po::notify(vm);

        // Validate input file extension
        if (input_file.extension() != ".msa") {
            std::cerr << "Error: Input file must be an MSA file (.msa)\n";
            std::cerr << "Got: " << input_file << "\n";
            print_performance();
            return 1;
        }

        // Validate context length
        if (context_length < 0) {
            std::cerr << "Error: Context length must be >= 0\n";
            print_performance();
            return 1;
        }

        // Open input MSA file
        std::ifstream msa_in(input_file);
        if (!msa_in) {
            throw std::runtime_error("Failed to open input file: " + input_file.string());
        }

        // Determine transformation type
        bool create_leds = (context_length > 0);

        if (create_leds) {
            std::cout << "MSA → l-EDS transformation (l=" << context_length << ")\n";
        } else {
            std::cout << "MSA → EDS transformation\n";
        }
        std::cout << "  Input: " << input_file << "\n";

        // Determine output paths
        std::filesystem::path eds_path;
        std::filesystem::path seds_path;

        if (create_leds) {
            // l-EDS output with _l<N> suffix
            std::string base_name = input_file.stem().string();
            std::string suffix = "_l" + std::to_string(context_length);

            eds_path = output_file.empty()
                ? input_file.parent_path() / (base_name + suffix + ".leds")
                : output_file;

            seds_path = sources_file.empty()
                ? eds_path.parent_path() / (base_name + suffix + ".seds")
                : sources_file;
        } else {
            // Regular EDS output
            eds_path = output_file.empty()
                ? input_file.parent_path() / (input_file.stem().string() + ".eds")
                : output_file;

            seds_path = sources_file.empty()
                ? eds_path.parent_path() / (eds_path.stem().string() + ".seds")
                : sources_file;
        }

        // Open output streams
        std::ofstream eds_out(eds_path);
        if (!eds_out) {
            throw std::runtime_error("Failed to open output file: " + eds_path.string());
        }

        std::ofstream seds_out(seds_path);
        if (!seds_out) {
            throw std::runtime_error("Failed to open sources file: " + seds_path.string());
        }

        // Wrap MSA input so the progress bar can track bytes consumed.
        edsparser::CountingStreambuf msa_cbuf(msa_in.rdbuf());
        std::istream msa_stream(&msa_cbuf);
        size_t msa_file_size = std::filesystem::file_size(input_file);

        {
            edsparser::ProgressBar pb("MSA", msa_file_size, msa_cbuf);
            if (create_leds) {
                edsparser::parse_msa_to_leds_streaming(msa_stream, eds_out, seds_out, context_length);
            } else {
                edsparser::parse_msa_to_eds_streaming(msa_stream, eds_out, seds_out);
            }
        }

        // Close files
        msa_in.close();
        eds_out.close();
        seds_out.close();

        std::cout << "Transformation complete!\n";
        std::cout << "  Output: " << eds_path << "\n";
        std::cout << "  Sources: " << seds_path << "\n";

        print_performance();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        print_performance();
        return 1;
    }
}
