// Deterministic replay of an ITCH session into a book, with the invariants
// asserted during the run rather than checked at the end.
//
// The point of the digest is not that it exists but that it is identical
// across: repeated runs, the three book implementations, and the four build
// configurations. The first proves there is no hidden state. The second proves
// the fast book agrees with the slow ones on a real day's traffic, not just on
// generated tests. The third proves nothing depends on undefined behaviour,
// because a digest that survives -O3 and ASan is evidence that no optimisation
// changed the answer.
//
// Usage:
//   itch_replay [--symbol SYM] [--book flat|avl|map] [--runs N]
//               [--validate-every N] [--no-strict] <file|file.gz|->

#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "itch/book/avl_level_map.hpp"
#include "itch/book/flat_book.hpp"
#include "itch/book/map_book.hpp"
#include "itch/replay/book_builder.hpp"
#include "itch/replay/invariants.hpp"
#include "itch/wire/framing.hpp"
#include "itch/wire/gzip_source.hpp"
#include "itch/wire/source.hpp"
#include "itch/wire/views.hpp"

using namespace itch;
using namespace itch::wire;
using itch::book::AvlBook;
using itch::book::FlatBook;
using itch::book::MapBook;
using itch::replay::BookBuilder;
using itch::replay::ReplayObserver;

namespace {

struct Outcome {
    u64    digest = 0;
    u64    applied = 0;
    u64    messages = 0;
    u64    crossed = 0;
    u64    validations = 0;
    u64    peak_orders = 0;
    u64    peak_levels = 0;
    double seconds = 0;
    std::string symbol;
};

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

template <class Builder>
void drive(const std::string& path, Builder& b) {
    if (path == "-") {
        GzipSource<FdSource>                gz{FdSource{0}};
        FrameReader<GzipSource<FdSource>>   reader{std::move(gz)};
        std::span<const std::byte>          f;
        while (reader.next(f) == FrameStatus::Ok) {
            dispatch(f.data(), b);
        }
    } else if (ends_with(path, ".gz")) {
        GzipSource<FdSource>              gz{FdSource{path}};
        FrameReader<GzipSource<FdSource>> reader{std::move(gz)};
        std::span<const std::byte>        f;
        while (reader.next(f) == FrameStatus::Ok) {
            dispatch(f.data(), b);
        }
    } else {
        const MappedFile           file{path};
        FrameCursor                cur{file.span()};
        std::span<const std::byte> f;
        while (cur.next(f) == FrameStatus::Ok) {
            dispatch(f.data(), b);
        }
    }
}

template <class Book>
Outcome replay_once(const std::string& path, const std::string& symbol, u64 validate_every,
                    bool strict) {
    Book book;
    BookBuilder<Book, ReplayObserver> builder{book, symbol,
                                              ReplayObserver{validate_every, strict}};
    const auto t0 = std::chrono::steady_clock::now();
    drive(path, builder);
    const auto t1 = std::chrono::steady_clock::now();

    // One final full structural check, whatever the interval was.
    book.validate();

    Outcome o;
    o.digest = builder.observer().digest();
    const auto& c = builder.observer().counters();
    o.applied = c.applied;
    o.crossed = c.crossed_states;
    o.validations = c.full_validations;
    o.peak_orders = c.peak_orders;
    o.peak_levels = c.peak_levels;
    o.messages = builder.stats().messages;
    o.symbol = builder.symbol();
    o.seconds = std::chrono::duration<double>(t1 - t0).count();
    return o;
}

std::string commas(u64 v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), ",");
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string symbol = "QQQ";
    std::string which = "flat";
    int         runs = 1;
    u64         validate_every = 0;
    bool        strict = true;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--symbol" && i + 1 < argc) {
            symbol = argv[++i];
        } else if (a == "--book" && i + 1 < argc) {
            which = argv[++i];
        } else if (a == "--runs" && i + 1 < argc) {
            runs = std::atoi(argv[++i]);
        } else if (a == "--validate-every" && i + 1 < argc) {
            validate_every = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--no-strict") {
            strict = false;
        } else {
            path = a;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr,
                     "usage: itch_replay [--symbol SYM] [--book flat|avl|map] [--runs N]\n"
                     "                   [--validate-every N] [--no-strict] <file|file.gz|->\n");
        return 2;
    }

    std::vector<Outcome> outs;
    try {
        for (int r = 0; r < runs; ++r) {
            if (which == "flat") {
                outs.push_back(replay_once<FlatBook>(path, symbol, validate_every, strict));
            } else if (which == "avl") {
                outs.push_back(replay_once<AvlBook>(path, symbol, validate_every, strict));
            } else if (which == "map") {
                outs.push_back(replay_once<MapBook>(path, symbol, validate_every, strict));
            } else {
                std::fprintf(stderr, "unknown book '%s'\n", which.c_str());
                return 2;
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    const Outcome& o = outs.front();
    std::printf("book             %s\n", which.c_str());
    std::printf("symbol           %s\n", o.symbol.c_str());
    std::printf("messages seen    %s\n", commas(o.messages).c_str());
    std::printf("book updates     %s\n", commas(o.applied).c_str());
    std::printf("peak orders      %s\n", commas(o.peak_orders).c_str());
    std::printf("peak levels      %s\n", commas(o.peak_levels).c_str());
    std::printf("crossed states   %s%s\n", commas(o.crossed).c_str(),
                o.crossed == 0 ? "" : "  <-- the feed showed a crossed book");
    std::printf("full validations %s\n", commas(o.validations).c_str());
    std::printf("elapsed          %.2f s\n", o.seconds);

    // Determinism: identical input, identical output, every time.
    bool all_same = true;
    for (const Outcome& x : outs) {
        if (x.digest != o.digest) {
            all_same = false;
        }
    }
    std::printf("digest           %016llx\n", static_cast<unsigned long long>(o.digest));
    if (runs > 1) {
        std::printf("determinism      %d runs, %s\n", runs,
                    all_same ? "all digests identical" : "DIGESTS DIFFER");
    }
    return all_same ? 0 : 1;
}
