#include "lrd/common/version.hpp"

#include "test_harness.hpp"

#include <string>
#include <string_view>

// Step 0's only test: it exists to prove the harness, the CTest wiring and the
// link against lrd_core all work before anything real depends on them.

LRD_TEST("version is non-empty and dotted") {
    const std::string_view v = lrd::version();
    LRD_REQUIRE(!v.empty());
    LRD_CHECK(v.find('.') != std::string_view::npos);
}

LRD_TEST("build_info mentions the version and the language level") {
    const std::string info(lrd::build_info());
    LRD_CHECK(info.find(std::string(lrd::version())) != std::string::npos);
    LRD_CHECK(info.find("C++2") != std::string::npos);
}
