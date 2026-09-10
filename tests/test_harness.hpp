#pragma once

// A small test harness with zero external dependencies.
//
// Why not GoogleTest? Phase 1 is meant to be `git clone && cmake && ctest` with
// no network access and no third-party build integration to debug. GoogleTest
// buys parameterised fixtures and death tests, neither of which this phase
// needs. If the suite outgrows this, swapping in GoogleTest is mechanical:
// the assertion macros below map one-to-one onto EXPECT_* / ASSERT_*.

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace lrd::test {

/// Thrown by LRD_REQUIRE to abandon the current test case. A failed
/// precondition usually makes every following assertion meaningless noise, so
/// REQUIRE aborts the case while CHECK keeps going and collects more evidence.
struct RequirementFailed : std::exception {
    const char* what() const noexcept override { return "requirement failed"; }
};

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

/// Function-local static, not a namespace-scope global: this sidesteps the
/// static initialisation order fiasco. Test cases register themselves from
/// other translation units' static constructors, and a namespace-scope vector
/// might not be constructed yet when they do. A function-local static is
/// constructed on first use, which is exactly at registration time.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& failure_count() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back(TestCase{std::move(name), std::move(fn)});
    }
};

inline void report_failure(const char* file, int line, const std::string& message) {
    ++failure_count();
    std::cout << "    FAIL " << file << ":" << line << "  " << message << "\n";
}

/// Renders a value for failure output when it is streamable, and falls back to
/// a placeholder when it is not. `if constexpr` means the unstreamable branch is
/// never instantiated, so a type with no operator<< still compiles.
template <typename T>
std::string describe(const T& value) {
    if constexpr (requires(std::ostringstream& os, const T& v) { os << v; }) {
        std::ostringstream os;
        os << value;
        return os.str();
    } else {
        return "<not printable>";
    }
}

int run_all();

}  // namespace lrd::test

// Token pasting through two levels so __LINE__ expands before concatenation.
#define LRD_CONCAT_INNER(a, b) a##b
#define LRD_CONCAT(a, b) LRD_CONCAT_INNER(a, b)

/// Defines and registers a test case:
///     LRD_TEST("lru evicts the oldest entry") { ... }
#define LRD_TEST(name)                                                       \
    static void LRD_CONCAT(lrd_test_fn_, __LINE__)();                        \
    static const ::lrd::test::Registrar LRD_CONCAT(lrd_test_reg_, __LINE__){ \
        name, &LRD_CONCAT(lrd_test_fn_, __LINE__)};                          \
    static void LRD_CONCAT(lrd_test_fn_, __LINE__)()

#define LRD_CHECK(expr)                                                     \
    do {                                                                    \
        if (!(expr)) {                                                      \
            ::lrd::test::report_failure(__FILE__, __LINE__,                 \
                                        "expected: " #expr);                \
        }                                                                   \
    } while (false)

#define LRD_CHECK_EQ(a, b)                                                       \
    do {                                                                         \
        const auto& lrd_lhs_ = (a);                                              \
        const auto& lrd_rhs_ = (b);                                              \
        if (!(lrd_lhs_ == lrd_rhs_)) {                                           \
            ::lrd::test::report_failure(                                         \
                __FILE__, __LINE__,                                              \
                std::string(#a " == " #b "  (got ") +                            \
                    ::lrd::test::describe(lrd_lhs_) + " vs " +                   \
                    ::lrd::test::describe(lrd_rhs_) + ")");                      \
        }                                                                        \
    } while (false)

#define LRD_REQUIRE(expr)                                                   \
    do {                                                                    \
        if (!(expr)) {                                                      \
            ::lrd::test::report_failure(__FILE__, __LINE__,                 \
                                        "required: " #expr);                \
            throw ::lrd::test::RequirementFailed{};                         \
        }                                                                   \
    } while (false)
