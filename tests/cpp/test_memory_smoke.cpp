#include "formats/eds.hpp"
#include "memory_monitor.hpp"
#include "common.hpp"
#include <iostream>
#include <filesystem>
#include <iomanip>

using namespace edsparser;

/**
 * Quick smoke test for memory validation (~1-2 minutes)
 * Tests only loading and source streaming - skips slow transformations
 */

int main(int argc, char** argv) {
    std::cout << "EDSParser Memory Smoke Test (Quick Validation)\n";
    std::cout << "============================================\n\n";

    std::filesystem::path data_dir = "tests/stress/data";
    if (argc > 1) {
        data_dir = argv[1];
    }

    if (!std::filesystem::exists(data_dir)) {
        std::cerr << "ERROR: Test data directory not found: " << data_dir << "\n";
        return 1;
    }

    double max_memory_mb = 2000.0;
    std::vector<size_t> test_sizes = {10, 50};
    
    std::cout << "Testing file sizes: 10MB, 50MB\n";
    std::cout << "Memory limit: " << max_memory_mb << " MB\n\n";

    int passed = 0, failed = 0;

    for (size_t size_mb : test_sizes) {
        std::cout << "\n" << std::string(60, '=') << "\n";
        std::cout << "TEST: " << size_mb << "MB File\n";
        std::cout << std::string(60, '=') << "\n";

        auto eds_path = data_dir / ("test_" + std::to_string(size_mb) + "MB.eds");
        auto seds_path = data_dir / ("test_" + std::to_string(size_mb) + "MB.seds");

        if (!std::filesystem::exists(eds_path)) {
            std::cout << "SKIP: File not found\n";
            continue;
        }

        try {
            Timer timer;
            timer.start();

            // Load EDS with sources
            EDS eds = EDS::load(eds_path, seds_path);
            
            std::cout << "  Loaded: " << eds.length() << " symbols, "
                      << eds.cardinality() << " strings\n";

            // Access some symbols
            for (size_t i = 0; i < std::min(size_t(100), eds.length()); i += 10) {
                auto symbol = eds.read_symbol(i);
            }

            // Access some sources
            for (size_t i = 0; i < std::min(size_t(1000), eds.cardinality()); ++i) {
                auto sources = eds.read_source(i);
            }

            timer.stop();
            double memory = get_peak_memory_mb();

            std::cout << "  Memory: " << std::fixed << std::setprecision(1) 
                      << memory << " MB\n";
            std::cout << "  Runtime: " << std::fixed << std::setprecision(2) 
                      << timer.elapsed_seconds() << "s\n";

            if (memory > max_memory_mb) {
                std::cout << "  Result: FAIL (exceeded limit)\n";
                failed++;
            } else {
                std::cout << "  Result: PASS ✓\n";
                passed++;
            }

        } catch (const std::exception& e) {
            std::cout << "  ERROR: " << e.what() << "\n";
            failed++;
        }
    }

    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "SUMMARY: " << passed << " passed, " << failed << " failed\n";
    std::cout << std::string(60, '=') << "\n";

    return (failed == 0) ? 0 : 1;
}
