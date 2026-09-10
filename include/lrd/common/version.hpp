#pragma once

#include <string_view>

namespace lrd {

/// Semantic version of the daemon and wire protocol implementation.
/// Returned by std::string_view rather than std::string: the storage is a
/// compile-time literal that outlives every caller, so there is no reason to
/// allocate a copy on each call.
[[nodiscard]] std::string_view version() noexcept;

/// Short build description: version, compiler, C++ standard, sanitizer state.
/// Logged at daemon startup so a captured log always identifies its binary.
[[nodiscard]] std::string_view build_info() noexcept;

}  // namespace lrd
