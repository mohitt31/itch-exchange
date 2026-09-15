// Order reference to handle. The hottest lookup in the system: every execute,
// cancel, delete and replace does exactly one.
//
// Open addressing with linear probing, power-of-two capacity, load factor held
// at or below 0.5, and backward-shift deletion so there are no tombstones to
// degrade the table over a day of churn. Entries are 16 bytes, so eight sit on
// a 128-byte M4 cache line and a probe that misses usually stays on that line.
//
// The hash is a template parameter because NASDAQ order references are
// near-sequential, which makes identity hashing genuinely plausible here:
// sequential keys under linear probing fill the table in order and probe
// perfectly. It is also exactly the kind of thing that is obvious right up
// until it is wrong, so both are built and benchmarked rather than argued
// about. The number is in NUMBERS.md.
//
// Sized and prefaulted at construction. A rehash during replay is an assertion,
// not a resize: growing the table mid-run would move every entry and put a
// multi-millisecond spike into the latency tail.
#pragma once

#include <bit>
#include <cstddef>
#include <vector>

#include "itch/book/handle.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/hash.hpp"
#include "itch/core/types.hpp"

namespace itch::book {

enum class IndexHash {
    Identity,  // order references are near-sequential; use them as-is
    Mixed,     // splitmix64 finaliser
};

template <IndexHash H = IndexHash::Mixed>
class OrderIndex {
public:
    struct Entry {
        OrderRef    ref;
        OrderHandle handle;
        u32         pad;
    };
    static_assert(sizeof(Entry) == 16, "eight entries per M4 cache line");

    // capacity_hint is the number of live orders to plan for; the table is
    // sized to at least twice that, rounded up to a power of two.
    explicit OrderIndex(u32 capacity_hint) {
        const u32 want = std::bit_ceil(capacity_hint < 8 ? u32{16} : capacity_hint * 2);
        slots_.assign(want, Entry{0, OrderHandle{}, 0});
        mask_ = want - 1;
        prefault();
    }

    [[nodiscard]] static u64 hash(OrderRef ref) noexcept {
        if constexpr (H == IndexHash::Identity) {
            return ref;
        } else {
            return mix64(ref);
        }
    }

    void insert(OrderRef ref, OrderHandle h) {
        ITCH_ASSERT_MSG(!h.is_null(), "cannot index a null handle");
        ITCH_ASSERT_MSG(live_ * 2 < slots_.size(),
                        "order index would exceed half full; it never resizes during replay");
        std::size_t i = hash(ref) & mask_;
        while (!slots_[i].handle.is_null()) {
            ITCH_INVARIANT_MSG(slots_[i].ref != ref, "order reference inserted twice");
            i = (i + 1) & mask_;
            ++probes_;
        }
        slots_[i] = Entry{ref, h, 0};
        ++live_;
        ++inserts_;
    }

    [[nodiscard]] OrderHandle find(OrderRef ref) const {
        std::size_t i = hash(ref) & mask_;
        for (;;) {
            const Entry& e = slots_[i];
            if (e.handle.is_null()) {
                return OrderHandle{};
            }
            if (e.ref == ref) {
                return e.handle;
            }
            i = (i + 1) & mask_;
            ++probes_;
        }
    }

    void erase(OrderRef ref) {
        std::size_t i = hash(ref) & mask_;
        for (;;) {
            ITCH_ASSERT_MSG(!slots_[i].handle.is_null(), "erasing a reference not in the index");
            if (slots_[i].ref == ref) {
                break;
            }
            i = (i + 1) & mask_;
            ++probes_;
        }
        erase_at(i);
        --live_;
    }

    [[nodiscard]] u32 size() const noexcept { return live_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] u64 probes() const noexcept { return probes_; }
    [[nodiscard]] u64 inserts() const noexcept { return inserts_; }

    // Deliberately returns the two counters rather than their ratio: the
    // no-floating-point rule applies below apps/, and a diagnostic accessor is
    // not a good enough reason to put a double here. Callers that want an
    // average divide them.

    void validate() const {
        u32 counted = 0;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].handle.is_null()) {
                continue;
            }
            ++counted;
            // Every live entry must be reachable from its ideal slot without
            // crossing an empty one, which is what linear probing promises and
            // what backward-shift deletion has to preserve.
            std::size_t j = hash(slots_[i].ref) & mask_;
            while (j != i) {
                ITCH_ASSERT_MSG(!slots_[j].handle.is_null(),
                                "an empty slot sits between an entry and its ideal slot");
                j = (j + 1) & mask_;
            }
        }
        ITCH_ASSERT_MSG(counted == live_, "index count disagrees with its contents");
    }

private:
    // Knuth's algorithm R. Moves later entries back over the hole so that no
    // tombstone is needed and no probe chain is broken.
    void erase_at(std::size_t i) {
        std::size_t j = i;
        for (;;) {
            slots_[i].handle = OrderHandle{};
            std::size_t k = 0;
            for (;;) {
                j = (j + 1) & mask_;
                if (slots_[j].handle.is_null()) {
                    return;
                }
                k = hash(slots_[j].ref) & mask_;
                // Is k outside the cyclic interval (i, j]? Then entry j must
                // move back to i, otherwise it stays where it is.
                const bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
                if (!stays) {
                    break;
                }
            }
            slots_[i] = slots_[j];
            i = j;
        }
    }

    void prefault() {
        constexpr std::size_t kPage = 4096;
        auto* bytes = reinterpret_cast<volatile unsigned char*>(slots_.data());
        const std::size_t total = slots_.size() * sizeof(Entry);
        for (std::size_t off = 0; off < total; off += kPage) {
            bytes[off] = bytes[off];
        }
    }

    std::vector<Entry> slots_;
    std::size_t        mask_ = 0;
    u32                live_ = 0;
    mutable u64        probes_ = 0;
    u64                inserts_ = 0;
};

}  // namespace itch::book
