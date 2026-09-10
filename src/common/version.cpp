#include "lrd/common/version.hpp"

#include <string>

namespace lrd {

namespace {

// Stringify a macro's *value* rather than its name: LRD_STR(__cplusplus)
// expands to "202002L" and not "__cplusplus". The two-level indirection is the
// standard trick - the outer macro forces argument expansion before the inner
// one applies the # operator.
#define LRD_STR_INNER(x) #x
#define LRD_STR(x) LRD_STR_INNER(x)

#if defined(__SANITIZE_THREAD__)
constexpr const char* kSanitizer = " [tsan]";
#elif defined(__SANITIZE_ADDRESS__)
constexpr const char* kSanitizer = " [asan]";
#else
constexpr const char* kSanitizer = "";
#endif

// Function-local static: built once, on first call, in a thread-safe way that
// the compiler guarantees since C++11 ("magic statics"). Its storage lives
// until process exit, which is what makes returning a string_view into it safe.
const std::string& info_storage() {
    static const std::string kInfo = std::string("lrd ") + LRD_VERSION + " (C++" +
                                     LRD_STR(__cplusplus) + ", gcc " + __VERSION__ + ")" +
                                     kSanitizer;
    return kInfo;
}

}  // namespace

std::string_view version() noexcept {
    // LRD_VERSION is a string literal from CMake, so this string_view points at
    // static storage and never dangles.
    return LRD_VERSION;
}

std::string_view build_info() noexcept {
    return info_storage();
}

}  // namespace lrd
