#include "transforms/vcf_transforms.hpp"
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
        std::filesystem::path reference_file;
        std::filesystem::path output_file;
        std::filesystem::path sources_file;
        Length context_length;

        po::options_description desc("Transform VCF (Variant Call Format) to EDS/l-EDS");
        desc.add_options()
            ("help,h", "Show help message")
            ("input,i", po::value<std::filesystem::path>(&input_file)->required(), "Input VCF file (.vcf)")
            ("reference,r", po::value<std::filesystem::path>(&reference_file)->required(), "Reference FASTA file")
            ("output,o", po::value<std::filesystem::path>(&output_file), "Output EDS file (default: <input>.eds)")
            ("sources,s", po::value<std::filesystem::path>(&sources_file), "Output source file (default: <output>.seds)")
            ("context-length,l", po::value<Length>(&context_length)->default_value(0), "Create l-EDS with minimum context length (0 = regular EDS)");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << "vcf2eds - Transform VCF (Variant Call Format) to EDS\n\n";
            std::cout << desc << "\n";
            std::cout << "DESCRIPTION:\n";
            std::cout << "  Transforms a VCF file with a reference FASTA to an Elastic-Degenerate\n";
            std::cout << "  String (EDS) with sample-level source tracking. Each sample in the VCF\n";
            std::cout << "  is tracked as a separate path in the source file.\n\n";
            std::cout << "SUPPORTED VARIANTS:\n";
            std::cout << "  - SNPs (single nucleotide polymorphisms)\n";
            std::cout << "  - Small indels (insertions and deletions)\n";
            std::cout << "  - Simple deletions (<DEL>)\n";
            std::cout << "  - Simple insertions (<INS>)\n";
            std::cout << "  - Multi-allelic sites (multiple ALT alleles)\n\n";
            std::cout << "SKIPPED WITH WARNINGS:\n";
            std::cout << "  - Overlapping variants\n";
            std::cout << "  - Complex structural variants (<INV>, <CN*>, mobile elements)\n";
            std::cout << "  - Malformed VCF lines\n\n";
            std::cout << "SOURCE TRACKING:\n";
            std::cout << "  Sample-level tracking: Each sample contributes to one path.\n";
            std::cout << "  Path IDs are 1-indexed, matching sample order in VCF.\n";
            std::cout << "  Diploid samples contribute to a single path.\n\n";
            std::cout << "EXAMPLES:\n";
            std::cout << "  # Basic transformation (VCF → EDS):\n";
            std::cout << "  vcf2eds -i variants.vcf -r reference.fa\n";
            std::cout << "  # Creates: variants.eds and variants.seds\n\n";
            std::cout << "  # Two-stage transformation (VCF → EDS → l-EDS):\n";
            std::cout << "  vcf2eds -i variants.vcf -r reference.fa -l 5\n";
            std::cout << "  # Creates: variants_l5.leds and variants_l5.seds\n\n";
            std::cout << "  # Custom output paths:\n";
            std::cout << "  vcf2eds -i variants.vcf -r reference.fa -o output.eds -s output.seds\n\n";
            std::cout << "OUTPUT:\n";
            std::cout << "  Regular EDS:\n";
            std::cout << "    <input_base>.eds   - EDS file\n";
            std::cout << "    <input_base>.seds  - Source tracking file (sample-level)\n\n";
            std::cout << "  l-EDS (with -l):\n";
            std::cout << "    <input_base>_l<N>.leds - Length-constrained EDS\n";
            std::cout << "    <input_base>_l<N>.seds - Source tracking file\n\n";
            std::cout << "WHY TWO-STAGE FOR l-EDS:\n";
            std::cout << "  VCF represents sparse variants on a reference. Unlike MSA (which has\n";
            std::cout << "  full alignment), VCF doesn't provide a global view of common vs variant\n";
            std::cout << "  regions. The two-stage pipeline is the optimal approach:\n";
            std::cout << "    1. VCF→EDS: Handle VCF-specific complexity (overlaps, multi-allelic)\n";
            std::cout << "    2. EDS→l-EDS: Apply context-length constraint\n";
            std::cout << "  This provides better code reuse, testability, and performance.\n\n";
            std::cout << "IMPLEMENTATION:\n";
            std::cout << "  Uses streaming approach for FASTA reference.\n";
            std::cout << "  Only active regions loaded into memory.\n\n";
            print_performance();
            return 0;
        }

        po::notify(vm);

        // Validate input file extension
        if (input_file.extension() != ".vcf") {
            std::cerr << "Error: Input file must be a VCF file (.vcf)\n";
            std::cerr << "Got: " << input_file << "\n";
            print_performance();
            return 1;
        }

        // Validate reference file exists
        if (!std::filesystem::exists(reference_file)) {
            std::cerr << "Error: Reference FASTA file not found: " << reference_file << "\n";
            print_performance();
            return 1;
        }

        // Open VCF file
        std::ifstream vcf_in(input_file);
        if (!vcf_in) {
            throw std::runtime_error("Failed to open VCF file: " + input_file.string());
        }

        // Open FASTA reference file
        std::ifstream fasta_in(reference_file);
        if (!fasta_in) {
            throw std::runtime_error("Failed to open reference FASTA file: " + reference_file.string());
        }

        // Determine transformation type
        bool create_leds = (context_length > 0);

        if (create_leds) {
            std::cout << "VCF → l-EDS transformation (l=" << context_length << ")\n";
            std::cout << "  Using two-stage pipeline: VCF→EDS→l-EDS\n";
        } else {
            std::cout << "VCF → EDS transformation\n";
        }
        std::cout << "  Input: " << input_file << "\n";
        std::cout << "  Reference: " << reference_file << "\n";

        // Perform transformation with statistics tracking
        edsparser::VCFStats stats;
        std::string eds_str, seds_str;
        if (create_leds) {
            auto result = edsparser::parse_vcf_to_leds_streaming(vcf_in, fasta_in, context_length, &stats);
            eds_str = result.first;
            seds_str = result.second;
        } else {
            auto result = edsparser::parse_vcf_to_eds_streaming(vcf_in, fasta_in, &stats);
            eds_str = result.first;
            seds_str = result.second;
        }
        vcf_in.close();
        fasta_in.close();

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

        // Write EDS output
        std::ofstream eds_out(eds_path);
        if (!eds_out) {
            throw std::runtime_error("Failed to open output file: " + eds_path.string());
        }
        eds_out << eds_str;
        eds_out.close();

        // Write sources output
        std::ofstream seds_out(seds_path);
        if (!seds_out) {
            throw std::runtime_error("Failed to open sources file: " + seds_path.string());
        }
        seds_out << seds_str;
        seds_out.close();

        std::cout << "Transformation complete!\n";
        std::cout << "  Output: " << eds_path << "\n";
        std::cout << "  Sources: " << seds_path << "\n";
        std::cout << "\n";

        // Print variant processing statistics
        std::cout << "Variant Processing Statistics:\n";
        std::cout << "  Total variants read:        " << stats.total_variants << "\n";
        std::cout << "  Successfully processed:     " << stats.processed_variants << "\n";
        std::cout << "  Skipped (malformed):        " << stats.skipped_malformed << "\n";
        std::cout << "  Skipped (unsupported SV):   " << stats.skipped_unsupported_sv << "\n";
        std::cout << "  Total skipped:              " << stats.total_skipped() << "\n";
        std::cout << "  Variant groups created:     " << stats.variant_groups << "\n";

        if (stats.total_variants > 0) {
            double success_rate = (100.0 * stats.processed_variants) / stats.total_variants;
            std::cout << "  Success rate:               " << std::fixed << std::setprecision(1)
                      << success_rate << "%\n";
        }
        std::cout << "\n";

        print_performance();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        print_performance();
        return 1;
    }
}
