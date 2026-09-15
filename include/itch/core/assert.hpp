// Assertion levels used across the project.
//
// ITCH_ASSERT         always on, release included. A failure means a programming
//                     error whose consequence would otherwise be silent corruption
//                     (stale handle, exhausted pool, impossible message length).
// ITCH_INVARIANT      cheap O(1) structural checks. On at level >= 1.
// ITCH_SLOW_INVARIANT O(n) full-structure re-derivation. On at level >= 2.
//
// ITCH_INVARIANT_LEVEL is set by the build preset:
//   release           0
//   release-checked   1
//   asan-ubsan, tsan  2
#pragma once

#include <cstdint>

#ifndef ITCH_INVARIANT_LEVEL
#  ifdef NDEBUG
#    define ITCH_INVARIANT_LEVEL 0
#  else
#    define ITCH_INVARIANT_LEVEL 2
#  endif
#endif

namespace itch::detail {

struct AssertInfo {
    const char* file;
    int         line;
    const char* func;
    const char* expr;
    const char* msg;  // may be null
};

// Reports a failed assertion. The default handler prints to stderr and aborts.
// Tests install a handler that throws instead, so that "this must trip an
// assert" is an ordinary test case rather than a death test. If a handler
// returns normally, this function still aborts: an assertion must never be
// silently continued through.
[[gnu::cold]] void assert_failed(const AssertInfo& info);

using AssertHandler = void (*)(const AssertInfo&);

// Returns the previous handler. Passing nullptr restores the default.
AssertHandler set_assert_handler(AssertHandler handler);

}  // namespace itch::detail

#define ITCH_DETAIL_FAIL(expr_str, msg_ptr)                                        \
    ::itch::detail::assert_failed(                                                 \
        ::itch::detail::AssertInfo{__FILE__, __LINE__, __func__, expr_str, msg_ptr})

#define ITCH_ASSERT(expr)                                      \
    do {                                                       \
        if (!(expr)) [[unlikely]] {                            \
            ITCH_DETAIL_FAIL(#expr, nullptr);                  \
        }                                                      \
    } while (0)

#define ITCH_ASSERT_MSG(expr, msg)                             \
    do {                                                       \
        if (!(expr)) [[unlikely]] {                            \
            ITCH_DETAIL_FAIL(#expr, msg);                      \
        }                                                      \
    } while (0)

#if ITCH_INVARIANT_LEVEL >= 1
#  define ITCH_INVARIANT(expr) ITCH_ASSERT(expr)
#  define ITCH_INVARIANT_MSG(expr, msg) ITCH_ASSERT_MSG(expr, msg)
#else
#  define ITCH_INVARIANT(expr) ((void)0)
#  define ITCH_INVARIANT_MSG(expr, msg) ((void)0)
#endif

#if ITCH_INVARIANT_LEVEL >= 2
#  define ITCH_SLOW_INVARIANT(expr) ITCH_ASSERT(expr)
#  define ITCH_IF_SLOW(stmt) do { stmt; } while (0)
#else
#  define ITCH_SLOW_INVARIANT(expr) ((void)0)
#  define ITCH_IF_SLOW(stmt) ((void)0)
#endif
