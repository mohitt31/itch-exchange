// Is cancelling from the middle of a queue really O(1)?
//
// DESIGN.md asserts it, on the grounds that each order holds its own previous
// and next handles so unlinking touches a fixed number of fields. That is a
// claim about the code, and the code could still be O(n) somewhere else -- the
// index erase, the level bookkeeping, the pool free. So it is measured against
// the thing that would make it O(n): where in the queue the cancelled order
// sits, and how long the queue is.
//
// If it is O(1), the time does not move. If anything walks the queue, cancelling
// from the tail of a 100,000 deep level costs far more than from the head.

#include <cstdio>
#include <string>
#include <vector>

#include "bench_support.hpp"
#include "itch/book/flat_book.hpp"

using namespace itch;
using namespace itch::bench;
using namespace itch::book;

namespace {

constexpr Price kPrice = 100'0000;

// Builds one level `depth` deep, then cancels one order at the given fractional
// position, repeatedly. The book is rebuilt between measurements so every
// cancel faces the same queue.
template <class Book>
double cancel_at(u32 depth, double where, int reps) {
    double total = 0;
    for (int r = 0; r < reps; ++r) {
        Book b{depth * 2 + 64, 1024, index_entries_for(depth * 2)};
        for (u64 i = 1; i <= depth; ++i) {
            b.add(i, Side::Buy, kPrice, 100);
        }
        const auto target =
            static_cast<u64>(1 + static_cast<double>(depth - 1) * where);
        const u64 t0 = now_ns();
        b.remove(target);
        const u64 t1 = now_ns();
        do_not_optimize(b.order_count());
        total += static_cast<double>(t1 - t0);
    }
    return total / reps;
}

}  // namespace

int main(int argc, char** argv) {
    int reps = 2000;
    if (argc > 2 && std::string(argv[1]) == "--reps") {
        reps = std::atoi(argv[2]);
    }

    std::printf("one price level, all orders at the same price\n");
    std::printf("%d rebuild-and-cancel repetitions per cell, mean ns\n", reps);
    std::printf("timer floor is %llu ns, so single-cancel numbers are quantised;\n",
                static_cast<unsigned long long>(timer_resolution_ns()));
    std::printf("what matters is whether the column moves, not its absolute value\n\n");

    std::printf("%-12s %10s %12s %12s %12s %12s\n", "hash", "depth", "head", "25%",
                "middle", "tail");
    std::printf("%s\n", std::string(74, '-').c_str());
    for (u32 depth : {u32{64}, u32{1024}, u32{16384}, u32{100000}}) {
        std::printf("%-12s %10s", "identity", commas(depth).c_str());
        for (double where : {0.0, 0.25, 0.5, 1.0}) {
            std::printf(" %12.1f", cancel_at<FlatBookIdentity>(depth, where, reps));
        }
        std::printf("\n");
        std::printf("%-12s %10s", "splitmix64", commas(depth).c_str());
        for (double where : {0.0, 0.25, 0.5, 1.0}) {
            std::printf(" %12.1f", cancel_at<FlatBook>(depth, where, reps));
        }
        std::printf("\n");
    }
    std::printf("\nunlinking from the queue is a fixed number of field writes. Anything\n");
    std::printf("that grows with depth here is not the queue -- it is the order index's\n");
    std::printf("backward-shift deletion walking a probe cluster.\n");
    return 0;
}
