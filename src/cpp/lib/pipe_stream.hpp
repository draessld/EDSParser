#ifndef EDSPARSER_PIPE_STREAM_HPP
#define EDSPARSER_PIPE_STREAM_HPP

#include "pipe_buffer.hpp"
#include <iostream>
#include <memory>
#include <utility>

namespace edsparser {

/**
 * Output stream wrapper for PipeStreamBuffer.
 * Used by the producer thread to write data.
 */
class PipeOutputStream : public std::ostream {
public:
    explicit PipeOutputStream(std::shared_ptr<PipeStreamBuffer> buffer)
        : std::ostream(buffer.get())
        , buffer_(std::move(buffer))
    {}

    // Non-copyable
    PipeOutputStream(const PipeOutputStream&) = delete;
    PipeOutputStream& operator=(const PipeOutputStream&) = delete;

    // Movable
    PipeOutputStream(PipeOutputStream&& other) noexcept
        : std::ostream(std::move(other))
        , buffer_(std::move(other.buffer_))
    {
        rdbuf(buffer_.get());
    }

    PipeOutputStream& operator=(PipeOutputStream&& other) noexcept {
        if (this != &other) {
            std::ostream::operator=(std::move(other));
            buffer_ = std::move(other.buffer_);
            rdbuf(buffer_.get());
        }
        return *this;
    }

    /**
     * Signal end of data. Consumer will receive EOF after reading remaining data.
     * Must be called when producer is done writing.
     */
    void close() {
        if (buffer_) {
            flush();
            buffer_->close();
        }
    }

    /**
     * Get the underlying buffer.
     */
    std::shared_ptr<PipeStreamBuffer> buffer() const {
        return buffer_;
    }

private:
    std::shared_ptr<PipeStreamBuffer> buffer_;
};

/**
 * Input stream wrapper for PipeStreamBuffer.
 * Used by the consumer thread to read data.
 */
class PipeInputStream : public std::istream {
public:
    explicit PipeInputStream(std::shared_ptr<PipeStreamBuffer> buffer)
        : std::istream(buffer.get())
        , buffer_(std::move(buffer))
    {}

    // Non-copyable
    PipeInputStream(const PipeInputStream&) = delete;
    PipeInputStream& operator=(const PipeInputStream&) = delete;

    // Movable
    PipeInputStream(PipeInputStream&& other) noexcept
        : std::istream(std::move(other))
        , buffer_(std::move(other.buffer_))
    {
        rdbuf(buffer_.get());
    }

    PipeInputStream& operator=(PipeInputStream&& other) noexcept {
        if (this != &other) {
            std::istream::operator=(std::move(other));
            buffer_ = std::move(other.buffer_);
            rdbuf(buffer_.get());
        }
        return *this;
    }

    /**
     * Get the underlying buffer.
     */
    std::shared_ptr<PipeStreamBuffer> buffer() const {
        return buffer_;
    }

private:
    std::shared_ptr<PipeStreamBuffer> buffer_;
};

/**
 * Create a connected pair of pipe streams.
 *
 * @param buffer_size Size of the circular buffer (default 64MB)
 * @return Pair of (output stream, input stream) sharing the same buffer
 *
 * Usage:
 *   auto [out, in] = make_pipe();
 *
 *   // Producer thread writes to 'out'
 *   std::thread producer([&out] {
 *       out << "data";
 *       out.close();
 *   });
 *
 *   // Consumer thread reads from 'in'
 *   std::string line;
 *   while (std::getline(in, line)) {
 *       process(line);
 *   }
 *
 *   producer.join();
 */
inline std::pair<PipeOutputStream, PipeInputStream> make_pipe(
    size_t buffer_size = PipeStreamBuffer::DEFAULT_BUFFER_SIZE
) {
    auto buffer = std::make_shared<PipeStreamBuffer>(buffer_size);
    return {PipeOutputStream(buffer), PipeInputStream(buffer)};
}

}  // namespace edsparser

#endif  // EDSPARSER_PIPE_STREAM_HPP
