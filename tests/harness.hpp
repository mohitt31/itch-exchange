// Minimal test harness. No external dependency, deliberately.
//
// What it has to support that a generic framework would not:
//   - ITCH_REQUIRE_ASSERT, so "this must trip an ITCH_ASSERT" is a normal test
//     rather than a death test;
//   - ITCH_TEST_CONTEXT, so a seeded property test prints the seed that failed
//     and nothing else has to be reconstructed by hand.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace itch::test {

// Thrown by a failing REQUIRE. Aborts the current test only.
struct Failure {
    std::string msg;
};

// Thrown by the assert handler installed by AssertTrap.
struct AssertFired {
    std::string expr;
};

using TestFn = void (*)();

void register_test(const char* name, TestFn fn, const char* file, int line);

struct Registrar {
    Registrar(const char* name, TestFn fn, const char* file, int line) {
        register_test(name, fn, file, line);
    }
};

// Pushes a note that is printed if anything in the enclosing scope fails.
class Context {
public:
    explicit Context(std::string note);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
};

// Installs an assert handler that throws AssertFired instead of aborting.
class AssertTrap {
public:
    AssertTrap();
    ~AssertTrap();
    AssertTrap(const AssertTrap&) = delete;
    AssertTrap& operator=(const AssertTrap&) = delete;

private:
    void* prev_;
};

void report_failure(const char* file, int line, const std::string& msg, bool fatal);

// Deterministic generator for property tests. splitmix64: tiny, well
// distributed, and identical on every platform, which matters because a failing
// seed has to reproduce on a different machine.
class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed) noexcept : s_(seed) {}

    constexpr std::uint64_t next() noexcept {
        std::uint64_t z = (s_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    // Uniform in [lo, hi]. Uses the multiply-shift reduction, which is slightly
    // biased; the bias is far below anything a test can observe and it avoids a
    // rejection loop that would make run length seed-dependent.
    constexpr std::uint64_t range(std::uint64_t lo, std::uint64_t hi) noexcept {
        const std::uint64_t span = hi - lo + 1;
        return lo + static_cast<std::uint64_t>((static_cast<__uint128_t>(next()) * span) >> 64);
    }

    constexpr bool chance(std::uint32_t percent) noexcept { return range(0, 99) < percent; }

private:
    std::uint64_t s_;
};

// Hides a value from the optimiser.
//
// Tests that deliberately pass an invalid handle or index mean a value that
// arrives at runtime, not one the compiler can see. Left visible, GCC constant
// folds it into the call site and then reports the out-of-bounds subscript that
// the assertion under test exists to prevent -- a correct warning about a path
// that cannot execute. This makes the value opaque, which is also a more honest
// model of the bug being tested.
template <class T>
[[nodiscard]] T opaque(T v) {
    asm volatile("" : "+r"(v));
    return v;
}

// Value formatting for failure messages.
template <class T>
std::string to_str(const T& v) {
    if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_same_v<std::decay_t<T>, char>) {
        return std::string("'") + v + "'";
    } else if constexpr (std::is_enum_v<std::decay_t<T>>) {
        return std::to_string(static_cast<long long>(v));
    } else if constexpr (std::is_integral_v<std::decay_t<T>>) {
        if constexpr (std::is_signed_v<std::decay_t<T>>) {
            return std::to_string(static_cast<long long>(v));
        } else {
            return std::to_string(static_cast<unsigned long long>(v));
        }
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
        return std::string(std::string_view(v));
    } else if constexpr (std::is_pointer_v<std::decay_t<T>>) {
        return v ? "<non-null>" : "<null>";
    } else {
        return "<value>";
    }
}

template <class A, class B>
std::string describe(const char* a_txt, const char* op, const char* b_txt, const A& a,
                     const B& b) {
    return std::string(a_txt) + " " + op + " " + b_txt + "\n    left  = " + to_str(a) +
           "\n    right = " + to_str(b);
}

}  // namespace itch::test

#define ITCH_TEST(name)                                                                \
    static void itch_test_fn_##name();                                                 \
    static const ::itch::test::Registrar itch_test_reg_##name{#name, &itch_test_fn_##name, \
                                                              __FILE__, __LINE__};     \
    static void itch_test_fn_##name()

// Two levels, because ## suppresses expansion of its operands: a single level
// would paste the literal text __LINE__ and two contexts in one scope would
// collide instead of nesting.
#define ITCH_DETAIL_CAT2(a, b) a##b
#define ITCH_DETAIL_CAT(a, b) ITCH_DETAIL_CAT2(a, b)

#define ITCH_TEST_CONTEXT(note) \
    const ::itch::test::Context ITCH_DETAIL_CAT(itch_ctx_, __LINE__) { note }

#define ITCH_DETAIL_BOOL(expr, fatal)                                                  \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            ::itch::test::report_failure(__FILE__, __LINE__,                           \
                                         std::string("expected: ") + #expr, fatal);    \
        }                                                                              \
    } while (0)

#define ITCH_DETAIL_BINOP(a, op, b, fatal)                                             \
    do {                                                                               \
        auto&& itch_lhs = (a);                                                         \
        auto&& itch_rhs = (b);                                                         \
        if (!(itch_lhs op itch_rhs)) {                                                 \
            ::itch::test::report_failure(                                              \
                __FILE__, __LINE__,                                                    \
                ::itch::test::describe(#a, #op, #b, itch_lhs, itch_rhs), fatal);       \
        }                                                                              \
    } while (0)

#define ITCH_CHECK(expr) ITCH_DETAIL_BOOL(expr, false)
#define ITCH_REQUIRE(expr) ITCH_DETAIL_BOOL(expr, true)

#define ITCH_CHECK_EQ(a, b) ITCH_DETAIL_BINOP(a, ==, b, false)
#define ITCH_CHECK_NE(a, b) ITCH_DETAIL_BINOP(a, !=, b, false)
#define ITCH_CHECK_LT(a, b) ITCH_DETAIL_BINOP(a, <, b, false)
#define ITCH_CHECK_LE(a, b) ITCH_DETAIL_BINOP(a, <=, b, false)
#define ITCH_CHECK_GT(a, b) ITCH_DETAIL_BINOP(a, >, b, false)
#define ITCH_CHECK_GE(a, b) ITCH_DETAIL_BINOP(a, >=, b, false)

#define ITCH_REQUIRE_EQ(a, b) ITCH_DETAIL_BINOP(a, ==, b, true)
#define ITCH_REQUIRE_NE(a, b) ITCH_DETAIL_BINOP(a, !=, b, true)
#define ITCH_REQUIRE_LT(a, b) ITCH_DETAIL_BINOP(a, <, b, true)
#define ITCH_REQUIRE_LE(a, b) ITCH_DETAIL_BINOP(a, <=, b, true)
#define ITCH_REQUIRE_GT(a, b) ITCH_DETAIL_BINOP(a, >, b, true)
#define ITCH_REQUIRE_GE(a, b) ITCH_DETAIL_BINOP(a, >=, b, true)

// Asserts that the statement trips an ITCH_ASSERT.
#define ITCH_REQUIRE_ASSERT(stmt)                                                      \
    do {                                                                               \
        bool itch_fired = false;                                                       \
        {                                                                              \
            const ::itch::test::AssertTrap itch_trap;                                  \
            try {                                                                      \
                stmt;                                                                  \
            } catch (const ::itch::test::AssertFired&) {                               \
                itch_fired = true;                                                     \
            }                                                                          \
        }                                                                              \
        if (!itch_fired) {                                                             \
            ::itch::test::report_failure(                                              \
                __FILE__, __LINE__,                                                    \
                std::string("expected an assertion from: ") + #stmt, true);            \
        }                                                                              \
    } while (0)
