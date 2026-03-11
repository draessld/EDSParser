#ifndef EDSPARSER_MEMORY_MONITOR_HPP
#define EDSPARSER_MEMORY_MONITOR_HPP

#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <string>

namespace edsparser {

/**
 * Memory sample point
 */
struct MemorySample {
    double timestamp_sec;      // Seconds since monitoring started
    double memory_mb;          // Memory usage in MB
    std::string label;         // Optional label for this sample
};

/**
 * Periodic memory monitor for detecting memory growth patterns
 *
 * Usage:
 *   MemoryMonitor monitor(1.0);  // Sample every 1 second
 *   monitor.start();
 *   // ... run operations ...
 *   monitor.stop();
 *   auto samples = monitor.get_samples();
 */
class MemoryMonitor {
public:
    explicit MemoryMonitor(double sample_interval_sec = 1.0);
    ~MemoryMonitor();

    // Control
    void start();
    void stop();
    void add_label(const std::string& label);  // Add labeled sample point

    // Analysis
    std::vector<MemorySample> get_samples() const;
    double get_peak_memory_mb() const;
    double get_average_memory_mb() const;
    double get_memory_growth_mb() const;  // Growth from first to last sample
    bool detect_memory_leak(double threshold_mb_per_sec = 1.0) const;  // Linear regression

private:
    void sampling_loop();

    double sample_interval_sec_;
    std::atomic<bool> running_;
    std::thread sampling_thread_;
    mutable std::mutex samples_mutex_;
    std::vector<MemorySample> samples_;
    std::chrono::high_resolution_clock::time_point start_time_;
};

/**
 * Memory assertion helpers for tests
 */
void assert_memory_below(double max_mb, const std::string& context);
void assert_no_memory_growth(const MemoryMonitor& monitor,
                             double max_growth_mb,
                             const std::string& context);

} // namespace edsparser

#endif // EDSPARSER_MEMORY_MONITOR_HPP
