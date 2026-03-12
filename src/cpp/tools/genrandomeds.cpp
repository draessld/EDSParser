#include "formats/eds.hpp"
#include "common.hpp"
#include <boost/program_options.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#include <algorithm>
#include <set>
#include <vector>
#include <map>

namespace po = boost::program_options;
using namespace edsparser;

/**
 * Metadata for tracking degenerate symbols during generation
 */
struct SymbolMetadata {
    bool is_degenerate;
    size_t num_alternatives;

    SymbolMetadata() : is_degenerate(false), num_alternatives(1) {}
    SymbolMetadata(bool deg, size_t num_alts)
        : is_degenerate(deg), num_alternatives(num_alts) {}
};

/**
 * Generate a random DNA sequence of given length from alphabet
 */
std::string generate_random_sequence(size_t length, const std::string& alphabet, std::mt19937& gen) {
    std::uniform_int_distribution<> dist(0, alphabet.size() - 1);
    std::string seq;
    seq.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        seq += alphabet[dist(gen)];
    }
    return seq;
}

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
        return base; // Fallback if alphabet has only one character
    }
    std::uniform_int_distribution<> dist(0, alternatives.size() - 1);
    return alternatives[dist(gen)];
}

/**
 * Generate random variant positions with optional minimum context spacing
 */
std::vector<size_t> generate_variant_positions(
    size_t total_length,
    size_t num_variants,
    size_t min_context,
    std::mt19937& gen
) {
    std::vector<size_t> positions;

    if (num_variants == 0) {
        return positions;
    }

    if (min_context == 0) {
        // No spacing constraint - randomly sample positions
        std::set<size_t> unique_positions;
        std::uniform_int_distribution<size_t> dist(0, total_length - 1);

        while (unique_positions.size() < num_variants && unique_positions.size() < total_length) {
            unique_positions.insert(dist(gen));
        }

        positions.assign(unique_positions.begin(), unique_positions.end());
    } else {
        // With spacing constraint - ensure min_context between variants
        size_t max_possible_variants = total_length / (min_context + 1);
        size_t actual_variants = std::min(num_variants, max_possible_variants);

        if (actual_variants < num_variants) {
            std::cerr << "Warning: Can only fit " << actual_variants
                      << " variants with min-context=" << min_context
                      << " (requested: " << num_variants << ")\n";
        }

        // Divide sequence into segments and place one variant per segment
        size_t segment_size = total_length / actual_variants;
        std::uniform_int_distribution<size_t> offset_dist(0, std::min(segment_size - 1, min_context));

        for (size_t i = 0; i < actual_variants; ++i) {
            size_t base_pos = i * segment_size + min_context;
            if (base_pos < total_length) {
                size_t offset = offset_dist(gen);
                size_t pos = std::min(base_pos + offset, total_length - 1);
                positions.push_back(pos);
            }
        }
    }

    std::sort(positions.begin(), positions.end());
    return positions;
}

/**
 * Build source sets with realistic haplotype-style phasing.
 *
 * For non-degenerate (context) symbols: all paths (every path traverses context).
 * For degenerate symbols: each path is assigned to exactly ONE alternative,
 * distributed round-robin across alternatives. This mirrors real phased data
 * where each haplotype follows one specific variant.
 *
 * This ensures linear merge is efficient: only paths sharing the same
 * alternative at both positions produce valid combinations (no combinatorial
 * explosion from all-paths-in-every-alternative).
 *
 * Returns a vector of sets where sources[string_idx] = set of path IDs
 */
std::vector<std::set<int>> build_source_sets(
    const std::vector<SymbolMetadata>& symbols,
    size_t num_paths
) {
    std::vector<std::set<int>> sources;

    std::set<int> all_paths;
    for (size_t path_id = 1; path_id <= num_paths; ++path_id) {
        all_paths.insert(static_cast<int>(path_id));
    }

    for (const auto& symbol : symbols) {
        if (!symbol.is_degenerate) {
            // Context block: all paths traverse this symbol
            sources.push_back(all_paths);
        } else {
            // Degenerate symbol: assign each path to exactly one alternative
            // Round-robin distribution: path p goes to alternative (p-1) % num_alternatives
            size_t num_alts = symbol.num_alternatives;
            std::vector<std::set<int>> alt_sources(num_alts);
            for (size_t path_id = 1; path_id <= num_paths; ++path_id) {
                size_t alt_idx = (path_id - 1) % num_alts;
                alt_sources[alt_idx].insert(static_cast<int>(path_id));
            }
            for (size_t alt_idx = 0; alt_idx < num_alts; ++alt_idx) {
                sources.push_back(alt_sources[alt_idx]);
            }
        }
    }

    return sources;
}

/**
 * Write sources to .seds file
 */
void write_seds_file(
    const std::filesystem::path& seds_path,
    const std::vector<std::set<int>>& sources
) {
    std::ofstream outfile(seds_path);
    if (!outfile) {
        throw std::runtime_error("Cannot open sources file: " + seds_path.string());
    }

    for (const auto& source_set : sources) {
        outfile << "{";
        bool first = true;
        for (int path_id : source_set) {
            if (!first) {
                outfile << ",";
            }
            outfile << path_id;
            first = false;
        }
        outfile << "}";
    }

    outfile.close();
}

/**
 * Generate EDS with random variants and track metadata for source generation
 */
std::pair<std::string, std::vector<SymbolMetadata>> generate_random_eds_with_metadata(
    size_t ref_size_mb,
    double variability,
    size_t min_alternatives,
    size_t max_alternatives,
    size_t variant_length_max,
    double snp_ratio,
    const std::string& alphabet,
    size_t min_context,
    unsigned seed,
    size_t& num_paths
) {
    std::mt19937 gen(seed);
    std::ostringstream eds;
    std::vector<SymbolMetadata> symbols;

    // Calculate total length in base pairs
    size_t total_bp = ref_size_mb * 1000000;

    // Calculate number of variant sites
    size_t num_variants = static_cast<size_t>(total_bp * variability);

    // Calculate number of paths: ensure enough to cover all alternatives
    num_paths = std::max(max_alternatives, size_t(3));

    std::cerr << "Generating random EDS:\n";
    std::cerr << "  Reference size: " << ref_size_mb << " MB (" << total_bp << " bp)\n";
    std::cerr << "  Variability: " << (variability * 100) << "%\n";
    std::cerr << "  Number of variant sites: " << num_variants << "\n";
    std::cerr << "  Alternatives per variant: [" << min_alternatives << ", " << max_alternatives << "]\n";
    std::cerr << "  Max variant length: " << variant_length_max << " bp\n";
    std::cerr << "  SNP ratio: " << (snp_ratio * 100) << "%\n";
    std::cerr << "  Number of paths (samples): " << num_paths << "\n";
    if (min_context > 0) {
        std::cerr << "  Minimum context: " << min_context << " bp (l-EDS mode)\n";
    }

    // Generate reference sequence
    std::cerr << "Generating reference sequence...\n";
    std::string reference = generate_random_sequence(total_bp, alphabet, gen);

    // Generate variant positions
    std::cerr << "Placing variant sites...\n";
    std::vector<size_t> variant_positions = generate_variant_positions(
        total_bp, num_variants, min_context, gen
    );

    std::cerr << "Building EDS with " << variant_positions.size() << " variant sites...\n";

    // Distributions
    std::uniform_int_distribution<size_t> num_alt_dist(min_alternatives, max_alternatives);
    std::uniform_int_distribution<size_t> indel_length_dist(1, variant_length_max);
    std::uniform_real_distribution<double> variant_type_dist(0.0, 1.0);
    std::uniform_real_distribution<double> indel_type_dist(0.0, 1.0);

    size_t pos = 0;
    size_t var_idx = 0;
    size_t progress_interval = std::max(size_t(1), total_bp / 100); // Report every 1%
    size_t last_progress = 0;

    while (pos < total_bp) {
        // Progress reporting
        if (pos - last_progress >= progress_interval) {
            double percent = (100.0 * pos) / total_bp;
            std::cerr << "  Progress: " << std::fixed << std::setprecision(1)
                      << percent << "%\r" << std::flush;
            last_progress = pos;
        }

        // Check if current position is a variant site
        if (var_idx < variant_positions.size() && pos == variant_positions[var_idx]) {
            // Generate degenerate symbol
            size_t num_alternatives = num_alt_dist(gen);

            // Track metadata
            symbols.emplace_back(true, num_alternatives);

            eds << "{";

            // First alternative is always the reference base
            char ref_base = reference[pos];
            eds << ref_base;

            // Generate additional alternatives
            for (size_t alt_idx = 1; alt_idx < num_alternatives; ++alt_idx) {
                eds << ",";

                double type_rand = variant_type_dist(gen);

                if (type_rand < snp_ratio) {
                    // SNP: different base
                    char alt_base = get_different_base(ref_base, alphabet, gen);
                    eds << alt_base;
                } else {
                    // Indel
                    double indel_rand = indel_type_dist(gen);

                    if (indel_rand < 0.5) {
                        // Insertion: reference + extra bases
                        eds << ref_base;
                        size_t ins_length = indel_length_dist(gen);
                        eds << generate_random_sequence(ins_length, alphabet, gen);
                    } else {
                        // Deletion: empty string
                        // Leave empty (nothing between commas)
                    }
                }
            }

            eds << "}";
            pos++;
            var_idx++;
        } else {
            // Generate non-degenerate symbol (context block)
            size_t next_variant_pos = (var_idx < variant_positions.size())
                                       ? variant_positions[var_idx]
                                       : total_bp;
            size_t block_length = next_variant_pos - pos;

            // Track metadata
            symbols.emplace_back(false, 1);

            eds << "{" << reference.substr(pos, block_length) << "}";
            pos += block_length;
        }
    }

    std::cerr << "  Progress: 100.0%\n";
    std::cerr << "EDS generation complete\n";

    return {eds.str(), symbols};
}

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
             "Reference size in megabytes (1 MB = 1,000,000 bp)")
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

        // Validate parameters
        if (ref_size_mb == 0) {
            std::cerr << "Error: Reference size must be greater than 0 MB\n";
            print_performance();
            return 1;
        }

        if (variability < 0.0 || variability > 1.0) {
            std::cerr << "Error: Variability must be between 0.0 and 1.0\n";
            print_performance();
            return 1;
        }

        if (min_alternatives < 2) {
            std::cerr << "Error: Minimum alternatives must be at least 2\n";
            print_performance();
            return 1;
        }

        if (max_alternatives < min_alternatives) {
            std::cerr << "Error: Maximum alternatives must be >= minimum alternatives\n";
            print_performance();
            return 1;
        }

        if (variant_length_max == 0) {
            std::cerr << "Error: Variant length max must be greater than 0\n";
            print_performance();
            return 1;
        }

        if (snp_ratio < 0.0 || snp_ratio > 1.0) {
            std::cerr << "Error: SNP ratio must be between 0.0 and 1.0\n";
            print_performance();
            return 1;
        }

        if (alphabet.empty()) {
            std::cerr << "Error: Alphabet cannot be empty\n";
            print_performance();
            return 1;
        }

        // Generate random EDS with metadata
        size_t num_paths = 0;
        auto [eds_string, symbols] = generate_random_eds_with_metadata(
            ref_size_mb,
            variability,
            min_alternatives,
            max_alternatives,
            variant_length_max,
            snp_ratio,
            alphabet,
            min_context,
            seed,
            num_paths
        );

        // Build source sets (all paths on every string)
        std::cerr << "Generating source paths...\n";
        auto sources = build_source_sets(symbols, num_paths);

        // Write EDS file
        std::cerr << "Writing to file: " << output_file << "\n";
        std::ofstream outfile(output_file);
        if (!outfile) {
            std::cerr << "Error: Cannot open output file: " << output_file << "\n";
            print_performance();
            return 1;
        }
        outfile << eds_string;
        outfile.close();

        // Write sources file (.seds)
        std::filesystem::path seds_path = output_file;
        seds_path.replace_extension(".seds");

        std::cerr << "Writing sources to file: " << seds_path << "\n";
        write_seds_file(seds_path, sources);

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
