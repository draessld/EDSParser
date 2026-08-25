#include "formats/eds.hpp"
#include "formats/sources.hpp"
#include "common.hpp"
#include <boost/program_options.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <optional>

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
        std::filesystem::path sources_edz_file;
        size_t count;
        Length length;
        uint64_t seed = 0;
        bool no_sources = false;
        bool allow_duplicates = false;

        po::options_description desc("Generate random patterns from EDS");
        desc.add_options()
            ("help,h", "Show help message")
            ("version,V", "Print version and build provenance (COMMIT, COMMIT_DATE, DIRTY) and exit")
            ("input,i", po::value<std::filesystem::path>(&input_file)->required(), "Input EDS file")
            ("output,o", po::value<std::filesystem::path>(&output_file)->required(), "Output pattern file")
            ("count,n", po::value<size_t>(&count)->default_value(100), "Number of patterns")
            ("length,l", po::value<Length>(&length)->default_value(10), "Pattern length")
            ("seds,s", po::value<std::filesystem::path>(&sources_file),
                "Source file (.seds/.edz), format auto-detected. With sources, each "
                "pattern walks a single path, so it is a substring of a genome the "
                "panel actually contains. Strongly recommended for benchmarking.")
            ("edz,z", po::value<std::filesystem::path>(&sources_edz_file),
                "Source file explicitly treated as binary EDZ regardless of its "
                "extension (mutually exclusive with -s)")
            ("seed", po::value<uint64_t>(&seed),
                "PRNG seed, for a reproducible pattern set. Without it the seed is "
                "drawn from random_device and the output differs on every run.")
            ("ignore-sources", po::bool_switch(&no_sources),
                "Pick alternatives independently per symbol even when sources are "
                "given. This samples the cartesian language and can emit strings no "
                "genome carries — only for reproducing the old behaviour.")
            ("allow-duplicates", po::bool_switch(&allow_duplicates),
                "Keep repeated patterns instead of retrying for a distinct one. "
                "A repeat is the same query timed twice, not a second measurement, "
                "so this is only for regenerating a set produced before "
                "deduplication existed — and note the same seed still yields a "
                "different set, because rejecting a duplicate changes how much "
                "randomness is consumed.");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("version")) {
            edsparser::print_version("edsparser-genpatterns");
            return 0;
        }

        if (vm.count("help")) {
            std::cout << desc << "\n";
            print_performance();
            return 0;
        }

        po::notify(vm);

        // Validate input file exists
        if (!std::filesystem::exists(input_file)) {
            std::cerr << "Error: Input file does not exist: " << input_file << "\n";
            print_performance();
            return 1;
        }

        // Validate parameters
        if (count == 0) {
            std::cerr << "Error: Pattern count must be greater than 0\n";
            print_performance();
            return 1;
        }

        if (length == 0) {
            std::cerr << "Error: Pattern length must be greater than 0\n";
            print_performance();
            return 1;
        }

        if (vm.count("seds") && vm.count("edz")) {
            std::cerr << "Error: --seds/-s and --edz/-z are mutually exclusive\n";
            print_performance();
            return 1;
        }

        // Load EDS (always uses streaming mode), attaching sources when given.
        std::cerr << "Loading EDS file: " << input_file << "\n";
        EDS eds;
        if (vm.count("edz")) {
            // -z forces EDZ parsing regardless of extension. EDZ_COMPRESSED has a
            // different header layout that parse_edz() rejects, so peek at the
            // flags and pick the right parser rather than failing on a file the
            // user explicitly said is EDZ.
            if (!std::filesystem::exists(sources_edz_file)) {
                std::cerr << "Error: Source file does not exist: " << sources_edz_file << "\n";
                print_performance();
                return 1;
            }
            Sources::Format edz_variant = Sources::Format::EDZ;
            {
                std::ifstream probe(sources_edz_file, std::ios::binary);
                char magic[4] = {};
                uint16_t flags = 0;
                probe.read(magic, 4);
                probe.read(reinterpret_cast<char*>(&flags), sizeof(flags));
                if (probe && magic[0] == 'E' && magic[1] == 'D' &&
                    magic[2] == 'Z' && magic[3] == '\0' && (flags & 0x0001)) {
                    edz_variant = Sources::Format::EDZ_COMPRESSED;
                }
            }
            eds = EDS::load(input_file);
            auto sources = Sources::load(sources_edz_file, edz_variant);
            if (!eds.empty() && sources->cardinality() != eds.cardinality()) {
                std::cerr << "Error: Sources cardinality (" << sources->cardinality()
                          << ") does not match EDS cardinality (" << eds.cardinality() << ")\n";
                print_performance();
                return 1;
            }
            eds.set_sources_object(sources);
        } else if (vm.count("seds")) {
            if (!std::filesystem::exists(sources_file)) {
                std::cerr << "Error: Source file does not exist: " << sources_file << "\n";
                print_performance();
                return 1;
            }
            eds = EDS::load(input_file, sources_file);
        } else {
            eds = EDS::load(input_file);
        }

        if (eds.empty()) {
            std::cerr << "Error: Cannot generate patterns from empty EDS\n";
            print_performance();
            return 1;
        }

        std::cerr << "Loaded EDS with " << eds.length() << " symbols, "
                  << eds.cardinality() << " strings\n";

        // Check if pattern length is reasonable
        if (length > eds.size()) {
            std::cerr << "Warning: Pattern length (" << length
                      << ") is greater than total EDS size (" << eds.size() << ")\n";
            std::cerr << "Patterns may be truncated or generation may fail\n";
        }

        // Open output file
        std::ofstream outfile(output_file);
        if (!outfile) {
            std::cerr << "Error: Cannot open output file: " << output_file << "\n";
            print_performance();
            return 1;
        }

        // Generate patterns
        std::optional<uint64_t> seed_opt;
        if (vm.count("seed")) seed_opt = seed;

        std::cerr << "Generating " << count << " patterns of length " << length << "...\n";
        auto stats = eds.generate_patterns(outfile, count, length, seed_opt,
                                           !no_sources, !allow_duplicates);

        if (stats.source_aware) {
            std::cerr << "Mode: source-aware (each pattern walks one of "
                      << stats.num_paths << " paths)\n";
        } else if (eds.has_sources() && no_sources) {
            std::cerr << "Mode: cartesian (--ignore-sources) — patterns may not exist "
                         "in any single genome\n";
        } else {
            std::cerr << "Warning: no sources given, so alternatives are picked "
                         "independently per symbol.\n"
                         "  Generated patterns may be strings no genome carries, and "
                         "will go missing\n"
                         "  from a LINEAR-merged l-EDS as l grows. Pass -s/--seds for "
                         "benchmarking.\n";
        }

        if (stats.duplicates_discarded > 0) {
            std::cerr << "Discarded " << stats.duplicates_discarded
                      << " duplicate pattern(s) and retried for distinct ones\n";
        }

        if (stats.generated < stats.requested) {
            std::cerr << "Warning: generated " << stats.generated << " of "
                      << stats.requested << " requested patterns; the rest could not be "
                         "built\n  (pattern length may exceed what a single path spans "
                         "from a random start";
            if (stats.unique) {
                std::cerr << ", or the EDS holds fewer than "
                          << stats.requested << " distinct patterns of this length";
            }
            std::cerr << ")\n";
        }

        std::cerr << "Successfully generated " << stats.generated << " patterns\n";
        std::cerr << "Output written to: " << output_file << "\n";

        print_performance();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        print_performance();
        return 1;
    }
}
