// Streaming gzip input.
//
// The corpus is a byte range of a gzip stream, so it ends mid-member by
// construction. allow_truncated makes that a clean end of input rather than an
// error, which is the whole reason a bounded prefix is usable at all. It covers
// only that case: a corrupt stream still throws, because a silently empty
// result is indistinguishable from a working one at the call site.
//
// The z_stream lives on the heap. zlib's internal state holds a back-pointer to
// the z_stream it was initialised with and inflate() rejects the stream if that
// pointer no longer matches, so a z_stream cannot be moved by value. Keeping it
// behind a unique_ptr lets this class stay movable while the address zlib knows
// about stays put.
//
// Nothing here belongs in a timing loop. Benchmarks run over a mapped plain
// file; putting inflate in the measurement would be measuring inflate.
#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <string>
#include <vector>

#include <zlib.h>

#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"

namespace itch::wire {

template <class Inner>
class GzipSource {
public:
    static constexpr std::size_t kInputChunk = 1u << 18;

    explicit GzipSource(Inner inner, bool allow_truncated = true)
        : inner_(std::move(inner)),
          zs_(std::make_unique<::z_stream>()),
          in_(kInputChunk),
          allow_truncated_(allow_truncated) {
        *zs_ = ::z_stream{};
        // 15 window bits plus 32 to auto-detect between zlib and gzip framing.
        const int rc = ::inflateInit2(zs_.get(), 15 + 32);
        if (rc != Z_OK) {
            throw std::runtime_error("inflateInit2 failed");
        }
    }

    ~GzipSource() {
        if (zs_) {
            ::inflateEnd(zs_.get());
        }
    }

    GzipSource(const GzipSource&) = delete;
    GzipSource& operator=(const GzipSource&) = delete;

    // Movable: the z_stream stays where it is, and moving a vector keeps its
    // heap buffer at the same address, so next_in stays valid too.
    GzipSource(GzipSource&& other) noexcept = default;
    GzipSource& operator=(GzipSource&& other) noexcept = default;

    std::size_t read(std::byte* dst, std::size_t n) {
        if (finished_ || n == 0) {
            return 0;
        }
        zs_->next_out = reinterpret_cast<::Bytef*>(dst);
        zs_->avail_out = static_cast<::uInt>(n);

        while (zs_->avail_out != 0) {
            if (zs_->avail_in == 0) {
                const std::size_t got = inner_.read(in_.data(), in_.size());
                if (got == 0) {
                    // The compressed input ran out. Ending before Z_STREAM_END
                    // is the truncated-prefix case and the only failure mode
                    // allow_truncated forgives.
                    if (!allow_truncated_) {
                        throw std::runtime_error("gzip stream ended mid-member");
                    }
                    finished_ = true;
                    break;
                }
                zs_->next_in = reinterpret_cast<::Bytef*>(in_.data());
                zs_->avail_in = static_cast<::uInt>(got);
            }

            const int rc = ::inflate(zs_.get(), Z_NO_FLUSH);
            if (rc == Z_STREAM_END) {
                finished_ = true;
                break;
            }
            if (rc == Z_BUF_ERROR) {
                // No progress was possible. That is normal only when the input
                // is exhausted and more is available; with bytes still unread it
                // means the stream is broken, and looping would spin forever.
                if (zs_->avail_in != 0) {
                    throw std::runtime_error("inflate made no progress on available input");
                }
                continue;
            }
            if (rc != Z_OK) {
                // Never forgiven, whatever allow_truncated says. A corrupt
                // stream that silently decodes to nothing is indistinguishable
                // from a working one at the call site, and that is exactly how
                // a broken z_stream move went unnoticed here once.
                throw std::runtime_error(std::string("inflate failed: ") +
                                         (zs_->msg != nullptr ? zs_->msg : "unknown"));
            }
        }

        const std::size_t produced = n - static_cast<std::size_t>(zs_->avail_out);
        out_total_ += produced;
        return produced;
    }

    [[nodiscard]] u64 bytes_produced() const noexcept { return out_total_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

private:
    Inner                       inner_;
    std::unique_ptr<::z_stream> zs_;
    std::vector<std::byte>      in_;
    bool                        allow_truncated_ = true;
    bool                        finished_ = false;
    u64                         out_total_ = 0;
};

}  // namespace itch::wire
