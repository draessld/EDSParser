// Implementation of the shared utilities declared in common.hpp: the wall-clock
// timer and the peak-RSS reading behind every tool's [Performance] line.
#include "common.hpp"
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

namespace edsparser {

// Timer implementation
struct Timer::Impl {
    std::chrono::high_resolution_clock::time_point start_time;
    std::chrono::high_resolution_clock::time_point stop_time;
    bool running = false;
};

Timer::Timer() : impl_(std::make_unique<Impl>()) {}

Timer::~Timer() = default;

void Timer::start() {
    impl_->start_time = std::chrono::high_resolution_clock::now();
    impl_->running = true;
}

void Timer::stop() {
    impl_->stop_time = std::chrono::high_resolution_clock::now();
    impl_->running = false;
}

double Timer::elapsed_seconds() const {
    auto end = impl_->running ? std::chrono::high_resolution_clock::now() : impl_->stop_time;
    return std::chrono::duration<double>(end - impl_->start_time).count();
}

// Peak RSS comes from /proc rather than from in-process accounting: it must
// include allocations made by every library in the address space, and it must
// survive a peak that has already been freed by the time we report it.
double get_peak_memory_mb() {
#ifdef __linux__
    // Read from /proc/self/status
    std::ifstream status_file("/proc/self/status");
    if (!status_file.is_open()) {
        return 0.0;
    }

    std::string line;
    while (std::getline(status_file, line)) {
        // Look for VmHWM (peak resident set size) or VmPeak (peak virtual memory)
        // VmHWM is more accurate for actual memory usage
        if (line.substr(0, 6) == "VmHWM:") {
            std::istringstream iss(line);
            std::string label;
            double value;
            std::string unit;

            iss >> label >> value >> unit;

            // Value is typically in kB, convert to MB
            if (unit == "kB") {
                return value / 1024.0;
            }
            return value;  // Assume MB if no unit or different unit
        }
    }

    return 0.0;  // Not found
#else
    // Non-Linux platform
    return 0.0;
#endif
}

} // namespace edsparser
