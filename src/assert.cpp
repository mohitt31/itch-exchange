#include "itch/core/assert.hpp"

#include <cstdio>
#include <cstdlib>

namespace itch::detail {
namespace {

void default_handler(const AssertInfo& info) {
    std::fprintf(stderr, "%s:%d: %s: assertion failed: %s\n", info.file, info.line,
                 info.func, info.expr);
    if (info.msg != nullptr) {
        std::fprintf(stderr, "  %s\n", info.msg);
    }
    std::fflush(stderr);
}

AssertHandler g_handler = &default_handler;

}  // namespace

AssertHandler set_assert_handler(AssertHandler handler) {
    AssertHandler prev = g_handler;
    g_handler = (handler != nullptr) ? handler : &default_handler;
    return prev;
}

void assert_failed(const AssertInfo& info) {
    g_handler(info);
    // Reached only if the handler returned rather than throwing or exiting.
    // An assertion is never continued through.
    std::abort();
}

}  // namespace itch::detail
