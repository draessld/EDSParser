#include "formats/sources.hpp"
#include "common.hpp"
#include <boost/program_options.hpp>
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <string>
#include <algorithm>

namespace po = boost::program_options;
using namespace edsparser;

namespace {

// Map a lowercase format name to a Sources::Format. Throws on unknown names.
Sources::Format parse_format(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (name == "seds")            return Sources::Format::SEDS;
    if (name == "seds_sparse")     return Sources::Format::SEDS_SPARSE;
    if (name == "edz")             return Sources::Format::EDZ;
    if (name == "edz_sparse")      return Sources::Format::EDZ_SPARSE;
    if (name == "edz_compressed")  return Sources::Format::EDZ_COMPRESSED;
    throw std::runtime_error("Unknown source format '" + name +
        "' (expected: seds, seds_sparse, edz, edz_sparse, edz_compressed)");
}

const char* format_name(Sources::Format fmt) {
    switch (fmt) {
        case Sources::Format::SEDS:           return "seds";
        case Sources::Format::SEDS_SPARSE:    return "seds_sparse";
        case Sources::Format::EDZ:            return "edz";
        case Sources::Format::EDZ_SPARSE:     return "edz_sparse";
        case Sources::Format::EDZ_COMPRESSED: return "edz_compressed";
    }
    return "unknown";
}

// Base output format implied by an output file's extension (dense variant).
// Unknown extensions default to dense SEDS.
Sources::Format infer_output_format(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    if (ext == ".edz") return Sources::Format::EDZ;
    return Sources::Format::SEDS;
}

// Sparse counterpart of a dense format (identity if already sparse/compressed).
Sources::Format sparse_variant(Sources::Format fmt) {
    switch (fmt) {
        case Sources::Format::SEDS: return Sources::Format::SEDS_SPARSE;
        case Sources::Format::EDZ:  return Sources::Format::EDZ_SPARSE;
        default:                    return fmt;
    }
}

// Canonicalize a source set for cross-format comparison. The universal set has two
// equivalent spellings: the marker {0}, and the explicit full list {1,2,...,np}.
// The EDZ bitset format stores both as all-ones and always reads back {0}, so a
// SEDS entry that explicitly enumerates every path is equal-but-not-identical after
// a round-trip. Collapse both spellings to {0} before comparing.
PathSet canonicalize_universal(PathSet ps, size_t num_paths) {
    if (ps.size() == 1 && ps[0] == 0) return {0};
    if (num_paths > 0 && ps.size() == num_paths) {
        for (size_t k = 0; k < num_paths; ++k) {
            if (ps[k] != static_cast<int>(k + 1)) return ps;
        }
        return {0};
    }
    return ps;
}

}  // namespace

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
        std::filesystem::path input_file;
        std::filesystem::path output_file;
        std::string from_str;
        std::string to_str;
        bool sparse = false;
        bool verify = false;

        po::options_description desc(
            "Convert EDS source files between formats (SEDS <-> EDZ).\n"
            "Re-encodes an existing source file without re-running the EDS transform.");
        desc.add_options()
            ("help,h", "Show help message")
            ("input,i",  po::value<std::filesystem::path>(&input_file)->required(),
                "Input source file (.seds or .edz)")
            ("output,o", po::value<std::filesystem::path>(&output_file)->required(),
                "Output source file")
            ("from", po::value<std::string>(&from_str),
                "Input format: seds | seds_sparse | edz | edz_sparse (default: auto-detect)")
            ("to", po::value<std::string>(&to_str),
                "Output format: seds | seds_sparse | edz | edz_sparse "
                "(default: inferred from output extension)")
            ("sparse", po::bool_switch(&sparse),
                "Select the sparse variant of the output format (omit universal {0} entries)")
            ("verify", po::bool_switch(&verify),
                "Reload input and output and confirm every source set matches");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            std::cout << "Examples:\n"
                      << "  edsparser-source-transform -i in.seds -o out.edz\n"
                      << "  edsparser-source-transform -i in.edz  -o out.seds\n"
                      << "  edsparser-source-transform -i in.seds -o out.edz --sparse\n"
                      << "  edsparser-source-transform -i in.seds -o out.edz --verify\n";
            return 0;
        }
        po::notify(vm);

        if (!std::filesystem::exists(input_file)) {
            std::cerr << "Error: input file does not exist: " << input_file << "\n";
            return static_cast<int>(ErrorCode::FILE_NOT_FOUND);
        }

        // Resolve input format: explicit --from, else auto-detect by content/extension.
        Sources::Format in_fmt = vm.count("from")
            ? parse_format(from_str)
            : Sources::detect_format(input_file);

        // Resolve output format: explicit --to wins; otherwise infer from the output
        // extension and apply --sparse if requested.
        Sources::Format out_fmt;
        if (vm.count("to")) {
            out_fmt = parse_format(to_str);
            if (sparse) out_fmt = sparse_variant(out_fmt);
        } else {
            out_fmt = infer_output_format(output_file);
            if (sparse) out_fmt = sparse_variant(out_fmt);
        }

        if (out_fmt == Sources::Format::EDZ_COMPRESSED) {
            std::cerr << "Error: EDZ_COMPRESSED output is not yet implemented.\n";
            return static_cast<int>(ErrorCode::INVALID_PARAMETER);
        }

        std::cerr << "Converting " << input_file << " (" << format_name(in_fmt) << ") -> "
                  << output_file << " (" << format_name(out_fmt) << ")\n";

        auto sources = Sources::load(input_file, in_fmt);
        sources->save_as(output_file, out_fmt);

        std::cerr << "Wrote " << sources->cardinality() << " source sets to "
                  << output_file << "\n";

        if (verify) {
            std::cerr << "Verifying round-trip...\n";
            auto reloaded = Sources::load(output_file, out_fmt);
            if (reloaded->cardinality() != sources->cardinality()) {
                std::cerr << "Error: cardinality mismatch (input "
                          << sources->cardinality() << " vs output "
                          << reloaded->cardinality() << ")\n";
                print_performance();
                return static_cast<int>(ErrorCode::BUILD_FAILED);
            }
            // Reload the input independently so the comparison is against fresh reads.
            auto original = Sources::load(input_file, in_fmt);
            const size_t np_in  = original->num_paths();
            const size_t np_out = reloaded->num_paths();
            for (size_t i = 0; i < original->cardinality(); ++i) {
                PathSet a = canonicalize_universal(original->read_source(i), np_in);
                PathSet b = canonicalize_universal(reloaded->read_source(i), np_out);
                if (a != b) {
                    std::cerr << "Error: source set mismatch at index " << i << "\n";
                    print_performance();
                    return static_cast<int>(ErrorCode::BUILD_FAILED);
                }
            }
            std::cerr << "Verification passed: all " << original->cardinality()
                      << " source sets match.\n";
        }

        print_performance();
        return static_cast<int>(ErrorCode::SUCCESS);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        print_performance();
        return static_cast<int>(ErrorCode::UNKNOWN_ERROR);
    }
}
