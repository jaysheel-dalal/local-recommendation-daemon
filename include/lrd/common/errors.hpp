#pragma once

#include <string>
#include <string_view>
#include <system_error>

namespace lrd {

/// Error policy for this codebase, stated once:
///
///   * Setup failures throw. Binding a socket, creating a thread pool,
///     resolving a path - these happen once, at startup, and there is no
///     sensible local recovery. An exception carries the errno and the context
///     up to main() where it becomes an error message and a non-zero exit.
///
///   * Per-request I/O returns a status. Read and write on a connection fail
///     routinely and for boring reasons (the peer went away), on a hot path,
///     inside a worker thread that must keep serving other connections.
///     Throwing there would mean a try/catch in the inner loop and would make
///     "peer disconnected" - a normal event - look exceptional.
///
/// See lrd/net/io.hpp for the returning half of that policy.

/// A std::system_error carrying an errno value plus the operation that failed.
/// Deriving rather than using system_error directly gives call sites a type to
/// catch that means specifically "a syscall failed", distinct from any other
/// system_error a library might throw.
class SystemError : public std::system_error {
public:
    SystemError(int errno_value, std::string_view context);

    /// The errno value as captured at the failure site.
    [[nodiscard]] int error_number() const noexcept { return code().value(); }
};

/// Throws SystemError built from the current `errno`.
///
/// Call this immediately after the failing syscall: errno is only meaningful
/// until the next library call touches it, and even building the message could
/// in principle clobber it. The function reads errno first, then formats.
[[noreturn]] void throw_errno(std::string_view context);

/// Throws SystemError for an explicitly supplied errno value, for the cases
/// where the value was saved before some intervening cleanup.
[[noreturn]] void throw_errno(int errno_value, std::string_view context);

/// Renders an errno value as "context: message (errno=N)".
[[nodiscard]] std::string describe_errno(int errno_value, std::string_view context);

}  // namespace lrd
