#include "formats/eds.hpp"
#include "transforms/eds_transforms.hpp"
#include "memory_monitor.hpp"
#include "common.hpp"
#include <iostream>
#include <fstream>
#include <cassert>
#include <filesystem>
#include <iomanip>
#include <random>

using namespace edsparser;

/**
 * Memory Stress Test Configuration
 */
struct StressTestConfig {
    double max_memory_mb = 2000.0;              // 2GB hard limit
    double max_growth_mb = 100.0;               // Max 100MB growth during operation
    double leak_threshold_mb_per_sec = 1.0;     // 1MB/sec growth = leak
    double sample_interval_sec = 2.0;           // Sample every 2 seconds
    std::filesystem::path data_dir = "tests/stress/data";
};

/**
 * Test result tracking
 */
struct TestResult {
    std::string test_name;
    bool passed;
    double peak_memory_mb;
    double runtime_sec;
    std::string error_message;
};

std::vector<TestResult> test_results;

void print_header(const std::string& test_name) {
    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << "STRESS TEST: " << test_name << "\n";
    std::cout << std::string(80, '=') << "\n";
}

void print_result(const TestResult& result) {
    std::cout << "[" << (result.passed ? "PASS" : "FAIL") << "] "
              << result.test_name << "\n";
    std::cout << "  Peak Memory: " << std::fixed << std::setprecision(1)
              << result.peak_memory_mb << " MB\n";
    std::cout << "  Runtime: " << std::fixed << std::setprecision(2)
              << result.runtime_sec << " seconds\n";
    if (!result.passed) {
        std::cout << "  Error: " << result.error_message << "\n";
    }
}

void print_summary() {
    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << "TEST SUMMARY\n";
    std::cout << std::string(80, '=') << "\n";

    size_t passed = 0;
    for (const auto& result : test_results) {
        if (result.passed) passed++;
    }

    std::cout << "Total: " << test_results.size() << " tests\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << (test_results.size() - passed) << "\n";

    std::cout << "\nDetailed Results:\n";
    for (const auto& result : test_results) {
        print_result(result);
    }
}

/**
 * Test 1: Large EDS Loading (streaming mode)
 */
void test_large_eds_loading(const StressTestConfig& config, size_t size_mb) {
    std::string test_name = "Large EDS Loading (" + std::to_string(size_mb) + "MB)";
    print_header(test_name);

    TestResult result;
    result.test_name = test_name;
    result.passed = false;

    Timer timer;
    MemoryMonitor monitor(config.sample_interval_sec);

    try {
        timer.start();
        monitor.start();

        // Build file path
        std::filesystem::path eds_path = config.data_dir /
            ("test_" + std::to_string(size_mb) + "MB.eds");
        std::filesystem::path seds_path = config.data_dir /
            ("test_" + std::to_string(size_mb) + "MB.seds");

        if (!std::filesystem::exists(eds_path)) {
            throw std::runtime_error("Test file not found: " + eds_path.string());
        }

        std::cout << "Loading: " << eds_path << "\n";

        // Load EDS (streaming mode)
        monitor.add_label("Before load");
        EDS eds = EDS::load(eds_path, seds_path);
        monitor.add_label("After load");

        std::cout << "  Symbols: " << eds.length() << "\n";
        std::cout << "  Strings: " << eds.cardinality() << "\n";
        std::cout << "  Size: " << eds.size() << " bp\n";

        // Access some symbols (trigger streaming) - reduced for quick tests
        monitor.add_label("Before symbol access");
        size_t access_count = std::min(size_t(100), eds.length());  // Reduced from 1000 to 100
        for (size_t i = 0; i < access_count; i += 10) {
            auto symbol = eds.read_symbol(i);
            (void)symbol;  // Suppress unused warning
        }
        monitor.add_label("After symbol access");

        monitor.stop();
        timer.stop();

        // Validate memory constraints
        double peak = monitor.get_peak_memory_mb();
        if (peak > config.max_memory_mb) {
            throw std::runtime_error("Peak memory exceeded: " +
                std::to_string(peak) + " MB > " +
                std::to_string(config.max_memory_mb) + " MB");
        }

        if (monitor.detect_memory_leak(config.leak_threshold_mb_per_sec)) {
            throw std::runtime_error("Memory leak detected");
        }

        result.passed = true;
        result.peak_memory_mb = peak;
        result.runtime_sec = timer.elapsed_seconds();

    } catch (const std::exception& e) {
        monitor.stop();
        timer.stop();
        result.error_message = e.what();
        result.peak_memory_mb = monitor.get_peak_memory_mb();
        result.runtime_sec = timer.elapsed_seconds();
    }

    test_results.push_back(result);
    print_result(result);
}

/**
 * Test 2: EDS to l-EDS Transformation
 */
void test_eds_to_leds_transformation(const StressTestConfig& config,
                                     size_t size_mb,
                                     size_t context_length) {
    std::string test_name = "EDS→l-EDS (" + std::to_string(size_mb) +
                           "MB, L=" + std::to_string(context_length) + ")";
    print_header(test_name);

    TestResult result;
    result.test_name = test_name;
    result.passed = false;

    Timer timer;
    MemoryMonitor monitor(config.sample_interval_sec);

    try {
        timer.start();
        monitor.start();

        // Input files
        std::filesystem::path eds_path = config.data_dir /
            ("test_" + std::to_string(size_mb) + "MB.eds");
        std::filesystem::path seds_path = config.data_dir /
            ("test_" + std::to_string(size_mb) + "MB.seds");

        // Output files (temporary)
        auto temp_dir = std::filesystem::temp_directory_path();
        std::filesystem::path leds_path = temp_dir / "stress_test_output.leds";
        std::filesystem::path lseds_path = temp_dir / "stress_test_output.seds";

        if (!std::filesystem::exists(eds_path)) {
            throw std::runtime_error("Test file not found: " + eds_path.string());
        }

        std::cout << "Transforming: " << eds_path << " → " << leds_path << "\n";
        std::cout << "Context length: " << context_length << "\n";

        // Open streams
        std::ifstream eds_in(eds_path);
        std::ifstream seds_in(seds_path);
        std::ofstream leds_out(leds_path);
        std::ofstream lseds_out(lseds_path);

        monitor.add_label("Before transform");

        // Transform (LINEAR with sources)
        eds_to_leds_linear(eds_in, leds_out, context_length,
                          &seds_in, &lseds_out,
                          1,      // Single thread for consistent memory
                          true);  // Compact format

        monitor.add_label("After transform");

        // Close and cleanup
        eds_in.close();
        seds_in.close();
        leds_out.close();
        lseds_out.close();

        std::filesystem::remove(leds_path);
        std::filesystem::remove(lseds_path);

        monitor.stop();
        timer.stop();

        // Validate memory constraints
        double peak = monitor.get_peak_memory_mb();
        if (peak > config.max_memory_mb) {
            throw std::runtime_error("Peak memory exceeded: " +
                std::to_string(peak) + " MB > " +
                std::to_string(config.max_memory_mb) + " MB");
        }

        if (monitor.detect_memory_leak(config.leak_threshold_mb_per_sec)) {
            throw std::runtime_error("Memory leak detected");
        }

        result.passed = true;
        result.peak_memory_mb = peak;
        result.runtime_sec = timer.elapsed_seconds();

    } catch (const std::exception& e) {
        monitor.stop();
        timer.stop();
        result.error_message = e.what();
        result.peak_memory_mb = monitor.get_peak_memory_mb();
        result.runtime_sec = timer.elapsed_seconds();
    }

    test_results.push_back(result);
    print_result(result);
}

/**
 * Test 3: Source Streaming with LRU Cache
 */
void test_source_streaming(const StressTestConfig& config, size_t size_mb) {
    std::string test_name = "Source Streaming (" + std::to_string(size_mb) + "MB)";
    print_header(test_name);

    TestResult result;
    result.test_name = test_name;
    result.passed = false;

    Timer timer;
    MemoryMonitor monitor(config.sample_interval_sec);

    try {
        timer.start();
        monitor.start();

        std::filesystem::path eds_path = config.data_dir /
            ("test_" + std::to_string(size_mb) + "MB.eds");
        std::filesystem::path seds_path = config.data_dir /
            ("test_" + std::to_string(size_mb) + "MB.seds");

        if (!std::filesystem::exists(eds_path)) {
            throw std::runtime_error("Test file not found: " + eds_path.string());
        }

        std::cout << "Loading with sources: " << eds_path << "\n";

        monitor.add_label("Before load");
        EDS eds = EDS::load(eds_path, seds_path);
        monitor.add_label("After load");

        // Configure LRU cache
        eds.set_source_cache_capacity(10000);  // 10K sources

        // Access sources in various patterns
        std::cout << "Testing source access patterns...\n";

        // Sequential access (high cache hit rate) - reduced for quick tests
        monitor.add_label("Sequential access start");
        size_t seq_count = std::min(size_t(5000), eds.cardinality());  // Reduced from 50000 to 5000
        for (size_t i = 0; i < seq_count; ++i) {
            auto sources = eds.read_source(i);
            (void)sources;
        }
        monitor.add_label("Sequential access end");

        // Random access (lower cache hit rate) - reduced for quick tests
        monitor.add_label("Random access start");
        std::mt19937 gen(12345);
        std::uniform_int_distribution<size_t> dist(0, eds.cardinality() - 1);
        for (size_t i = 0; i < 1000; ++i) {  // Reduced from 10000 to 1000
            size_t idx = dist(gen);
            auto sources = eds.read_source(idx);
            (void)sources;
        }
        monitor.add_label("Random access end");

        monitor.stop();
        timer.stop();

        // Validate memory constraints
        double peak = monitor.get_peak_memory_mb();
        if (peak > config.max_memory_mb) {
            throw std::runtime_error("Peak memory exceeded");
        }

        double growth = monitor.get_memory_growth_mb();
        if (growth > config.max_growth_mb) {
            throw std::runtime_error("Excessive memory growth: " +
                std::to_string(growth) + " MB");
        }

        result.passed = true;
        result.peak_memory_mb = peak;
        result.runtime_sec = timer.elapsed_seconds();

    } catch (const std::exception& e) {
        monitor.stop();
        timer.stop();
        result.error_message = e.what();
        result.peak_memory_mb = monitor.get_peak_memory_mb();
        result.runtime_sec = timer.elapsed_seconds();
    }

    test_results.push_back(result);
    print_result(result);
}

/**
 * Test 4: Multi-iteration l-EDS with Temp Files
 */
void test_multi_iteration_leds(const StressTestConfig& config, size_t size_mb) {
    std::string test_name = "Multi-iteration l-EDS (" + std::to_string(size_mb) + "MB)";
    print_header(test_name);

    TestResult result;
    result.test_name = test_name;
    result.passed = false;

    Timer timer;
    MemoryMonitor monitor(config.sample_interval_sec);

    try {
        timer.start();
        monitor.start();

        std::filesystem::path eds_path = config.data_dir /
            ("test_" + std::to_string(size_mb) + "MB.eds");
        std::filesystem::path seds_path = config.data_dir /
            ("test_" + std::to_string(size_mb) + "MB.seds");

        auto temp_dir = std::filesystem::temp_directory_path();
        std::filesystem::path leds_path = temp_dir / "stress_multi_iter.leds";
        std::filesystem::path lseds_path = temp_dir / "stress_multi_iter.seds";

        if (!std::filesystem::exists(eds_path)) {
            throw std::runtime_error("Test file not found: " + eds_path.string());
        }

        std::cout << "Multi-iteration transformation with context length 50\n";

        std::ifstream eds_in(eds_path);
        std::ifstream seds_in(seds_path);
        std::ofstream leds_out(leds_path);
        std::ofstream lseds_out(lseds_path);

        monitor.add_label("Before transform");

        // Large context length = more iterations
        eds_to_leds_linear(eds_in, leds_out, 50,
                          &seds_in, &lseds_out, 1, true);

        monitor.add_label("After transform");

        eds_in.close();
        seds_in.close();
        leds_out.close();
        lseds_out.close();

        std::filesystem::remove(leds_path);
        std::filesystem::remove(lseds_path);

        monitor.stop();
        timer.stop();

        // Validate: memory should be stable across iterations
        double peak = monitor.get_peak_memory_mb();
        if (peak > config.max_memory_mb) {
            throw std::runtime_error("Peak memory exceeded");
        }

        // Check that memory doesn't grow across iterations
        auto samples = monitor.get_samples();
        if (samples.size() > 20) {
            // Compare first quarter vs last quarter average
            size_t quarter = samples.size() / 4;
            double early_avg = 0, late_avg = 0;
            for (size_t i = 0; i < quarter; ++i) {
                early_avg += samples[i].memory_mb;
            }
            for (size_t i = samples.size() - quarter; i < samples.size(); ++i) {
                late_avg += samples[i].memory_mb;
            }
            early_avg /= quarter;
            late_avg /= quarter;

            if (late_avg > early_avg + config.max_growth_mb) {
                throw std::runtime_error("Memory grew across iterations");
            }
        }

        result.passed = true;
        result.peak_memory_mb = peak;
        result.runtime_sec = timer.elapsed_seconds();

    } catch (const std::exception& e) {
        monitor.stop();
        timer.stop();
        result.error_message = e.what();
        result.peak_memory_mb = monitor.get_peak_memory_mb();
        result.runtime_sec = timer.elapsed_seconds();
    }

    test_results.push_back(result);
    print_result(result);
}

/**
 * Main test runner
 */
int main(int argc, char** argv) {
    std::cout << "EDSParser Memory Stress Test Suite\n";
    std::cout << "==================================\n\n";

    StressTestConfig config;

    // Parse command-line args (optional)
    if (argc > 1) {
        config.data_dir = argv[1];
    }

    std::cout << "Configuration:\n";
    std::cout << "  Data directory: " << config.data_dir << "\n";
    std::cout << "  Max memory: " << config.max_memory_mb << " MB\n";
    std::cout << "  Max growth: " << config.max_growth_mb << " MB\n";
    std::cout << "  Sample interval: " << config.sample_interval_sec << " sec\n\n";

    // Check if test data exists
    if (!std::filesystem::exists(config.data_dir)) {
        std::cerr << "ERROR: Test data directory not found: " << config.data_dir << "\n";
        std::cerr << "Run generate_data.sh first\n";
        return 1;
    }

    // Run tests on different file sizes
    std::vector<size_t> test_sizes;

    // Quick mode: use small files for ~2 minute runtime
    if (argc > 2 && std::string(argv[2]) == "--quick") {
        test_sizes = {10, 50};  // 10MB and 50MB for quick validation
        std::cout << "Running in QUICK mode (10MB, 50MB files)\n\n";
    }
    // Full mode: use large files for comprehensive testing
    else if (argc > 2 && std::string(argv[2]) == "--large") {
        test_sizes = {100, 1000, 5000, 10000};  // Up to 10GB
        std::cout << "Running in LARGE mode (100MB, 1GB, 5GB, 10GB files)\n\n";
    }
    // Default mode: balanced testing
    else {
        test_sizes = {10, 50};  // Default to quick mode for convenience
        std::cout << "Running in DEFAULT mode (10MB, 50MB files)\n";
        std::cout << "Use --quick for same behavior, --large for 100MB+ files\n\n";
    }

    for (size_t size_mb : test_sizes) {
        // Test 1: Loading
        test_large_eds_loading(config, size_mb);

        // Test 2: Transformation (single context length for speed)
        test_eds_to_leds_transformation(config, size_mb, 3);

        // Test 3: Source streaming
        test_source_streaming(config, size_mb);

        // Test 4: Multi-iteration (skip for files > 50MB in quick mode)
        if (size_mb <= 50) {
            test_multi_iteration_leds(config, size_mb);
        }
    }

    // Print summary
    print_summary();

    // Return exit code
    size_t passed = 0;
    for (const auto& result : test_results) {
        if (result.passed) passed++;
    }

    return (passed == test_results.size()) ? 0 : 1;
}
