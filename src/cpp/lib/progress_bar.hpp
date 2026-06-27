#ifndef EDSPARSER_PROGRESS_BAR_HPP
#define EDSPARSER_PROGRESS_BAR_HPP

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <streambuf>
#include <string>
#include <thread>
#include <unistd.h>

namespace edsparser {

// Transparent byte-counting wrapper for any std::streambuf.
// Intercepts underflow() to count every byte delivered through the stream.
// count() is safe to call from a background thread (relaxed atomic).
class CountingStreambuf : public std::streambuf {
public:
    explicit CountingStreambuf(std::streambuf* src) : src_(src), bytes_(0) {}

    size_t count() const { return bytes_.load(std::memory_order_relaxed); }

protected:
    int_type underflow() override {
        std::streamsize n = src_->sgetn(buf_, sizeof(buf_));
        if (n <= 0) return traits_type::eof();
        bytes_.fetch_add(static_cast<size_t>(n), std::memory_order_relaxed);
        setg(buf_, buf_, buf_ + n);
        return traits_type::to_int_type(buf_[0]);
    }

private:
    std::streambuf* src_;
    std::atomic<size_t> bytes_;
    char buf_[65536];
};

// Animated progress bar written to stderr.
// Only renders when stderr is a terminal (isatty check); silent when piped.
// Overwrites a single line with \r every 250 ms from a background thread.
// Destructor stops the thread and prints the final 100% line + newline.
//
// Typical use:
//   CountingStreambuf cbuf(input.rdbuf());
//   std::istream cs(&cbuf);
//   {
//       ProgressBar pb("Reading", file_size, cbuf);
//       do_work(cs);
//   }  // prints 100% and '\n'
class ProgressBar {
public:
    ProgressBar(const char* label, size_t total_bytes, const CountingStreambuf& src)
        : label_(label), total_(total_bytes), src_(src),
          tty_(isatty(STDERR_FILENO)),
          start_(std::chrono::steady_clock::now()),
          running_(true),
          thread_([this] { loop(); }) {}

    ~ProgressBar() {
        running_.store(false, std::memory_order_relaxed);
        thread_.join();
        if (tty_) {
            render(src_.count());
            std::fputc('\n', stderr);
        }
    }

    // Non-copyable, non-movable
    ProgressBar(const ProgressBar&) = delete;
    ProgressBar& operator=(const ProgressBar&) = delete;

private:
    static std::string fmt_bytes(size_t b) {
        char buf[32];
        if (b >= (size_t)1 << 30)
            std::snprintf(buf, sizeof(buf), "%.1f GB", b / double(1ULL << 30));
        else if (b >= (size_t)1 << 20)
            std::snprintf(buf, sizeof(buf), "%.0f MB", b / double(1 << 20));
        else
            std::snprintf(buf, sizeof(buf), "%.0f KB", b / double(1 << 10));
        return buf;
    }

    static std::string fmt_speed(double Bps) {
        char buf[32];
        if (Bps >= double(1 << 20))
            std::snprintf(buf, sizeof(buf), "%.1f MB/s", Bps / (1 << 20));
        else
            std::snprintf(buf, sizeof(buf), "%.0f KB/s", Bps / (1 << 10));
        return buf;
    }

    static std::string fmt_eta(double s) {
        if (s <= 0 || s > 99.0 * 3600) return "?";
        char buf[16];
        if (s >= 3600)
            std::snprintf(buf, sizeof(buf), "%dh%02dm", (int)(s / 3600), (int)(s / 60) % 60);
        else if (s >= 60)
            std::snprintf(buf, sizeof(buf), "%dm%02ds", (int)(s / 60), (int)s % 60);
        else
            std::snprintf(buf, sizeof(buf), "%ds", (int)s);
        return buf;
    }

    void render(size_t cur) const {
        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_).count();
        double pct   = total_ > 0 ? std::min(100.0 * cur / total_, 100.0) : 0.0;
        double speed = elapsed > 0.1 ? cur / elapsed : 0.0;
        double eta   = (speed > 0 && cur < total_) ? (total_ - cur) / speed : 0.0;

        const int W = 35;
        char bar[W + 2];
        int fill = (int)(pct / 100.0 * W);
        for (int i = 0; i < W; i++)
            bar[i] = i < fill ? '=' : (i == fill ? '>' : ' ');
        bar[W] = '\0';

        std::fprintf(stderr, "\r%-10s [%s] %4.1f%%  %s / %s  %s  ETA %s  ",
                     label_, bar, pct,
                     fmt_bytes(cur).c_str(), fmt_bytes(total_).c_str(),
                     fmt_speed(speed).c_str(),
                     total_ > 0 ? fmt_eta(eta).c_str() : "--");
        std::fflush(stderr);
    }

    void loop() {
        while (running_.load(std::memory_order_relaxed)) {
            if (tty_) render(src_.count());
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    const char* label_;
    size_t total_;
    const CountingStreambuf& src_;
    bool tty_;
    std::chrono::steady_clock::time_point start_;
    std::atomic<bool> running_;
    std::thread thread_;
};

} // namespace edsparser

#endif // EDSPARSER_PROGRESS_BAR_HPP
