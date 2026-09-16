// Builds framed ITCH streams by hand, for tests.
//
// Fields are written with store_be at offsetof(...) on the layout struct, which
// is the mirror image of how the views read them. A test that writes a field
// and reads it back therefore exercises both directions against the same single
// source of truth for the offset.
#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "itch/core/assert.hpp"
#include "itch/core/endian.hpp"
#include "itch/wire/framing.hpp"
#include "itch/wire/messages.hpp"

namespace itch::test {

class WireBuilder {
public:
    // Appends a framed message of the given type, body zero filled, and returns
    // the offset of its body so the caller can write fields into it.
    std::size_t begin(char type) {
        const std::size_t len = itch::wire::message_length(type);
        ITCH_ASSERT_MSG(len > 0, "cannot build a message of an unknown type");
        const std::size_t at = buf_.size();
        // insert rather than resize: GCC's array-bounds analysis does not model
        // the reallocation in vector::resize here and reports the zero fill of
        // the new region as writing past the old end.
        buf_.insert(buf_.end(), itch::wire::kLengthPrefixSize + len, std::byte{0});
        itch::store_be<itch::u16>(buf_.data() + at, static_cast<itch::u16>(len));
        const std::size_t body = at + itch::wire::kLengthPrefixSize;
        buf_[body] = static_cast<std::byte>(type);
        return body;
    }

    // Appends a framed message whose declared length deliberately disagrees
    // with the type, for the malformed-input tests.
    void append_raw(itch::u16 declared_length, std::span<const std::byte> body) {
        const std::size_t at = buf_.size();
        buf_.resize(at + itch::wire::kLengthPrefixSize + body.size(), std::byte{0});
        itch::store_be<itch::u16>(buf_.data() + at, declared_length);
        if (!body.empty()) {
            std::memcpy(buf_.data() + at + itch::wire::kLengthPrefixSize, body.data(),
                        body.size());
        }
    }

    void append_bytes(std::span<const std::byte> raw) {
        buf_.insert(buf_.end(), raw.begin(), raw.end());
    }

    template <class T>
    void put(std::size_t body, std::size_t field_offset, T value) {
        itch::store_be<T>(buf_.data() + body + field_offset, value);
    }

    void put_be48(std::size_t body, std::size_t field_offset, itch::u64 value) {
        itch::store_be48(buf_.data() + body + field_offset, value & 0xFFFFFFFFFFFFULL);
    }

    void put_char(std::size_t body, std::size_t field_offset, char value) {
        buf_[body + field_offset] = static_cast<std::byte>(value);
    }

    // Alpha fields are left justified and space padded.
    void put_alpha(std::size_t body, std::size_t field_offset, std::size_t width,
                   std::string_view text) {
        for (std::size_t i = 0; i < width; ++i) {
            buf_[body + field_offset + i] =
                static_cast<std::byte>(i < text.size() ? text[i] : ' ');
        }
    }

    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        return std::span<const std::byte>(buf_.data(), buf_.size());
    }

    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }

    // Drops the last n bytes, producing a truncated tail.
    void truncate(std::size_t n) { buf_.resize(buf_.size() - n); }

    void clear() noexcept { buf_.clear(); }

private:
    std::vector<std::byte> buf_;
};

}  // namespace itch::test
