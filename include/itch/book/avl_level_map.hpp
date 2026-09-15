// A hand-written AVL tree of price levels, exposing the same interface as
// PriceLadder.
//
// This exists to isolate one variable. MapBook answers "what did the whole
// design buy over the obvious implementation?", which is a fair question but
// changes the level structure, the queue representation, the allocator and the
// order index all at once. Swapping only the price-to-level lookup, with the
// same pools, the same intrusive queues and the same order index on both sides,
// answers the narrower and more useful question: what did the ladder buy?
//
// It is a deliberately strong baseline. Nodes come from a pooled vector with a
// free list, not from the allocator, so the comparison is between a tree walk
// and an array index, not between a tree and malloc.
#pragma once

#include <algorithm>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/book/handle.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"

namespace itch::book {

class AvlLevelMap {
public:
    static constexpr u32 kNull = 0xFFFF'FFFFu;

    explicit AvlLevelMap(Side side, u32 capacity_hint = 1u << 15) : side_(side) {
        nodes_.reserve(capacity_hint);
    }

    // --- lookup ----------------------------------------------------------

    [[nodiscard]] LevelHandle find(Price p) const noexcept {
        u32 n = root_;
        while (n != kNull) {
            const Node& node = nodes_[n];
            if (p == node.price) {
                return node.level;
            }
            n = (p < node.price) ? node.left : node.right;
        }
        return LevelHandle{};
    }

    // --- mutation --------------------------------------------------------

    void insert(Price p, LevelHandle h) {
        ITCH_ASSERT_MSG(!h.is_null(), "cannot insert a null level handle");
        root_ = insert_at(root_, p, h);
        ++size_;
    }

    void erase(Price p) {
        ITCH_ASSERT_MSG(size_ > 0, "erasing from an empty tree");
        const u32 before = size_;
        root_ = erase_at(root_, p);
        ITCH_ASSERT_MSG(size_ == before - 1, "erasing a level that is not there");
    }

    // --- best ------------------------------------------------------------

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] u32 size() const noexcept { return size_; }

    // Bids want the maximum price, asks the minimum. The tree is always ordered
    // ascending; only which end counts as the inside depends on the side.
    [[nodiscard]] Price best() const {
        ITCH_ASSERT_MSG(!empty(), "best() on an empty side");
        u32 n = root_;
        if (side_ == Side::Buy) {
            while (nodes_[n].right != kNull) {
                n = nodes_[n].right;
            }
        } else {
            while (nodes_[n].left != kNull) {
                n = nodes_[n].left;
            }
        }
        return nodes_[n].price;
    }

    // Levels from the inside outwards: reverse in-order for bids, in-order for
    // asks. Iterative, with an explicit stack, so a pathological tree cannot
    // overflow the real one.
    template <class Fn>
    void for_each_level(Fn&& fn) const {
        stack_.clear();
        u32 n = root_;
        while (n != kNull || !stack_.empty()) {
            while (n != kNull) {
                stack_.push_back(n);
                n = (side_ == Side::Buy) ? nodes_[n].right : nodes_[n].left;
            }
            n = stack_.back();
            stack_.pop_back();
            fn(nodes_[n].price, nodes_[n].level);
            n = (side_ == Side::Buy) ? nodes_[n].left : nodes_[n].right;
        }
    }

    void validate() const {
        u32 counted = 0;
        Price prev = 0;
        bool  first = true;
        check(root_, counted);
        // In-order traversal must be strictly ascending.
        walk_ascending(root_, [&](Price p) {
            if (!first) {
                ITCH_ASSERT_MSG(p > prev, "tree is out of order");
            }
            first = false;
            prev = p;
        });
        ITCH_ASSERT_MSG(counted == size_, "tree size disagrees with its contents");
    }

    // Reported for the benchmark: how deep the tree actually got.
    [[nodiscard]] u32 height() const noexcept { return height_of(root_); }

private:
    struct Node {
        Price       price;
        LevelHandle level;
        u32         left = kNull;
        u32         right = kNull;
        u32         height = 1;
    };

    [[nodiscard]] u32 height_of(u32 n) const noexcept {
        return n == kNull ? 0 : nodes_[n].height;
    }

    [[nodiscard]] int balance_of(u32 n) const noexcept {
        return n == kNull ? 0
                          : static_cast<int>(height_of(nodes_[n].left)) -
                                static_cast<int>(height_of(nodes_[n].right));
    }

    void retune(u32 n) noexcept {
        nodes_[n].height = 1 + std::max(height_of(nodes_[n].left), height_of(nodes_[n].right));
    }

    u32 rotate_right(u32 y) noexcept {
        const u32 x = nodes_[y].left;
        nodes_[y].left = nodes_[x].right;
        nodes_[x].right = y;
        retune(y);
        retune(x);
        return x;
    }

    u32 rotate_left(u32 x) noexcept {
        const u32 y = nodes_[x].right;
        nodes_[x].right = nodes_[y].left;
        nodes_[y].left = x;
        retune(x);
        retune(y);
        return y;
    }

    u32 rebalance(u32 n) noexcept {
        retune(n);
        const int b = balance_of(n);
        if (b > 1) {
            if (balance_of(nodes_[n].left) < 0) {
                nodes_[n].left = rotate_left(nodes_[n].left);
            }
            return rotate_right(n);
        }
        if (b < -1) {
            if (balance_of(nodes_[n].right) > 0) {
                nodes_[n].right = rotate_right(nodes_[n].right);
            }
            return rotate_left(n);
        }
        return n;
    }

    u32 new_node(Price p, LevelHandle h) {
        u32 i;
        if (free_ != kNull) {
            i = free_;
            free_ = nodes_[i].left;
        } else {
            i = static_cast<u32>(nodes_.size());
            nodes_.emplace_back();
        }
        nodes_[i] = Node{p, h, kNull, kNull, 1};
        return i;
    }

    void release(u32 i) noexcept {
        nodes_[i].left = free_;
        free_ = i;
    }

    u32 insert_at(u32 n, Price p, LevelHandle h) {
        if (n == kNull) {
            return new_node(p, h);
        }
        ITCH_INVARIANT_MSG(p != nodes_[n].price, "a level already exists at this price");
        if (p < nodes_[n].price) {
            nodes_[n].left = insert_at(nodes_[n].left, p, h);
        } else {
            nodes_[n].right = insert_at(nodes_[n].right, p, h);
        }
        return rebalance(n);
    }

    u32 erase_at(u32 n, Price p) {
        if (n == kNull) {
            return kNull;
        }
        if (p < nodes_[n].price) {
            nodes_[n].left = erase_at(nodes_[n].left, p);
        } else if (p > nodes_[n].price) {
            nodes_[n].right = erase_at(nodes_[n].right, p);
        } else {
            --size_;
            const u32 l = nodes_[n].left;
            const u32 r = nodes_[n].right;
            if (l == kNull || r == kNull) {
                release(n);
                return (l == kNull) ? r : l;
            }
            // Two children: take the in-order successor's payload, then delete
            // the successor from the right subtree.
            u32 s = r;
            while (nodes_[s].left != kNull) {
                s = nodes_[s].left;
            }
            const Price       sp = nodes_[s].price;
            const LevelHandle sh = nodes_[s].level;
            ++size_;  // the recursive call below decrements for the successor
            nodes_[n].right = erase_at(nodes_[n].right, sp);
            nodes_[n].price = sp;
            nodes_[n].level = sh;
        }
        return rebalance(n);
    }

    void check(u32 n, u32& counted) const {
        if (n == kNull) {
            return;
        }
        ++counted;
        check(nodes_[n].left, counted);
        check(nodes_[n].right, counted);
        const u32 h = 1 + std::max(height_of(nodes_[n].left), height_of(nodes_[n].right));
        ITCH_ASSERT_MSG(nodes_[n].height == h, "stored height is wrong");
        const int b = balance_of(n);
        ITCH_ASSERT_MSG(b >= -1 && b <= 1, "tree is not balanced");
        ITCH_ASSERT_MSG(!nodes_[n].level.is_null(), "node holds a null level handle");
    }

    template <class Fn>
    void walk_ascending(u32 n, Fn&& fn) const {
        if (n == kNull) {
            return;
        }
        walk_ascending(nodes_[n].left, fn);
        fn(nodes_[n].price);
        walk_ascending(nodes_[n].right, fn);
    }

    Side                      side_;
    std::vector<Node>         nodes_;
    u32                       root_ = kNull;
    u32                       free_ = kNull;
    u32                       size_ = 0;
    mutable std::vector<u32>  stack_;
};

}  // namespace itch::book
