// NASDAQ BinaryFILE framing: every message is preceded by a two-byte
// big-endian length covering the message body, type byte included.
//
// The cursor validates as it goes. A length prefix that disagrees with the
// declared length for that message type is reported rather than followed,
// which is what turns a run over the real feed into a check of all 23 layout
// structs. It is two instructions: a table lookup and a compare.
#pragma once

#include <cstddef>
#include <span>

#include "itch/core/assert.hpp"
#include "itch/core/endian.hpp"
#include "itch/core/types.hpp"
#include "itch/wire/messages.hpp"

namespace itch::wire {

enum class FrameStatus : u8 {
    Ok,          // a complete, well-formed frame was produced
    EndOfInput,  // the buffer ended exactly on a frame boundary
    NeedMore,    // a frame straddles the end of the buffer
    Malformed,   // the stream cannot be trusted past this point
};

enum class FrameError : u8 {
    None,
    TruncatedLengthPrefix,  // fewer than two bytes remain
    TruncatedBody,          // the prefix promises more bytes than remain
    ZeroLength,             // a zero-length message cannot carry a type byte
    UnknownType,            // type byte is not one of the 23 known messages
    LengthMismatch,         // prefix disagrees with the type's declared length
};

[[nodiscard]] constexpr const char* to_string(FrameError e) noexcept {
    switch (e) {
        case FrameError::None: return "none";
        case FrameError::TruncatedLengthPrefix: return "truncated length prefix";
        case FrameError::TruncatedBody: return "truncated message body";
        case FrameError::ZeroLength: return "zero length message";
        case FrameError::UnknownType: return "unknown message type";
        case FrameError::LengthMismatch: return "length disagrees with message type";
    }
    return "unknown";
}

// Size of the length prefix that precedes each message.
inline constexpr std::size_t kLengthPrefixSize = 2;

// A buffer smaller than this cannot always hold one whole framed message.
inline constexpr std::size_t kMinBufferSize = kLengthPrefixSize + kMaxMessageLength;

class FrameCursor {
public:
    constexpr FrameCursor() noexcept = default;

    explicit constexpr FrameCursor(std::span<const std::byte> buf) noexcept : buf_(buf) {}

    // Produces the next frame. On Ok, out points at the type byte and its size
    // is the message length. On NeedMore or Malformed nothing is consumed, so
    // the caller can refill the buffer or inspect the failure.
    FrameStatus next(std::span<const std::byte>& out) noexcept {
        error_ = FrameError::None;
        const std::size_t avail = buf_.size() - pos_;

        if (avail == 0) {
            return FrameStatus::EndOfInput;
        }
        if (avail < kLengthPrefixSize) {
            error_ = FrameError::TruncatedLengthPrefix;
            return FrameStatus::NeedMore;
        }

        const std::size_t len = load_be<u16>(buf_.data() + pos_);
        if (len == 0) {
            error_ = FrameError::ZeroLength;
            return FrameStatus::Malformed;
        }
        if (avail - kLengthPrefixSize < len) {
            error_ = FrameError::TruncatedBody;
            return FrameStatus::NeedMore;
        }

        const std::byte* body = buf_.data() + pos_ + kLengthPrefixSize;
        const char type = static_cast<char>(load_be<u8>(body));
        const std::size_t declared = message_length(type);
        if (declared == 0) {
            error_ = FrameError::UnknownType;
            return FrameStatus::Malformed;
        }
        if (declared != len) {
            error_ = FrameError::LengthMismatch;
            return FrameStatus::Malformed;
        }

        out = std::span<const std::byte>(body, len);
        pos_ += kLengthPrefixSize + len;
        ITCH_INVARIANT(pos_ <= buf_.size());
        return FrameStatus::Ok;
    }

    [[nodiscard]] constexpr FrameError error() const noexcept { return error_; }

    // Bytes consumed as complete frames, including their length prefixes.
    [[nodiscard]] constexpr std::size_t offset() const noexcept { return pos_; }

    // The unconsumed tail. After NeedMore this is the partial frame.
    [[nodiscard]] constexpr std::span<const std::byte> rest() const noexcept {
        return buf_.subspan(pos_);
    }

    constexpr void reset(std::span<const std::byte> buf) noexcept {
        buf_ = buf;
        pos_ = 0;
        error_ = FrameError::None;
    }

private:
    std::span<const std::byte> buf_{};
    std::size_t                pos_ = 0;
    FrameError                 error_ = FrameError::None;
};

}  // namespace itch::wire
