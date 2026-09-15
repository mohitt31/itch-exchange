// Slot pool with an intrusive free list and generation-checked handles.
//
// Intrusive in the literal sense: the free list link lives inside the slot,
// reusing a field the slot cannot be using while it is free. An order in the
// free list is not in any queue, so its queue link is available. That costs
// zero extra bytes, and unlike the usual trick of writing an index over the
// slot's raw storage it involves no type punning, so UBSan stays quiet.
//
// The generation lives in the slot too, not in a parallel array. Validating a
// handle then touches the same cache line as the data it is about to return,
// rather than two lines on every single dereference.
//
// Prefaulted at construction: every page is written once up front so that a
// replay run never takes a page fault inside the book. That is not a
// micro-optimisation, it is what keeps the tail latency measurement honest --
// otherwise the first touch of each page shows up as a multi-microsecond
// outlier and pollutes p99.9 with something that is not the book's doing.
#pragma once

#include <concepts>
#include <cstddef>
#include <vector>

#include "itch/book/handle.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"

namespace itch::book {

// A poolable slot lends the pool two fields it is not using while free.
template <class T, class Tag>
concept PoolSlot = requires(T& t) {
    { t.pool_link() } -> std::same_as<Handle<Tag>&>;
    { t.pool_generation() } -> std::same_as<u8&>;
};

// How many freed slots are held back before they can be handed out again.
// Nonzero only where the extra memory is worth certainty: with a quarantine,
// a handle used after free names a slot whose generation has definitely moved,
// so detection does not depend on the generation not having wrapped.
#if ITCH_INVARIANT_LEVEL >= 2
inline constexpr std::size_t kPoolQuarantine = 4096;
#else
inline constexpr std::size_t kPoolQuarantine = 0;
#endif

template <class T, class Tag>
    requires PoolSlot<T, Tag>
class Pool {
public:
    using HandleType = Handle<Tag>;

    // quarantine: how many freed slots to hold back before reuse. Defaults to
    // kPoolQuarantine, which is nonzero only in the sanitizer builds. Tests that
    // need to force a slot to come straight back pass 0 explicitly.
    explicit Pool(u32 capacity, std::size_t quarantine = kPoolQuarantine)
        : slots_(capacity), quarantine_limit_(quarantine) {
        ITCH_ASSERT_MSG(capacity > 0, "a pool needs at least one slot");
        ITCH_ASSERT_MSG(capacity <= HandleType::kMaxCapacity,
                        "capacity exceeds what a 24-bit index can address");

        // Build the free list in ascending order so the first allocations walk
        // forward through memory rather than backwards, which is what the
        // hardware prefetcher expects.
        for (u32 i = 0; i < capacity; ++i) {
            slots_[i].pool_generation() = 0;  // even: free
            slots_[i].pool_link() = (i + 1 < capacity) ? HandleType::make(i + 1, 0)
                                                       : HandleType{};
        }
        free_head_ = HandleType::make(0, 0);
        prefault();
    }

    [[nodiscard]] HandleType allocate() {
        ITCH_ASSERT_MSG(!free_head_.is_null(), "order pool exhausted");
        const u32 index = free_head_.index();
        T&        slot = slots_[index];
        free_head_ = slot.pool_link();

        // Odd generation means live. Incrementing on both allocate and free is
        // what lets one compare answer both "same generation?" and "alive?".
        slot.pool_generation() = static_cast<u8>(slot.pool_generation() + 1);
        ITCH_INVARIANT((slot.pool_generation() & 1u) == 1u);

        ++live_;
        high_water_ = (live_ > high_water_) ? live_ : high_water_;
        return HandleType::make(index, slot.pool_generation());
    }

    void free(HandleType h) {
        T& slot = deref_checked(h);
        slot.pool_generation() = static_cast<u8>(slot.pool_generation() + 1);
        ITCH_INVARIANT((slot.pool_generation() & 1u) == 0u);
        --live_;

        if (quarantine_limit_ > 0) {
            // Hold the slot back so that a stale handle names a slot whose
            // generation has certainly moved on.
            quarantine_.push_back(h.index());
            if (quarantine_.size() > quarantine_limit_) {
                release(quarantine_.front());
                quarantine_.erase(quarantine_.begin());
            }
        } else {
            release(h.index());
        }
    }

    [[nodiscard]] T& operator[](HandleType h) { return deref_checked(h); }
    [[nodiscard]] const T& operator[](HandleType h) const {
        return const_cast<Pool*>(this)->deref_checked(h);
    }

    // True when the handle names a slot that is still the one it was issued for.
    [[nodiscard]] bool is_live(HandleType h) const noexcept {
        if (h.is_null() || h.index() >= slots_.size()) {
            return false;
        }
        const u8 g = const_cast<Pool*>(this)->slots_[h.index()].pool_generation();
        return g == h.generation() && (g & 1u) == 1u;
    }

    [[nodiscard]] u32 live() const noexcept { return live_; }
    [[nodiscard]] u32 high_water() const noexcept { return high_water_; }
    [[nodiscard]] u32 capacity() const noexcept { return static_cast<u32>(slots_.size()); }
    [[nodiscard]] u32 quarantined() const noexcept {
        return static_cast<u32>(quarantine_.size());
    }

    // O(n) check that the free list is a well-formed list over free slots only,
    // with no cycle and no live slot in it.
    void validate() const {
        std::size_t walked = 0;
        HandleType  h = free_head_;
        std::vector<bool> seen(slots_.size(), false);
        while (!h.is_null()) {
            const u32 i = h.index();
            ITCH_ASSERT_MSG(i < slots_.size(), "free list leaves the pool");
            ITCH_ASSERT_MSG(!seen[i], "free list has a cycle");
            seen[i] = true;
            const u8 g = const_cast<Pool*>(this)->slots_[i].pool_generation();
            ITCH_ASSERT_MSG((g & 1u) == 0u, "a live slot is in the free list");
            h = const_cast<Pool*>(this)->slots_[i].pool_link();
            ++walked;
        }
        ITCH_ASSERT_MSG(walked + live_ + quarantine_.size() == slots_.size(),
                        "slots are neither live, free nor quarantined");
    }

private:
    T& deref_checked(HandleType h) {
        ITCH_ASSERT_MSG(!h.is_null(), "dereferenced a null handle");
        ITCH_ASSERT_MSG(h.index() < slots_.size(), "handle index out of range");
        T& slot = slots_[h.index()];
        ITCH_ASSERT_MSG(slot.pool_generation() == h.generation(),
                        "stale handle: the slot has been reused");
        ITCH_ASSERT_MSG((slot.pool_generation() & 1u) == 1u,
                        "stale handle: the slot has been freed");
        return slot;
    }

    void release(u32 index) {
        slots_[index].pool_link() = free_head_;
        free_head_ = HandleType::make(index, slots_[index].pool_generation());
    }

    void prefault() {
        // One write per page. Reading would not do: a read of untouched
        // anonymous memory can be served by the shared zero page, which faults
        // again on the first write.
        constexpr std::size_t kPage = 4096;  // smallest across our targets
        auto* bytes = reinterpret_cast<volatile unsigned char*>(slots_.data());
        const std::size_t total = slots_.size() * sizeof(T);
        for (std::size_t off = 0; off < total; off += kPage) {
            bytes[off] = bytes[off];
        }
    }

    std::vector<T>   slots_;
    std::vector<u32> quarantine_;
    std::size_t      quarantine_limit_ = 0;
    HandleType       free_head_{};
    u32              live_ = 0;
    u32              high_water_ = 0;
};

}  // namespace itch::book
