// A replayable stream of book operations, extracted from the real feed.
//
// Benchmarks run on this rather than on generated traffic. The shape of the
// work is what determines the result: the measured feed is 42% adds and 39%
// deletes, half of all insertions land exactly on the inside, and every symbol
// carries permanent stub quotes in the overflow map. A synthetic workload with
// a uniform price distribution would benchmark a book nobody is running.
//
// Extraction happens once, outside any timing loop, so reading the corpus
// through gzip costs nothing that is measured.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/core/types.hpp"
#include "itch/wire/framing.hpp"
#include "itch/wire/gzip_source.hpp"
#include "itch/wire/source.hpp"
#include "itch/wire/views.hpp"

namespace itch::bench {

using itch::OrderRef;
using itch::Price;
using itch::Qty;
using itch::book::Side;

enum class Op : itch::u8 { Add, Cancel, Execute, Delete, Replace };

struct WorkloadOp {
    OrderRef  ref;
    OrderRef  new_ref;  // Replace only
    Price     price;
    Qty       qty;
    Op        op;
    itch::u8  side;
    itch::u16 pad;
};
static_assert(sizeof(WorkloadOp) == 32);

struct Workload {
    std::string             symbol;
    itch::u16               locate = 0;
    std::vector<WorkloadOp> ops;
    std::size_t             adds = 0;
    std::size_t             cancels = 0;
    std::size_t             executes = 0;
    std::size_t             deletes = 0;
    std::size_t             replaces = 0;
};

namespace detail {

class Recorder {
public:
    explicit Recorder(Workload& out, std::string_view symbol) : w_(out) { w_.symbol = symbol; }

    void on_stock_directory(wire::StockDirectoryView v) {
        if (resolved_) {
            return;
        }
        const std::string_view sym = wire::trim_alpha(v.stock());
        if (w_.symbol.empty() || sym == w_.symbol) {
            w_.symbol = sym;
            w_.locate = v.stock_locate();
            resolved_ = true;
        }
    }

    void on_add_order(wire::AddOrderView v) {
        if (!mine(v.stock_locate())) {
            return;
        }
        push(Op::Add, v.order_reference_number(), 0, v.price(), v.shares(),
             v.buy_sell_indicator() == 'B' ? Side::Buy : Side::Sell);
        ++w_.adds;
    }

    void on_add_order_with_mpid(wire::AddOrderWithMpidView v) {
        if (!mine(v.stock_locate())) {
            return;
        }
        push(Op::Add, v.order_reference_number(), 0, v.price(), v.shares(),
             v.buy_sell_indicator() == 'B' ? Side::Buy : Side::Sell);
        ++w_.adds;
    }

    void on_order_executed(wire::OrderExecutedView v) {
        if (!mine(v.stock_locate())) {
            return;
        }
        push(Op::Execute, v.order_reference_number(), 0, 0, v.executed_shares(), Side::Buy);
        ++w_.executes;
    }

    void on_order_executed_with_price(wire::OrderExecutedWithPriceView v) {
        if (!mine(v.stock_locate())) {
            return;
        }
        push(Op::Execute, v.order_reference_number(), 0, 0, v.executed_shares(), Side::Buy);
        ++w_.executes;
    }

    void on_order_cancel(wire::OrderCancelView v) {
        if (!mine(v.stock_locate())) {
            return;
        }
        push(Op::Cancel, v.order_reference_number(), 0, 0, v.cancelled_shares(), Side::Buy);
        ++w_.cancels;
    }

    void on_order_delete(wire::OrderDeleteView v) {
        if (!mine(v.stock_locate())) {
            return;
        }
        push(Op::Delete, v.order_reference_number(), 0, 0, 0, Side::Buy);
        ++w_.deletes;
    }

    void on_order_replace(wire::OrderReplaceView v) {
        if (!mine(v.stock_locate())) {
            return;
        }
        push(Op::Replace, v.original_order_reference_number(), v.new_order_reference_number(),
             v.price(), v.shares(), Side::Buy);
        ++w_.replaces;
    }

    [[nodiscard]] bool resolved() const noexcept { return resolved_; }

private:
    [[nodiscard]] bool mine(itch::u16 locate) const noexcept {
        return resolved_ && locate == w_.locate;
    }

    void push(Op op, OrderRef ref, OrderRef new_ref, Price price, Qty qty, Side side) {
        w_.ops.push_back(WorkloadOp{ref, new_ref, price, qty, op, static_cast<itch::u8>(side),
                                    0});
    }

    Workload& w_;
    bool      resolved_ = false;
};

inline bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace detail

[[nodiscard]] inline Workload load_workload(const std::string& path, std::string_view symbol) {
    Workload w;
    detail::Recorder rec{w, symbol};
    if (detail::ends_with(path, ".gz")) {
        wire::GzipSource<wire::FdSource>                     gz{wire::FdSource{path}};
        wire::FrameReader<wire::GzipSource<wire::FdSource>> reader{std::move(gz)};
        std::span<const std::byte>                           f;
        while (reader.next(f) == wire::FrameStatus::Ok) {
            wire::dispatch(f.data(), rec);
        }
    } else {
        const wire::MappedFile     file{path};
        wire::FrameCursor          cur{file.span()};
        std::span<const std::byte> f;
        while (cur.next(f) == wire::FrameStatus::Ok) {
            wire::dispatch(f.data(), rec);
        }
    }
    return w;
}

// Applies one operation. Shared by every implementation so that the benchmark
// compares books and not driver code.
template <class Book>
inline void apply(Book& book, const WorkloadOp& op) {
    switch (op.op) {
        case Op::Add:
            book.add(op.ref, static_cast<Side>(op.side), op.price, op.qty);
            break;
        case Op::Cancel:
            book.cancel(op.ref, op.qty);
            break;
        case Op::Execute:
            (void)book.execute(op.ref, op.qty);
            break;
        case Op::Delete:
            book.remove(op.ref);
            break;
        case Op::Replace:
            book.replace(op.ref, op.new_ref, op.price, op.qty);
            break;
    }
}

}  // namespace itch::bench
