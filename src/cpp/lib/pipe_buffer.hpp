#ifndef EDSPARSER_PIPE_BUFFER_HPP
#define EDSPARSER_PIPE_BUFFER_HPP

#include <streambuf>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace edsparser {

/**
 * Thread-safe circular buffer implementing std::streambuf.
 *
 * Allows one producer thread to write data via overflow()/xsputn()
 * while another consumer thread reads via underflow()/xsgetn().
 *
 * Used to connect VCF→EDS output directly to EDS→l-EDS input
 * without intermediate temp files.
 */
class PipeStreamBuffer : public std::streambuf {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 64 * 1024 * 1024;  // 64MB

    explicit PipeStreamBuffer(size_t buffer_size = DEFAULT_BUFFER_SIZE)
        : buffer_(buffer_size)
        , read_pos_(0)
        , write_pos_(0)
        , data_size_(0)
        , eof_(false)
        , closed_(false)
    {
        // Set up put area (producer side)
        setp(nullptr, nullptr);  // We'll handle put directly

        // Set up get area (consumer side)
        setg(nullptr, nullptr, nullptr);  // We'll handle get directly
    }

    /**
     * Signal end of data from producer side.
     * Consumer will receive EOF after reading remaining data.
     */
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        eof_ = true;
        closed_ = true;
        not_empty_.notify_all();  // Wake up consumer if waiting
    }

    /**
     * Check if the buffer has been closed.
     */
    bool is_closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

protected:
    // ===== PRODUCER (OUTPUT) INTERFACE =====

    /**
     * Called when single character is written.
     */
    int_type overflow(int_type ch) override {
        if (ch == traits_type::eof()) {
            return traits_type::eof();
        }

        char c = traits_type::to_char_type(ch);

        std::unique_lock<std::mutex> lock(mutex_);

        // Wait for space in buffer
        not_full_.wait(lock, [this] {
            return data_size_ < buffer_.size() || closed_;
        });

        if (closed_ && !eof_) {
            return traits_type::eof();  // Error close
        }

        // Write character
        buffer_[write_pos_] = c;
        write_pos_ = (write_pos_ + 1) % buffer_.size();
        bool was_empty = (data_size_ == 0);
        ++data_size_;

        if (was_empty) {
            not_empty_.notify_one();  // Wake up consumer only on empty→nonempty
        }

        return ch;
    }

    /**
     * Called to write multiple characters (more efficient than repeated overflow).
     */
    std::streamsize xsputn(const char* s, std::streamsize count) override {
        std::streamsize written = 0;

        while (written < count) {
            std::unique_lock<std::mutex> lock(mutex_);

            // Wait for space in buffer
            not_full_.wait(lock, [this] {
                return data_size_ < buffer_.size() || closed_;
            });

            if (closed_ && !eof_) {
                return written;  // Error close
            }

            // Calculate how much we can write
            size_t available = buffer_.size() - data_size_;
            size_t to_write = std::min(static_cast<size_t>(count - written), available);

            // Write in up to two chunks (wrap around)
            size_t first_chunk = std::min(to_write, buffer_.size() - write_pos_);
            std::copy(s + written, s + written + first_chunk, buffer_.begin() + write_pos_);

            if (to_write > first_chunk) {
                // Wrap around - copy remaining to start of buffer
                std::copy(s + written + first_chunk, s + written + to_write, buffer_.begin());
            }

            write_pos_ = (write_pos_ + to_write) % buffer_.size();
            bool was_empty = (data_size_ == 0);
            data_size_ += to_write;
            written += to_write;

            if (was_empty) {
                not_empty_.notify_one();  // Wake up consumer only on empty→nonempty
            }
        }

        return written;
    }

    // ===== CONSUMER (INPUT) INTERFACE =====

    /**
     * Called when input buffer is empty and more data is needed.
     */
    int_type underflow() override {
        std::unique_lock<std::mutex> lock(mutex_);

        // Wait for data or EOF
        not_empty_.wait(lock, [this] {
            return data_size_ > 0 || eof_;
        });

        if (data_size_ == 0 && eof_) {
            return traits_type::eof();
        }

        // Return next character without consuming it
        return traits_type::to_int_type(buffer_[read_pos_]);
    }

    /**
     * Called to read and consume a single character.
     */
    int_type uflow() override {
        std::unique_lock<std::mutex> lock(mutex_);

        // Wait for data or EOF
        not_empty_.wait(lock, [this] {
            return data_size_ > 0 || eof_;
        });

        if (data_size_ == 0 && eof_) {
            return traits_type::eof();
        }

        // Read and consume character
        char c = buffer_[read_pos_];
        read_pos_ = (read_pos_ + 1) % buffer_.size();
        --data_size_;

        not_full_.notify_one();  // Wake up producer

        return traits_type::to_int_type(c);
    }

    /**
     * Called to read multiple characters (more efficient than repeated uflow).
     */
    std::streamsize xsgetn(char* s, std::streamsize count) override {
        std::streamsize read_total = 0;

        while (read_total < count) {
            std::unique_lock<std::mutex> lock(mutex_);

            // Wait for data or EOF
            not_empty_.wait(lock, [this] {
                return data_size_ > 0 || eof_;
            });

            if (data_size_ == 0 && eof_) {
                break;  // No more data
            }

            // Calculate how much we can read
            size_t to_read = std::min(static_cast<size_t>(count - read_total), data_size_);

            // Read in up to two chunks (wrap around)
            size_t first_chunk = std::min(to_read, buffer_.size() - read_pos_);
            std::copy(buffer_.begin() + read_pos_, buffer_.begin() + read_pos_ + first_chunk, s + read_total);

            if (to_read > first_chunk) {
                // Wrap around
                size_t second_chunk = to_read - first_chunk;
                std::copy(buffer_.begin(), buffer_.begin() + second_chunk, s + read_total + first_chunk);
            }

            read_pos_ = (read_pos_ + to_read) % buffer_.size();
            data_size_ -= to_read;
            read_total += to_read;

            not_full_.notify_one();  // Wake up producer
        }

        return read_total;
    }

    /**
     * Show available characters for reading.
     */
    std::streamsize showmanyc() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (data_size_ > 0) {
            return static_cast<std::streamsize>(data_size_);
        }
        return eof_ ? -1 : 0;
    }

private:
    std::vector<char> buffer_;
    size_t read_pos_;
    size_t write_pos_;
    size_t data_size_;  // Current data in buffer

    bool eof_;          // Producer signaled end of data
    bool closed_;       // Buffer is closed

    mutable std::mutex mutex_;
    std::condition_variable not_full_;   // Producer waits here
    std::condition_variable not_empty_;  // Consumer waits here
};

}  // namespace edsparser

#endif  // EDSPARSER_PIPE_BUFFER_HPP
