#include "formats/eds.hpp"
#include "common.hpp"
#include <boost/program_options.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <random>
#include <algorithm>

namespace po = boost::program_options;
using namespace edsparser;

/**
 * Get a random character from alphabet different from the given character
 */
char get_different_base(char base, const std::string& alphabet, std::mt19937& gen) {
    std::string alternatives;
    for (char c : alphabet) {
        if (c != base) {
            alternatives += c;
        }
    }
    if (alternatives.empty()) {
        return base;
    }
    std::uniform_int_distribution<> dist(0, alternatives.size() - 1);
    return alternatives[dist(gen)];
}

/**
 * Stream-generate EDS and SEDS with O(1) memory.
 *
 * Two independent PRNGs:
 *   ref_gen(seed)              — reference character sampling only
 *   var_gen(seed ^ 0x9e3779b9) — all variant-structure decisions
 *
 * For min_context==0: per-position Bernoulli trial (expected count = total_bp * variability).
 * For min_context>0:  one variant per segment, O(1) memory (no position vector).
 *
 * Writes EDS symbols and SEDS entries incrementally as each position is decided.
 */
void generate_and_stream_eds(
    size_t ref_size_mb,
    double variability,
    size_t min_alternatives,
    size_t max_alternatives,
    size_t variant_length_max,
    double snp_ratio,
    const std::string& alphabet,
    size_t min_context,
    unsigned seed,
    size_t& num_paths,
    std::ostream& eds_out,
    std::ostream& seds_out
) {
    std::mt19937 ref_gen(seed);
    std::mt19937 var_gen(seed ^ 0x9e3779b9u);

    const size_t total_bp = ref_size_mb * 1000000;
    const size_t num_variants = static_cast<size_t>(total_bp * variability);
    num_paths = std::max(max_alternatives, size_t(3));

    std::uniform_int_distribution<> ref_char_dist(0, alphabet.size() - 1);
    std::uniform_int_distribution<size_t> num_alt_dist(min_alternatives, max_alternatives);
    std::uniform_int_distribution<size_t> indel_length_dist(1, variant_length_max);
    std::uniform_real_distribution<double> variant_type_dist(0.0, 1.0);
    std::uniform_real_distribution<double> indel_type_dist(0.0, 1.0);
    std::uniform_real_distribution<double> bernoulli_dist(0.0, 1.0);

    // Precompute all-paths SEDS entry (written once per context block)
    std::string all_paths_seds;
    {
        all_paths_seds.reserve(2 + num_paths * 3);
        all_paths_seds += '{';
        for (size_t p = 1; p <= num_paths; ++p) {
            if (p > 1) all_paths_seds += ',';
            all_paths_seds += std::to_string(p);
        }
        all_paths_seds += '}';
    }

    std::cerr << "Generating random EDS (streaming, O(1) memory):\n";
    std::cerr << "  Reference size: " << ref_size_mb << " MB (" << total_bp << " bp)\n";
    std::cerr << "  Variability: " << (variability * 100) << "%\n";
    std::cerr << "  Expected variant sites: " << num_variants << "\n";
    std::cerr << "  Alternatives per variant: [" << min_alternatives << ", " << max_alternatives << "]\n";
    std::cerr << "  Max variant length: " << variant_length_max << " bp\n";
    std::cerr << "  SNP ratio: " << (snp_ratio * 100) << "%\n";
    std::cerr << "  Number of paths (samples): " << num_paths << "\n";
    if (min_context > 0) {
        std::cerr << "  Minimum context: " << min_context << " bp (l-EDS mode)\n";
    }

    // Helper: write a context block [from, to) and its SEDS entry
    auto write_context_block = [&](size_t len) {
        if (len == 0) return;
        eds_out << '{';
        for (size_t i = 0; i < len; ++i)
            eds_out << alphabet[ref_char_dist(ref_gen)];
        eds_out << '}';
        seds_out << all_paths_seds;
    };

    // Helper: write a degenerate symbol and its SEDS round-robin entries.
    // ref_char must already be generated (and consumed from ref_gen).
    auto write_variant = [&](char ref_char) {
        const size_t num_alts = num_alt_dist(var_gen);

        eds_out << '{' << ref_char;
        for (size_t alt = 1; alt < num_alts; ++alt) {
            eds_out << ',';
            if (variant_type_dist(var_gen) < snp_ratio) {
                eds_out << get_different_base(ref_char, alphabet, var_gen);
            } else {
                if (indel_type_dist(var_gen) < 0.5) {
                    // insertion: ref base + extra chars
                    eds_out << ref_char;
                    const size_t ins_len = indel_length_dist(var_gen);
                    for (size_t i = 0; i < ins_len; ++i)
                        eds_out << alphabet[ref_char_dist(ref_gen)];
                }
                // else deletion: empty string between commas
            }
        }
        eds_out << '}';

        // SEDS: round-robin assignment — path p → alternative (p-1) % num_alts
        for (size_t a = 0; a < num_alts; ++a) {
            seds_out << '{';
            bool first = true;
            for (size_t p = a + 1; p <= num_paths; p += num_alts) {
                if (!first) seds_out << ',';
                seds_out << p;
                first = false;
            }
            seds_out << '}';
        }
    };

    const size_t progress_interval = std::max(size_t(1), total_bp / 100);
    size_t last_progress = 0;

    auto report_progress = [&](size_t pos) {
        if (pos - last_progress >= progress_interval) {
            std::cerr << "  Progress: " << std::fixed << std::setprecision(1)
                      << (100.0 * pos / total_bp) << "%\r" << std::flush;
            last_progress = pos;
        }
    };

    if (min_context == 0) {
        // --- Case A: no spacing constraint; per-position Bernoulli ---
        size_t pos = 0;
        size_t context_len = 0;

        while (pos < total_bp) {
            report_progress(pos);

            if (bernoulli_dist(var_gen) < variability) {
                write_context_block(context_len);
                context_len = 0;
                write_variant(alphabet[ref_char_dist(ref_gen)]);
            } else {
                // Advance ref_gen for this context character later (in write_context_block)
                ++context_len;
            }
            ++pos;
        }
        write_context_block(context_len);

    } else {
        // --- Case B: min_context spacing; segment-based, one variant per segment ---
        if (num_variants == 0) {
            write_context_block(total_bp);
            std::cerr << "  Progress: 100.0%\n";
            return;
        }

        const size_t max_possible = total_bp / (min_context + 1);
        const size_t actual_variants = std::min(num_variants, max_possible);

        if (actual_variants < num_variants) {
            std::cerr << "Warning: Can only fit " << actual_variants
                      << " variants with min-context=" << min_context
                      << " (requested: " << num_variants << ")\n";
        }

        const size_t segment_size = total_bp / actual_variants;
        std::uniform_int_distribution<size_t> offset_dist(
            0, std::min(segment_size - 1, min_context));

        size_t pos = 0;

        for (size_t seg = 0; seg < actual_variants; ++seg) {
            report_progress(pos);

            const size_t base_pos = seg * segment_size + min_context;
            if (base_pos >= total_bp) break;

            const size_t var_pos = std::min(base_pos + offset_dist(var_gen), total_bp - 1);

            // Context from current pos up to var_pos
            write_context_block(var_pos - pos);
            write_variant(alphabet[ref_char_dist(ref_gen)]);
            pos = var_pos + 1;
        }

        // Trailing context
        write_context_block(total_bp - pos);
    }

    std::cerr << "  Progress: 100.0%\n";
    std::cerr << "EDS generation complete\n";
}

int main(int argc, char** argv) {
    Timer timer;
    timer.start();

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
        std::filesystem::path output_file;
        size_t ref_size_mb;
        double variability;
        size_t min_alternatives;
        size_t max_alternatives;
        size_t variant_length_max;
        double snp_ratio;
        std::string alphabet;
        size_t min_context;
        unsigned seed;

        po::options_description desc("Generate random EDS file with controlled variability");
        desc.add_options()
            ("help,h", "Show help message")
            ("output,o", po::value<std::filesystem::path>(&output_file)->required(),
             "Output EDS file (.eds or .leds)")
            ("ref-size-mb", po::value<size_t>(&ref_size_mb)->required(),
             "Reference size in megabases (1 MB = 1,000,000 bp)")
            ("variability,v", po::value<double>(&variability)->default_value(0.10),
             "Fraction of positions with variants (e.g., 0.10 = 10%)")
            ("min-alternatives", po::value<size_t>(&min_alternatives)->default_value(2),
             "Minimum number of strings per degenerate symbol")
            ("max-alternatives", po::value<size_t>(&max_alternatives)->default_value(4),
             "Maximum number of strings per degenerate symbol")
            ("variant-length-max", po::value<size_t>(&variant_length_max)->default_value(10),
             "Maximum length of indel variants in bp")
            ("snp-ratio", po::value<double>(&snp_ratio)->default_value(0.7),
             "Fraction of variants that are SNPs (rest are indels)")
            ("alphabet", po::value<std::string>(&alphabet)->default_value("ACGT"),
             "Character alphabet for sequence generation")
            ("min-context", po::value<size_t>(&min_context)->default_value(0),
             "Minimum context length between variants (for l-EDS compliance, 0 = disabled)")
            ("seed", po::value<unsigned>(&seed)->default_value(std::random_device{}()),
             "Random seed for reproducibility");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            std::cout << "\nExample usage:\n";
            std::cout << "  genrandomeds --ref-size-mb 100 --variability 0.10 -o random.eds\n";
            std::cout << "  genrandomeds --ref-size-mb 50 --variability 0.05 --min-context 50 -o random.leds\n";
            std::cout << "\nNote: Sources file (.seds) is automatically generated alongside the EDS file.\n";
            print_performance();
            return 0;
        }

        po::notify(vm);

        if (ref_size_mb == 0) {
            std::cerr << "Error: Reference size must be greater than 0 MB\n";
            print_performance(); return 1;
        }
        if (variability < 0.0 || variability > 1.0) {
            std::cerr << "Error: Variability must be between 0.0 and 1.0\n";
            print_performance(); return 1;
        }
        if (min_alternatives < 2) {
            std::cerr << "Error: Minimum alternatives must be at least 2\n";
            print_performance(); return 1;
        }
        if (max_alternatives < min_alternatives) {
            std::cerr << "Error: Maximum alternatives must be >= minimum alternatives\n";
            print_performance(); return 1;
        }
        if (variant_length_max == 0) {
            std::cerr << "Error: Variant length max must be greater than 0\n";
            print_performance(); return 1;
        }
        if (snp_ratio < 0.0 || snp_ratio > 1.0) {
            std::cerr << "Error: SNP ratio must be between 0.0 and 1.0\n";
            print_performance(); return 1;
        }
        if (alphabet.empty()) {
            std::cerr << "Error: Alphabet cannot be empty\n";
            print_performance(); return 1;
        }

        std::filesystem::path seds_path = output_file;
        seds_path.replace_extension(".seds");

        std::ofstream eds_file(output_file);
        if (!eds_file) {
            std::cerr << "Error: Cannot open output file: " << output_file << "\n";
            print_performance(); return 1;
        }

        std::ofstream seds_file(seds_path);
        if (!seds_file) {
            std::cerr << "Error: Cannot open sources file: " << seds_path << "\n";
            print_performance(); return 1;
        }

        size_t num_paths = 0;
        generate_and_stream_eds(
            ref_size_mb, variability,
            min_alternatives, max_alternatives,
            variant_length_max, snp_ratio,
            alphabet, min_context,
            seed, num_paths,
            eds_file, seds_file);

        eds_file.close();
        seds_file.close();

        std::cerr << "Successfully generated random EDS with sources\n";
        std::cerr << "Output written to: " << output_file << "\n";
        std::cerr << "Sources written to: " << seds_path << "\n";

        print_performance();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        print_performance();
        return 1;
    }
}
