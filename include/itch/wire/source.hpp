// Input sources, and the buffered reader that sits on top of them.
//
// Two shapes, because two access patterns:
//
//   MappedFile      the whole file is already addressable, so FrameCursor runs
//                   straight over it with no copying at all. This is what the
//                   benchmarks use: a decompressor inside the timing loop would
//                   be measuring the decompressor.
//
//   FrameReader<S>  owns a buffer and refills it from a byte source, moving the
//                   straddling tail to the front. This is what gzip and stdin
//                   use, where nothing is addressable up front.
//
// The two are checked against each other: the same input through both paths
// must produce an identical sequence of frames.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"
#include "itch/wire/framing.hpp"

namespace itch::wire {

// Fills dst with up to n bytes. Returns the number written; 0 means the input
// is finished.
template <class S>
concept ByteSource = requires(S& s, std::byte* dst, std::size_t n) {
    { s.read(dst, n) } -> std::same_as<std::size_t>;
};

// ---------------------------------------------------------------------------

class MappedFile {
public:
    MappedFile() noexcept = default;

    explicit MappedFile(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
        }
        struct ::stat st {};
        if (::fstat(fd, &st) != 0) {
            const std::string err = std::strerror(errno);
            ::close(fd);
            throw std::runtime_error("cannot stat " + path + ": " + err);
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ > 0) {
            void* addr = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (addr == MAP_FAILED) {
                const std::string err = std::strerror(errno);
                ::close(fd);
                throw std::runtime_error("cannot mmap " + path + ": " + err);
            }
            data_ = static_cast<std::byte*>(addr);
            // The whole point of this path is one forward pass.
            ::madvise(addr, size_, MADV_SEQUENTIAL);
        }
        ::close(fd);
    }

    ~MappedFile() { unmap(); }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept
        : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) {
            unmap();
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        return std::span<const std::byte>(data_, size_);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    void unmap() noexcept {
        if (data_ != nullptr) {
            ::munmap(data_, size_);
            data_ = nullptr;
            size_ = 0;
        }
    }

    std::byte*  data_ = nullptr;
    std::size_t size_ = 0;
};

// ---------------------------------------------------------------------------

// Hands out bytes from memory. Used by tests to drive the buffered path over
// the same input the zero-copy path sees.
class MemorySource {
public:
    explicit MemorySource(std::span<const std::byte> data, std::size_t chunk = 0) noexcept
        : data_(data), chunk_(chunk) {}

    std::size_t read(std::byte* dst, std::size_t n) noexcept {
        if (chunk_ != 0 && n > chunk_) {
            n = chunk_;  // forces the straddle path to be exercised
        }
        const std::size_t left = data_.size() - pos_;
        const std::size_t take = (n < left) ? n : left;
        if (take != 0) {
            std::memcpy(dst, data_.data() + pos_, take);
            pos_ += take;
        }
        return take;
    }

private:
    std::span<const std::byte> data_;
    std::size_t                chunk_ = 0;
    std::size_t                pos_ = 0;
};

// ---------------------------------------------------------------------------

class FdSource {
public:
    explicit FdSource(int fd) noexcept : fd_(fd) {}

    explicit FdSource(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) {
            throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
        }
        owned_ = true;
    }

    ~FdSource() {
        if (owned_ && fd_ >= 0) {
            ::close(fd_);
        }
    }

    FdSource(const FdSource&) = delete;
    FdSource& operator=(const FdSource&) = delete;

    FdSource(FdSource&& other) noexcept : fd_(other.fd_), owned_(other.owned_) {
        other.fd_ = -1;
        other.owned_ = false;
    }

    FdSource& operator=(FdSource&& other) noexcept {
        if (this != &other) {
            if (owned_ && fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = other.fd_;
            owned_ = other.owned_;
            other.fd_ = -1;
            other.owned_ = false;
        }
        return *this;
    }

    std::size_t read(std::byte* dst, std::size_t n) {
        std::size_t total = 0;
        while (total < n) {
            const ::ssize_t got = ::read(fd_, dst + total, n - total);
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
            }
            if (got == 0) {
                break;
            }
            total += static_cast<std::size_t>(got);
        }
        return total;
    }

private:
    int  fd_ = -1;
    bool owned_ = false;
};

// ---------------------------------------------------------------------------

// Reads framed messages out of a byte source.
//
// The buffer must hold at least one whole framed message, otherwise a message
// straddling the end could never be completed. That is asserted, not assumed.
template <ByteSource Source>
class FrameReader {
public:
    static constexpr std::size_t kDefaultCapacity = 1u << 20;  // 64 pages on M4

    explicit FrameReader(Source source, std::size_t capacity = kDefaultCapacity)
        : source_(std::move(source)), buf_(capacity) {
        ITCH_ASSERT_MSG(capacity >= kMinBufferSize,
                        "frame buffer must hold at least one whole message");
    }

    // Same contract as FrameCursor::next, except that NeedMore is returned only
    // when the source is exhausted mid-message, which means a truncated input.
    FrameStatus next(std::span<const std::byte>& out) {
        for (;;) {
            const FrameStatus st = cursor_.next(out);
            if (st == FrameStatus::Ok) {
                frames_++;
                return st;
            }
            if (st == FrameStatus::Malformed) {
                return st;
            }
            // EndOfInput or NeedMore: both want more bytes.
            if (!refill()) {
                // The source is finished. Ask the cursor again so that the
                // status and the error describe what is actually left, rather
                // than what was true before refill() compacted and reset it.
                const FrameStatus fin = cursor_.next(out);
                if (fin == FrameStatus::Ok) {
                    frames_++;
                }
                return fin;
            }
        }
    }

    [[nodiscard]] FrameError error() const noexcept { return cursor_.error(); }
    [[nodiscard]] u64 frames() const noexcept { return frames_; }
    [[nodiscard]] u64 bytes_consumed() const noexcept {
        return consumed_ + cursor_.offset();
    }

    // Bytes left over when the input ended mid-message.
    [[nodiscard]] std::size_t trailing_bytes() const noexcept { return filled_; }

private:
    // Moves the unconsumed tail to the front, then reads into the space behind
    // it. Returns false when the source produced nothing.
    bool refill() {
        const std::size_t keep = filled_ - cursor_.offset();
        consumed_ += cursor_.offset();
        if (keep != 0 && cursor_.offset() != 0) {
            std::memmove(buf_.data(), buf_.data() + cursor_.offset(), keep);
        }
        filled_ = keep;

        const std::size_t got = source_.read(buf_.data() + filled_, buf_.size() - filled_);
        if (got == 0) {
            cursor_.reset(std::span<const std::byte>(buf_.data(), filled_));
            return false;
        }
        filled_ += got;
        cursor_.reset(std::span<const std::byte>(buf_.data(), filled_));
        return true;
    }

    Source                 source_;
    std::vector<std::byte> buf_;
    FrameCursor            cursor_{};
    std::size_t            filled_ = 0;
    u64                    frames_ = 0;
    u64                    consumed_ = 0;
};

}  // namespace itch::wire
