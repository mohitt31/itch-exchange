#include "harness.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "itch/core/assert.hpp"

namespace itch::test {
namespace {

struct Entry {
    const char* name;
    TestFn      fn;
    const char* file;
    int         line;
};

std::vector<Entry>& registry() {
    static std::vector<Entry> r;
    return r;
}

std::vector<std::string>& context_stack() {
    static std::vector<std::string> c;
    return c;
}

int  g_failures_in_test = 0;
bool g_any_failure = false;

void print_context() {
    for (const std::string& note : context_stack()) {
        std::fprintf(stderr, "    context: %s\n", note.c_str());
    }
}

void throwing_assert_handler(const ::itch::detail::AssertInfo& info) {
    throw AssertFired{info.expr};
}

}  // namespace

void register_test(const char* name, TestFn fn, const char* file, int line) {
    registry().push_back(Entry{name, fn, file, line});
}

Context::Context(std::string note) { context_stack().push_back(std::move(note)); }
Context::~Context() { context_stack().pop_back(); }

AssertTrap::AssertTrap()
    : prev_(reinterpret_cast<void*>(
          ::itch::detail::set_assert_handler(&throwing_assert_handler))) {}

AssertTrap::~AssertTrap() {
    ::itch::detail::set_assert_handler(
        reinterpret_cast<::itch::detail::AssertHandler>(prev_));
}

void report_failure(const char* file, int line, const std::string& msg, bool fatal) {
    ++g_failures_in_test;
    g_any_failure = true;
    std::fprintf(stderr, "  %s at %s:%d\n    %s\n", fatal ? "REQUIRE failed" : "CHECK failed",
                 file, line, msg.c_str());
    print_context();
    if (fatal) {
        throw Failure{msg};
    }
}

}  // namespace itch::test

int main(int argc, char** argv) {
    using namespace itch::test;

    const char* filter = nullptr;
    bool        list_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else if (std::strncmp(argv[i], "--", 2) != 0) {
            filter = argv[i];
        }
    }

    auto& tests = registry();
    std::stable_sort(tests.begin(), tests.end(),
                     [](const Entry& a, const Entry& b) {
                         return std::strcmp(a.name, b.name) < 0;
                     });

    if (list_only) {
        for (const Entry& e : tests) {
            std::printf("%s\n", e.name);
        }
        return 0;
    }

    int ran = 0;
    int failed = 0;
    for (const Entry& e : tests) {
        if (filter != nullptr && std::strstr(e.name, filter) == nullptr) {
            continue;
        }
        ++ran;
        g_failures_in_test = 0;
        std::fprintf(stderr, "[ run    ] %s\n", e.name);
        try {
            e.fn();
        } catch (const Failure&) {
            // already reported
        } catch (const AssertFired& a) {
            report_failure(e.file, e.line,
                           std::string("unexpected assertion: ") + a.expr, false);
        } catch (const std::exception& ex) {
            report_failure(e.file, e.line,
                           std::string("unexpected exception: ") + ex.what(), false);
        } catch (...) {
            report_failure(e.file, e.line, "unexpected unknown exception", false);
        }
        if (g_failures_in_test > 0) {
            ++failed;
            std::fprintf(stderr, "[   FAIL ] %s\n", e.name);
        } else {
            std::fprintf(stderr, "[     ok ] %s\n", e.name);
        }
    }

    std::fprintf(stderr, "\n%d run, %d failed\n", ran, failed);
    if (ran == 0) {
        std::fprintf(stderr, "no tests matched\n");
        return 2;
    }
    return failed == 0 ? 0 : 1;
}
