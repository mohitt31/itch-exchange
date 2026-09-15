// Counts messages by type over an ITCH 5.0 BinaryFILE stream and reports
// framing health.
//
// This is also the check that validates every layout struct against reality:
// the framing layer compares each message's length prefix with the length
// declared for its type, so a full pass asserts all 23 sizes once per message.
//
// Usage:
//   itch_stats <file>          plain file, mapped, zero copy
//   itch_stats <file.gz>       gzip, streamed
//   itch_stats -               stdin, gzip auto-detected
//   itch_stats --json ...      machine readable summary

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "itch/wire/framing.hpp"
#include "itch/wire/gzip_source.hpp"
#include "itch/wire/source.hpp"
#include "itch/wire/views.hpp"

using namespace itch;
using namespace itch::wire;

namespace {

struct Stats {
    std::array<u64, 256> by_type{};
    u64                  messages = 0;
    u64                  payload_bytes = 0;
    u64                  framed_bytes = 0;
    Timestamp            first_timestamp = 0;
    Timestamp            last_timestamp = 0;
    bool                 saw_timestamp = false;

    void observe(std::span<const std::byte> f) {
        const auto type = static_cast<unsigned char>(f[0]);
        ++by_type[type];
        ++messages;
        payload_bytes += f.size();
        framed_bytes += f.size() + kLengthPrefixSize;
        // Every message carries the timestamp at the same offset.
        const Timestamp ts = load_be48(f.data() + 5);
        if (!saw_timestamp) {
            first_timestamp = ts;
            saw_timestamp = true;
        }
        last_timestamp = ts;
    }
};

struct Outcome {
    FrameStatus status = FrameStatus::EndOfInput;
    FrameError  error = FrameError::None;
    u64         offset = 0;
    std::size_t trailing = 0;
};

Outcome run_mapped(const MappedFile& file, Stats& s) {
    FrameCursor cur{file.span()};
    std::span<const std::byte> f;
    for (;;) {
        const FrameStatus st = cur.next(f);
        if (st != FrameStatus::Ok) {
            return Outcome{st, cur.error(), cur.offset(), cur.rest().size()};
        }
        s.observe(f);
    }
}

template <class Reader>
Outcome run_streamed(Reader& reader, Stats& s) {
    std::span<const std::byte> f;
    for (;;) {
        const FrameStatus st = reader.next(f);
        if (st != FrameStatus::Ok) {
            return Outcome{st, reader.error(), reader.bytes_consumed(),
                           reader.trailing_bytes()};
        }
        s.observe(f);
    }
}

std::string commas(u64 v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), ",");
    }
    return s;
}

std::string time_of_day(Timestamp ns) {
    const u64 total_s = ns / 1'000'000'000ULL;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu.%09llu",
                  static_cast<unsigned long long>(total_s / 3600),
                  static_cast<unsigned long long>((total_s / 60) % 60),
                  static_cast<unsigned long long>(total_s % 60),
                  static_cast<unsigned long long>(ns % 1'000'000'000ULL));
    return buf;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void report_text(const Stats& s, const Outcome& out, double seconds, const std::string& src,
                 const char* path) {
    std::printf("input            %s (%s)\n", path, src.c_str());
    std::printf("messages         %s\n", commas(s.messages).c_str());
    std::printf("payload bytes    %s\n", commas(s.payload_bytes).c_str());
    std::printf("framed bytes     %s\n", commas(s.framed_bytes).c_str());
    if (s.saw_timestamp) {
        std::printf("first timestamp  %s\n", time_of_day(s.first_timestamp).c_str());
        std::printf("last timestamp   %s\n", time_of_day(s.last_timestamp).c_str());
    }
    std::printf("elapsed          %.3f s\n", seconds);
    if (seconds > 0) {
        std::printf("throughput       %s msg/s, %.1f MiB/s\n",
                    commas(static_cast<u64>(static_cast<double>(s.messages) / seconds)).c_str(),
                    static_cast<double>(s.framed_bytes) / seconds / (1024.0 * 1024.0));
    }

    std::printf("\nterminated       %s",
                out.status == FrameStatus::EndOfInput ? "clean end of input"
                : out.status == FrameStatus::NeedMore ? "truncated input"
                                                      : "MALFORMED");
    if (out.error != FrameError::None) {
        std::printf(" (%s)", to_string(out.error));
    }
    std::printf("\n");
    if (out.trailing != 0) {
        std::printf("trailing bytes   %zu\n", out.trailing);
    }

    std::printf("\n%-4s %-34s %14s %8s\n", "type", "name", "count", "share");
    std::printf("%s\n", std::string(64, '-').c_str());
    std::vector<std::pair<u64, unsigned char>> rows;
    for (unsigned t = 0; t < 256; ++t) {
        if (s.by_type[t] != 0) {
            rows.emplace_back(s.by_type[t], static_cast<unsigned char>(t));
        }
    }
    std::sort(rows.begin(), rows.end(), std::greater<>());
    for (const auto& [count, t] : rows) {
        const double share =
            s.messages != 0 ? 100.0 * static_cast<double>(count) / static_cast<double>(s.messages)
                            : 0.0;
        std::printf("%-4c %-34s %14s %7.3f%%\n", static_cast<char>(t),
                    type_name(static_cast<char>(t)), commas(count).c_str(), share);
    }
    std::printf("%s\n%-4s %-34s %14s %7.3f%%\n", std::string(64, '-').c_str(), "", "total",
                commas(s.messages).c_str(), s.messages != 0 ? 100.0 : 0.0);

    // Types defined by the spec that never appeared.
    std::string missing;
    for (unsigned t = 0; t < 256; ++t) {
        if (is_known_type(static_cast<char>(t)) && s.by_type[t] == 0) {
            missing += static_cast<char>(t);
            missing += ' ';
        }
    }
    if (!missing.empty()) {
        std::printf("\nnot seen         %s\n", missing.c_str());
    }
}

void report_json(const Stats& s, const Outcome& out, double seconds, const char* path) {
    std::printf("{\n  \"input\": \"%s\",\n  \"messages\": %llu,\n", path,
                static_cast<unsigned long long>(s.messages));
    std::printf("  \"payload_bytes\": %llu,\n  \"framed_bytes\": %llu,\n",
                static_cast<unsigned long long>(s.payload_bytes),
                static_cast<unsigned long long>(s.framed_bytes));
    std::printf("  \"seconds\": %.6f,\n", seconds);
    std::printf("  \"first_timestamp_ns\": %llu,\n  \"last_timestamp_ns\": %llu,\n",
                static_cast<unsigned long long>(s.first_timestamp),
                static_cast<unsigned long long>(s.last_timestamp));
    std::printf("  \"terminated\": \"%s\",\n  \"frame_error\": \"%s\",\n",
                out.status == FrameStatus::EndOfInput  ? "clean"
                : out.status == FrameStatus::NeedMore ? "truncated"
                                                      : "malformed",
                to_string(out.error));
    std::printf("  \"trailing_bytes\": %zu,\n  \"by_type\": {", out.trailing);
    bool first = true;
    for (unsigned t = 0; t < 256; ++t) {
        if (s.by_type[t] == 0) {
            continue;
        }
        std::printf("%s\n    \"%c\": %llu", first ? "" : ",", static_cast<char>(t),
                    static_cast<unsigned long long>(s.by_type[t]));
        first = false;
    }
    std::printf("\n  }\n}\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool        json = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            json = true;
        } else {
            path = argv[i];
        }
    }
    if (path == nullptr) {
        std::fprintf(stderr,
                     "usage: itch_stats [--json] <file|file.gz|->\n"
                     "  counts ITCH 5.0 messages by type and reports framing health\n");
        return 2;
    }

    Stats   stats;
    Outcome out;
    std::string source_kind;
    const auto t0 = std::chrono::steady_clock::now();

    try {
        const std::string p = path;
        if (p == "-") {
            source_kind = "stdin, gzip";
            GzipSource<FdSource> gz{FdSource{0}};
            FrameReader<GzipSource<FdSource>> reader{std::move(gz)};
            out = run_streamed(reader, stats);
        } else if (ends_with(p, ".gz")) {
            source_kind = "gzip, streamed";
            GzipSource<FdSource> gz{FdSource{p}};
            FrameReader<GzipSource<FdSource>> reader{std::move(gz)};
            out = run_streamed(reader, stats);
        } else {
            source_kind = "mapped, zero copy";
            const MappedFile file{p};
            out = run_mapped(file, stats);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    const auto   t1 = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(t1 - t0).count();

    if (json) {
        report_json(stats, out, seconds, path);
    } else {
        report_text(stats, out, seconds, source_kind, path);
    }
    return out.status == FrameStatus::Malformed ? 1 : 0;
}
