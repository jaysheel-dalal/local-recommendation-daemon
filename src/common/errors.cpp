#include "lrd/common/errors.hpp"

#include <cerrno>
#include <format>

namespace lrd {

namespace {

// std::generic_category() is the right category for errno values on POSIX.
// std::system_category() is for native OS error codes; on Linux the two
// happen to coincide numerically, but generic_category() is what makes
// `ec == std::errc::connection_refused` compare correctly and portably.
std::error_code make_errno_code(int errno_value) {
    return std::error_code(errno_value, std::generic_category());
}

}  // namespace

SystemError::SystemError(int errno_value, std::string_view context)
    : std::system_error(make_errno_code(errno_value), std::string(context)) {}

std::string describe_errno(int errno_value, std::string_view context) {
    return std::format("{}: {} (errno={})", context, make_errno_code(errno_value).message(),
                       errno_value);
}

void throw_errno(std::string_view context) {
    // Snapshot errno before anything else can touch it.
    const int saved = errno;
    throw SystemError(saved, context);
}

void throw_errno(int errno_value, std::string_view context) {
    throw SystemError(errno_value, context);
}

}  // namespace lrd
