#include "memory_monitor.hpp"
#include "common.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <stdexcept>
#include <sstream>

namespace edsparser {

MemoryMonitor::MemoryMonitor(double sample_interval_sec)
    : sample_interval_sec_(sample_interval_sec)
    , running_(false) {
}

MemoryMonitor::~MemoryMonitor() {
    if (running_) {
        stop();
    }
}

void MemoryMonitor::start() {
    if (running_) {
        throw std::runtime_error("MemoryMonitor already running");
    }

    samples_.clear();
    running_ = true;
    start_time_ = std::chrono::high_resolution_clock::now();
    sampling_thread_ = std::thread(&MemoryMonitor::sampling_loop, this);
}

void MemoryMonitor::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    if (sampling_thread_.joinable()) {
        sampling_thread_.join();
    }
}

void MemoryMonitor::add_label(const std::string& label) {
    auto now = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_time_).count();
    double memory = get_peak_memory_mb();

    std::lock_guard<std::mutex> lock(samples_mutex_);
    samples_.push_back({elapsed, memory, label});
}

void MemoryMonitor::sampling_loop() {
    while (running_) {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();
        double memory = get_peak_memory_mb();

        {
            std::lock_guard<std::mutex> lock(samples_mutex_);
            samples_.push_back({elapsed, memory, ""});
        }

        // Sleep for interval
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(sample_interval_sec_ * 1000))
        );
    }
}

std::vector<MemorySample> MemoryMonitor::get_samples() const {
    std::lock_guard<std::mutex> lock(samples_mutex_);
    return samples_;
}

double MemoryMonitor::get_peak_memory_mb() const {
    std::lock_guard<std::mutex> lock(samples_mutex_);
    if (samples_.empty()) {
        return 0.0;
    }

    auto max_it = std::max_element(samples_.begin(), samples_.end(),
        [](const MemorySample& a, const MemorySample& b) {
            return a.memory_mb < b.memory_mb;
        });

    return max_it->memory_mb;
}

double MemoryMonitor::get_average_memory_mb() const {
    std::lock_guard<std::mutex> lock(samples_mutex_);
    if (samples_.empty()) {
        return 0.0;
    }

    double sum = std::accumulate(samples_.begin(), samples_.end(), 0.0,
        [](double acc, const MemorySample& s) {
            return acc + s.memory_mb;
        });

    return sum / samples_.size();
}

double MemoryMonitor::get_memory_growth_mb() const {
    std::lock_guard<std::mutex> lock(samples_mutex_);
    if (samples_.size() < 2) {
        return 0.0;
    }

    return samples_.back().memory_mb - samples_.front().memory_mb;
}

bool MemoryMonitor::detect_memory_leak(double threshold_mb_per_sec) const {
    std::lock_guard<std::mutex> lock(samples_mutex_);
    if (samples_.size() < 10) {
        return false;  // Not enough samples for reliable detection
    }

    // Simple linear regression: y = mx + b
    // m = slope (MB/sec), if m > threshold, likely a leak

    double n = static_cast<double>(samples_.size());
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;

    for (const auto& sample : samples_) {
        sum_x += sample.timestamp_sec;
        sum_y += sample.memory_mb;
        sum_xy += sample.timestamp_sec * sample.memory_mb;
        sum_xx += sample.timestamp_sec * sample.timestamp_sec;
    }

    double slope = (n * sum_xy - sum_x * sum_y) / (n * sum_xx - sum_x * sum_x);
    return slope > threshold_mb_per_sec;
}

void assert_memory_below(double max_mb, const std::string& context) {
    double current = get_peak_memory_mb();
    if (current > max_mb) {
        std::ostringstream oss;
        oss << "Memory assertion failed in " << context << ": "
            << current << " MB > " << max_mb << " MB";
        throw std::runtime_error(oss.str());
    }
}

void assert_no_memory_growth(const MemoryMonitor& monitor,
                             double max_growth_mb,
                             const std::string& context) {
    double growth = monitor.get_memory_growth_mb();
    if (growth > max_growth_mb) {
        std::ostringstream oss;
        oss << "Memory growth assertion failed in " << context << ": "
            << growth << " MB > " << max_growth_mb << " MB";
        throw std::runtime_error(oss.str());
    }
}

} // namespace edsparser
