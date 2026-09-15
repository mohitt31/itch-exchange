// Price to level lookup: a sliding flat window plus an ordered overflow map.
//
// The geometry comes from measurement, not preference. DESIGN.md section 15 has
// the numbers; the short version is that half of all insertions land exactly on
// the inside, a +/- 2048 penny window covers 98.9% to 99.9% of them, and every
// symbol carries permanent stub quotes at $0.0001 and $199,999.99 that would
// squat on hot slots forever under a direct-mapped scheme.
//
// Three pieces per side:
//
//   occupancy  4096 bits = 512 B = 4 cache lines. Finding the next best level
//              after one clears is a scan of u64 words with clz/ctz, usually
//              one word, rather than a walk over the handle array.
//
//   slots      4096 x 4-byte level handle = 16 KiB, exactly one M4 page and one
//              TLB entry. Handles rather than inline Level records: inline would
//              be 128 KiB per side and almost all of it empty, whereas handles
//              keep the live levels packed densely in a pool that fits in L1.
//
//   overflow   std::map. Ordered deliberately: when the entire book is outside
//              the window -- pre-market, when the only resting orders are stub
//              quotes -- best() still has to be correct, and an unordered map
//              cannot answer that.
//
// Invariant that makes lookup a single probe: a level is in the window if and
// only if its price is penny-aligned and its tick is in [base, base + 4096).
// Everything else is in the overflow map. Rebasing reconciles both directions,
// so the two are always disjoint and exhaustive.
#pragma once

#include <bit>
#include <cstring>
#include <map>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/book/handle.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"

namespace itch::book {

class PriceLadder {
public:
    static constexpr u32   kWindowTicks = 4096;           // +/- $20.48
    static constexpr u32   kWords = kWindowTicks / 64;    // 64 u64 words
    static constexpr u32   kRebaseMargin = 512;           // recentre this close to an edge
    static constexpr Price kTickUnits = kPennyUnits;      // one cent, in raw ITCH units

    static_assert(kWindowTicks % 64 == 0);
    static_assert(std::has_single_bit(kWindowTicks), "window must be a power of two");

    explicit PriceLadder(Side side) : side_(side) {
        slots_.assign(kWindowTicks, LevelHandle{});
        std::memset(words_, 0, sizeof(words_));
    }

    // --- lookup ----------------------------------------------------------

    [[nodiscard]] LevelHandle find(Price p) const noexcept {
        if (in_window(p)) {
            const u32 s = slot_of(p);
            if (bit_set(s)) {
                ++window_hits_;
                return slots_[s];
            }
            ++window_hits_;
            return LevelHandle{};
        }
        ++overflow_hits_;
        const auto it = overflow_.find(p);
        return it == overflow_.end() ? LevelHandle{} : it->second;
    }

    // --- mutation --------------------------------------------------------

    void insert(Price p, LevelHandle h) {
        ITCH_ASSERT_MSG(!h.is_null(), "cannot insert a null level handle");
        ITCH_INVARIANT_MSG(find(p).is_null(), "a level already exists at this price");
        if (in_window(p)) {
            const u32 s = slot_of(p);
            slots_[s] = h;
            set_bit(s);
        } else {
            overflow_.emplace(p, h);
        }
        ++levels_;
        maybe_rebase();
    }

    void erase(Price p) {
        if (in_window(p)) {
            const u32 s = slot_of(p);
            ITCH_ASSERT_MSG(bit_set(s), "erasing a level that is not there");
            clear_bit(s);
            slots_[s] = LevelHandle{};
        } else {
            const auto it = overflow_.find(p);
            ITCH_ASSERT_MSG(it != overflow_.end(), "erasing a level that is not there");
            overflow_.erase(it);
        }
        ITCH_ASSERT(levels_ > 0);
        --levels_;
    }

    // --- best ------------------------------------------------------------

    [[nodiscard]] bool empty() const noexcept { return levels_ == 0; }

    // Highest price on the bid side, lowest on the ask side.
    //
    // Not noexcept, deliberately: it asserts, and in the test build the assert
    // handler throws so that "this misuse is detected" can be an ordinary test
    // rather than a death test. A noexcept function that throws calls
    // std::terminate, which would make the misuse untestable. The rule across
    // this codebase is that a function containing an assertion is not marked
    // noexcept; none of them are anywhere the annotation would buy anything.
    [[nodiscard]] Price best() const {
        ITCH_ASSERT_MSG(!empty(), "best() on an empty side");
        const Price from_window = best_in_window();
        if (overflow_.empty()) {
            ITCH_ASSERT(from_window != kNoPrice);
            return from_window;
        }
        const Price from_overflow =
            (side_ == Side::Buy) ? overflow_.rbegin()->first : overflow_.begin()->first;
        if (from_window == kNoPrice) {
            return from_overflow;
        }
        return better(from_window, from_overflow);
    }

    // --- introspection, for tests and for DESIGN.md ----------------------

    [[nodiscard]] u32 size() const noexcept { return levels_; }
    [[nodiscard]] u32 in_window_count() const noexcept {
        u32 n = 0;
        for (u64 w : words_) {
            n += static_cast<u32>(std::popcount(w));
        }
        return n;
    }
    [[nodiscard]] std::size_t overflow_count() const noexcept { return overflow_.size(); }
    [[nodiscard]] i64 base_tick() const noexcept { return base_; }
    [[nodiscard]] u64 rebases() const noexcept { return rebases_; }
    [[nodiscard]] u64 window_hits() const noexcept { return window_hits_; }
    [[nodiscard]] u64 overflow_hits() const noexcept { return overflow_hits_; }
    [[nodiscard]] u64 levels_displaced() const noexcept { return displaced_; }

    // Levels from the inside outwards.
    template <class Fn>
    void for_each_level(Fn&& fn) const {
        // Merge the window and the overflow map in price order. Both are
        // already ordered, so this is a two-way merge and never a sort.
        std::vector<Price> win;
        win.reserve(in_window_count());
        collect_window(win);
        auto wi = win.begin();
        if (side_ == Side::Buy) {
            auto oi = overflow_.rbegin();
            while (wi != win.end() || oi != overflow_.rend()) {
                if (oi == overflow_.rend() || (wi != win.end() && *wi > oi->first)) {
                    fn(*wi, slots_[slot_of(*wi)]);
                    ++wi;
                } else {
                    fn(oi->first, oi->second);
                    ++oi;
                }
            }
        } else {
            auto oi = overflow_.begin();
            while (wi != win.end() || oi != overflow_.end()) {
                if (oi == overflow_.end() || (wi != win.end() && *wi < oi->first)) {
                    fn(*wi, slots_[slot_of(*wi)]);
                    ++wi;
                } else {
                    fn(oi->first, oi->second);
                    ++oi;
                }
            }
        }
    }

    void validate() const {
        u32 counted = in_window_count();
        ITCH_ASSERT_MSG(counted + overflow_.size() == levels_,
                        "ladder level count disagrees with its contents");
        for (u32 w = 0; w < kWords; ++w) {
            u64 bits = words_[w];
            while (bits != 0) {
                const u32 s = w * 64 + static_cast<u32>(std::countr_zero(bits));
                bits &= bits - 1;
                ITCH_ASSERT_MSG(!slots_[s].is_null(), "occupied slot holds a null handle");
            }
        }
        for (const auto& [price, h] : overflow_) {
            ITCH_ASSERT_MSG(!h.is_null(), "overflow holds a null handle");
            // The whole point of the invariant: nothing that belongs in the
            // window may be sitting in the overflow map.
            ITCH_ASSERT_MSG(!in_window(price),
                            "a price inside the window is in the overflow map");
        }
    }

private:
    [[nodiscard]] static bool aligned(Price p) noexcept { return p % kTickUnits == 0; }

    [[nodiscard]] static i64 tick_of(Price p) noexcept {
        return static_cast<i64>(p / kTickUnits);
    }

    [[nodiscard]] bool in_window(Price p) const noexcept {
        if (!aligned(p)) {
            return false;  // sub-penny prices always live in the overflow map
        }
        const i64 t = tick_of(p);
        return t >= base_ && t < base_ + static_cast<i64>(kWindowTicks);
    }

    [[nodiscard]] u32 slot_of(Price p) const noexcept {
        return static_cast<u32>(tick_of(p) - base_);
    }

    [[nodiscard]] Price price_of_slot(u32 s) const noexcept {
        return static_cast<Price>((base_ + static_cast<i64>(s)) * static_cast<i64>(kTickUnits));
    }

    [[nodiscard]] bool bit_set(u32 s) const noexcept {
        return (words_[s >> 6] >> (s & 63)) & 1u;
    }
    void set_bit(u32 s) noexcept { words_[s >> 6] |= u64{1} << (s & 63); }
    void clear_bit(u32 s) noexcept { words_[s >> 6] &= ~(u64{1} << (s & 63)); }

    [[nodiscard]] Price better(Price a, Price b) const noexcept {
        return (side_ == Side::Buy) ? (a > b ? a : b) : (a < b ? a : b);
    }

    // Highest set bit for bids, lowest for asks. One word in the common case.
    [[nodiscard]] Price best_in_window() const noexcept {
        if (side_ == Side::Buy) {
            for (u32 w = kWords; w-- > 0;) {
                if (words_[w] != 0) {
                    const u32 s = w * 64 + 63 - static_cast<u32>(std::countl_zero(words_[w]));
                    return price_of_slot(s);
                }
            }
        } else {
            for (u32 w = 0; w < kWords; ++w) {
                if (words_[w] != 0) {
                    const u32 s = w * 64 + static_cast<u32>(std::countr_zero(words_[w]));
                    return price_of_slot(s);
                }
            }
        }
        return kNoPrice;
    }

    void collect_window(std::vector<Price>& out) const {
        if (side_ == Side::Buy) {
            for (u32 w = kWords; w-- > 0;) {
                u64 bits = words_[w];
                while (bits != 0) {
                    const u32 top = 63 - static_cast<u32>(std::countl_zero(bits));
                    bits &= ~(u64{1} << top);
                    out.push_back(price_of_slot(w * 64 + top));
                }
            }
        } else {
            for (u32 w = 0; w < kWords; ++w) {
                u64 bits = words_[w];
                while (bits != 0) {
                    const u32 low = static_cast<u32>(std::countr_zero(bits));
                    bits &= bits - 1;
                    out.push_back(price_of_slot(w * 64 + low));
                }
            }
        }
    }

    // Recentres the window on the inside when the inside has drifted to an
    // edge, or when it has left the window entirely.
    void maybe_rebase() {
        if (empty()) {
            return;
        }
        // The anchor is the inside's tick, whether or not the inside itself is
        // penny aligned. A sub-penny inside cannot be stored in the window, but
        // it still says where the action is, and the aligned prices around it
        // are exactly what the window should be covering. Bailing out here
        // instead left the window stranded at its initial position whenever the
        // first resting price was sub-penny.
        const i64 t = tick_of(best());
        const i64 lo = base_ + kRebaseMargin;
        const i64 hi = base_ + static_cast<i64>(kWindowTicks) - kRebaseMargin;
        if (t >= lo && t < hi) {
            return;
        }
        rebase(t);
    }

    void rebase(i64 anchor) {
        const i64 new_base = anchor - static_cast<i64>(kWindowTicks) / 2;
        if (new_base == base_) {
            return;
        }

        // Lift everything out of the window first. There are at most a few
        // thousand live levels, so this walks the occupied bits rather than the
        // 4096 slots, and never memmoves the whole array.
        scratch_.clear();
        for (u32 w = 0; w < kWords; ++w) {
            u64 bits = words_[w];
            while (bits != 0) {
                const u32 s = w * 64 + static_cast<u32>(std::countr_zero(bits));
                bits &= bits - 1;
                scratch_.emplace_back(price_of_slot(s), slots_[s]);
                slots_[s] = LevelHandle{};
            }
        }
        std::memset(words_, 0, sizeof(words_));
        base_ = new_base;

        // Put them back where they now belong. Anything that fell outside goes
        // to the overflow map, which is what keeps find() a single probe.
        for (const auto& [price, h] : scratch_) {
            if (in_window(price)) {
                const u32 s = slot_of(price);
                slots_[s] = h;
                set_bit(s);
            } else {
                overflow_.emplace(price, h);
                ++displaced_;
            }
        }

        // And pull in anything the window has now slid over. The map is
        // ordered, so this touches only the affected range.
        const Price lo_price = static_cast<Price>(base_ * static_cast<i64>(kTickUnits));
        const Price hi_price = static_cast<Price>(
            (base_ + static_cast<i64>(kWindowTicks)) * static_cast<i64>(kTickUnits));
        for (auto it = overflow_.lower_bound(lo_price);
             it != overflow_.end() && it->first < hi_price;) {
            if (in_window(it->first)) {
                const u32 s = slot_of(it->first);
                slots_[s] = it->second;
                set_bit(s);
                it = overflow_.erase(it);
            } else {
                ++it;  // sub-penny: stays where it is
            }
        }
        ++rebases_;
    }

    Side                          side_;
    i64                           base_ = 0;
    std::vector<LevelHandle>      slots_;
    u64                           words_[kWords]{};
    std::map<Price, LevelHandle>  overflow_;
    std::vector<std::pair<Price, LevelHandle>> scratch_;
    u32                           levels_ = 0;
    u64                           rebases_ = 0;
    u64                           displaced_ = 0;
    mutable u64                   window_hits_ = 0;
    mutable u64                   overflow_hits_ = 0;
};

}  // namespace itch::book
