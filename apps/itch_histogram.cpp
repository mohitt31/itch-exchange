// Measures the distributions that decide the price ladder's geometry.
//
// The ladder is the one structure in this project whose shape cannot be
// reasoned out from first principles. A flat array indexed by price is only
// worth having if real orders cluster near the inside, and only sizeable if the
// clustering is measured. So this tool answers four questions off the real
// feed, and the answers go into DESIGN.md before the ladder is written:
//
//   1. How far from the inside does a new order land, in ticks?
//      This sizes the hot window.
//
//   2. What fraction of prices are not a multiple of $0.01?
//      Reg NMS Rule 612 forces displayed orders on stocks at or above $1 to
//      penny increments, so a ladder indexed by penny should cover almost
//      everything -- but sub-penny prices are legal below $1. Indexing by raw
//      1/10000 units instead would need 5.1 million slots to span +/- $5
//      rather than 512, so this number decides whether the penny index works
//      and how much traffic the overflow map has to carry.
//
//   3. How many levels and orders are live at once?
//      This sizes the level pool, the order pool and the order index.
//
//   4. How far does the inside travel over the session?
//      This decides between a window fixed for the day and one that slides.
//
// Usage:
//   itch_histogram [--symbol SYM] [--json out.json] <file|file.gz|->

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "itch/book/map_book.hpp"
#include "itch/replay/book_builder.hpp"
#include "itch/wire/framing.hpp"
#include "itch/wire/gzip_source.hpp"
#include "itch/wire/source.hpp"
#include "itch/wire/views.hpp"

using namespace itch;
using namespace itch::wire;
using itch::book::MapBook;
using itch::book::Side;
using itch::book::side_index;

namespace {

// Ticks are counted in pennies: kPennyUnits raw ITCH units.
constexpr i64 kMaxTracked = 1 << 16;  // +/- 65536 pennies is +/- $655.36

class Collector {
public:
    // Called with the book as it stands before the order is applied.
    template <class Book>
    void before_add(const Book& book, Side side, Price price, Qty qty) {
        ++adds_;
        peak_qty_ = std::max<u64>(peak_qty_, qty);

        if (price % kPennyUnits != 0) {
            ++sub_penny_;
        }
        min_price_ = (min_price_ == 0) ? price : std::min(min_price_, price);
        max_price_ = std::max(max_price_, price);

        if (book.empty(side)) {
            ++no_inside_;
            return;
        }
        const Price best = book.best(side);
        // Positive means further from the inside, negative means the order
        // improves it. Buys improve upward, sells improve downward.
        const i64 raw = (side == Side::Buy) ? static_cast<i64>(best) - static_cast<i64>(price)
                                            : static_cast<i64>(price) - static_cast<i64>(best);
        const i64 ticks = raw / static_cast<i64>(kPennyUnits);

        ++measured_;
        if (ticks < 0) {
            ++improving_;
        }
        const i64 mag = ticks < 0 ? -ticks : ticks;
        max_distance_ = std::max(max_distance_, mag);
        if (mag < kMaxTracked) {
            ++distance_[static_cast<std::size_t>(mag)];
        } else {
            ++beyond_;
        }
    }

    template <class Book>
    void after_apply(const Book& book, Timestamp) {
        const u32 levels =
            book.stats().level_count[0] + book.stats().level_count[1];
        peak_levels_ = std::max<u64>(peak_levels_, levels);
        peak_orders_ = std::max<u64>(peak_orders_, book.order_count());
        level_sum_ += levels;
        order_sum_ += book.order_count();
        ++samples_;

        if (!book.empty(Side::Buy)) {
            const Price b = book.best(Side::Buy);
            min_bid_ = (min_bid_ == 0) ? b : std::min(min_bid_, b);
            max_bid_ = std::max(max_bid_, b);
        }
        if (book.crossed()) {
            ++crossed_;
        }
    }

    // Smallest tick distance covering at least `fraction` of measured adds.
    [[nodiscard]] i64 quantile(double fraction) const {
        const auto target = static_cast<u64>(static_cast<double>(measured_) * fraction);
        u64 acc = 0;
        for (std::size_t i = 0; i < kMaxTracked; ++i) {
            acc += distance_[i];
            if (acc >= target) {
                return static_cast<i64>(i);
            }
        }
        return kMaxTracked;
    }

    [[nodiscard]] u64 within(i64 ticks) const {
        u64 acc = 0;
        const auto n = static_cast<std::size_t>(std::min<i64>(ticks + 1, kMaxTracked));
        for (std::size_t i = 0; i < n; ++i) {
            acc += distance_[i];
        }
        return acc;
    }

    u64 adds_ = 0;
    u64 measured_ = 0;
    u64 improving_ = 0;
    u64 no_inside_ = 0;
    u64 beyond_ = 0;
    u64 sub_penny_ = 0;
    i64 max_distance_ = 0;
    u64 peak_levels_ = 0;
    u64 peak_orders_ = 0;
    u64 peak_qty_ = 0;
    u64 level_sum_ = 0;
    u64 order_sum_ = 0;
    u64 samples_ = 0;
    u64 crossed_ = 0;
    Price min_price_ = 0;
    Price max_price_ = 0;
    Price min_bid_ = 0;
    Price max_bid_ = 0;
    std::vector<u64> distance_ = std::vector<u64>(kMaxTracked, 0);
};

std::string commas(u64 v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), ",");
    }
    return s;
}

std::string dollars(Price p) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu.%04llu",
                  static_cast<unsigned long long>(p / kPriceScale),
                  static_cast<unsigned long long>(p % kPriceScale));
    return buf;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

template <class Builder>
void drive(std::span<const std::byte> buf, Builder& b) {
    FrameCursor cur{buf};
    std::span<const std::byte> f;
    while (cur.next(f) == FrameStatus::Ok) {
        dispatch(f.data(), b);
    }
}

template <class Reader, class Builder>
void drive_stream(Reader& reader, Builder& b) {
    std::span<const std::byte> f;
    while (reader.next(f) == FrameStatus::Ok) {
        dispatch(f.data(), b);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string symbol;
    std::string json_path;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--symbol" && i + 1 < argc) {
            symbol = argv[++i];
        } else if (a == "--json" && i + 1 < argc) {
            json_path = argv[++i];
        } else {
            path = argv[i];
        }
    }
    if (path == nullptr) {
        std::fprintf(stderr,
                     "usage: itch_histogram [--symbol SYM] [--json out.json] <file|file.gz|->\n");
        return 2;
    }

    MapBook book;
    itch::replay::BookBuilder<MapBook, Collector> builder{book, symbol, Collector{}};

    const auto t0 = std::chrono::steady_clock::now();
    try {
        const std::string p = path;
        if (p == "-") {
            GzipSource<FdSource> gz{FdSource{0}};
            FrameReader<GzipSource<FdSource>> reader{std::move(gz)};
            drive_stream(reader, builder);
        } else if (ends_with(p, ".gz")) {
            GzipSource<FdSource> gz{FdSource{p}};
            FrameReader<GzipSource<FdSource>> reader{std::move(gz)};
            drive_stream(reader, builder);
        } else {
            const MappedFile file{p};
            drive(file.span(), builder);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const Collector& c = builder.observer();
    const auto&      st = builder.stats();

    if (!builder.resolved()) {
        std::fprintf(stderr, "error: symbol '%s' never appeared in a stock directory message\n",
                     symbol.c_str());
        return 1;
    }
    if (c.measured_ == 0) {
        std::fprintf(stderr, "error: no book activity for %s\n",
                     std::string(builder.symbol()).c_str());
        return 1;
    }

    const auto pct = [&](u64 n) {
        return 100.0 * static_cast<double>(n) / static_cast<double>(c.measured_);
    };

    std::printf("symbol           %s (locate %u)\n", std::string(builder.symbol()).c_str(),
                builder.locate());
    std::printf("messages         %s total, %s applied\n", commas(st.messages).c_str(),
                commas(st.applied).c_str());
    std::printf("  adds           %s\n", commas(st.adds).c_str());
    std::printf("  executions     %s (%s shares)\n", commas(st.executions).c_str(),
                commas(st.executed_shares).c_str());
    std::printf("  cancels        %s\n", commas(st.cancels).c_str());
    std::printf("  deletes        %s\n", commas(st.deletes).c_str());
    std::printf("  replaces       %s\n", commas(st.replaces).c_str());
    std::printf("  trades ignored %s\n", commas(st.trades_ignored + st.crosses_ignored).c_str());
    std::printf("elapsed          %.2f s\n\n", seconds);

    std::printf("1. distance from the inside, in pennies, at insertion\n");
    std::printf("   measured       %s adds (%s had no inside yet)\n",
                commas(c.measured_).c_str(), commas(c.no_inside_).c_str());
    std::printf("   improving      %s (%.3f%%)\n", commas(c.improving_).c_str(),
                pct(c.improving_));
    std::printf("   max distance   %lld ticks ($%.2f)\n",
                static_cast<long long>(c.max_distance_),
                static_cast<double>(c.max_distance_) / 100.0);
    std::printf("\n   %-10s %14s %10s\n", "quantile", "ticks", "dollars");
    for (double q : {0.50, 0.90, 0.99, 0.999, 0.9999, 1.0}) {
        const i64 t = c.quantile(q);
        std::printf("   %-10.4f %14lld %10.2f\n", q, static_cast<long long>(t),
                    static_cast<double>(t) / 100.0);
    }
    std::printf("\n   %-14s %16s %10s\n", "window", "adds inside", "coverage");
    for (i64 w : {16, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384}) {
        const u64 in = c.within(w);
        std::printf("   +/-%-11lld %16s %9.4f%%\n", static_cast<long long>(w),
                    commas(in).c_str(), pct(in));
    }

    std::printf("\n2. tick alignment\n");
    std::printf("   sub-penny      %s of %s adds (%.6f%%)\n", commas(c.sub_penny_).c_str(),
                commas(c.adds_).c_str(),
                100.0 * static_cast<double>(c.sub_penny_) / static_cast<double>(c.adds_));
    std::printf("   price range    $%s .. $%s\n", dollars(c.min_price_).c_str(),
                dollars(c.max_price_).c_str());

    std::printf("\n3. live structure size\n");
    std::printf("   peak levels    %s (mean %.1f)\n", commas(c.peak_levels_).c_str(),
                c.samples_ ? static_cast<double>(c.level_sum_) / static_cast<double>(c.samples_)
                           : 0.0);
    std::printf("   peak orders    %s (mean %.1f)\n", commas(c.peak_orders_).c_str(),
                c.samples_ ? static_cast<double>(c.order_sum_) / static_cast<double>(c.samples_)
                           : 0.0);
    std::printf("   largest order  %s shares\n", commas(c.peak_qty_).c_str());

    std::printf("\n4. how far the inside travels\n");
    std::printf("   best bid range $%s .. $%s\n", dollars(c.min_bid_).c_str(),
                dollars(c.max_bid_).c_str());
    std::printf("   span           %lld ticks\n",
                static_cast<long long>((c.max_bid_ - c.min_bid_) / kPennyUnits));
    std::printf("   crossed states %s\n", commas(c.crossed_).c_str());

    if (!json_path.empty()) {
        std::FILE* out = std::fopen(json_path.c_str(), "w");
        if (out == nullptr) {
            std::fprintf(stderr, "error: cannot write %s\n", json_path.c_str());
            return 1;
        }
        std::fprintf(out, "{\n  \"symbol\": \"%s\",\n  \"locate\": %u,\n",
                     std::string(builder.symbol()).c_str(), builder.locate());
        std::fprintf(out, "  \"adds\": %llu,\n  \"measured\": %llu,\n  \"improving\": %llu,\n",
                     static_cast<unsigned long long>(c.adds_),
                     static_cast<unsigned long long>(c.measured_),
                     static_cast<unsigned long long>(c.improving_));
        std::fprintf(out, "  \"sub_penny\": %llu,\n  \"max_distance_ticks\": %lld,\n",
                     static_cast<unsigned long long>(c.sub_penny_),
                     static_cast<long long>(c.max_distance_));
        std::fprintf(out, "  \"peak_levels\": %llu,\n  \"peak_orders\": %llu,\n",
                     static_cast<unsigned long long>(c.peak_levels_),
                     static_cast<unsigned long long>(c.peak_orders_));
        std::fprintf(out, "  \"min_price\": %u,\n  \"max_price\": %u,\n", c.min_price_,
                     c.max_price_);
        std::fprintf(out, "  \"crossed_states\": %llu,\n  \"quantiles\": {",
                     static_cast<unsigned long long>(c.crossed_));
        bool first = true;
        for (double q : {0.50, 0.90, 0.99, 0.999, 0.9999, 1.0}) {
            std::fprintf(out, "%s\n    \"%.4f\": %lld", first ? "" : ",", q,
                         static_cast<long long>(c.quantile(q)));
            first = false;
        }
        std::fprintf(out, "\n  },\n  \"coverage\": {");
        first = true;
        for (i64 w : {16, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384}) {
            std::fprintf(out, "%s\n    \"%lld\": %llu", first ? "" : ",",
                         static_cast<long long>(w),
                         static_cast<unsigned long long>(c.within(w)));
            first = false;
        }
        std::fprintf(out, "\n  }\n}\n");
        std::fclose(out);
        std::printf("\nwrote %s\n", json_path.c_str());
    }
    return 0;
}
