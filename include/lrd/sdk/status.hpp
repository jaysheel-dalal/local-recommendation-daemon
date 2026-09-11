#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace lrd::sdk {

enum class StatusCode : std::uint8_t {
    Ok,

    /// The item does not exist. Not an error for get or delete - a caller
    /// asking "is this there" gets a truthful "no".
    NotFound,

    /// The caller's request was rejected. Retrying it unchanged will not help.
    InvalidArgument,

    /// The daemon could not be reached, or the connection was lost mid-call.
    /// Retrying may help; whether the SDK does so automatically depends on the
    /// operation - see RetryPolicy in client.hpp.
    Unavailable,

    /// A timeout elapsed. Distinct from Unavailable because the daemon may well
    /// be alive and merely slow, which is a different operational problem.
    DeadlineExceeded,

    /// The daemon failed internally.
    Internal,

    /// The daemon's reply did not make sense. Almost always a version mismatch
    /// between the two ends.
    ProtocolError,
};

[[nodiscard]] const char* to_string(StatusCode code) noexcept;

/// The result of one SDK call.
///
/// ## Why a returned value and not a member like Connection::last_error()
///
/// `Client` is shared across threads. A `last_error()` member would be shared
/// mutable state written by every call, so two threads failing concurrently
/// would each read the other's message - a data race that reports the wrong
/// cause, which is worse than reporting none. Returning the detail with the
/// result is the only shape that stays correct when the object is shared.
///
/// ## Why not exceptions
///
/// Three reasons, in order of weight:
///
///   1. A library that throws across its boundary forces every caller into the
///      same error-handling style, and imposes it transitively on their callers.
///   2. Exception types are part of the ABI. Throwing `lrd::SystemError` would
///      make that class's layout a compatibility surface, which is exactly what
///      the pimpl in client.hpp exists to avoid.
///   3. "The daemon is not running" and "this item does not exist" are ordinary
///      answers, not exceptional ones. Modelling them as throws makes the common
///      path the one with a try block around it.
///
/// The constructor of Client is the one exception to the no-exceptions rule, and
/// only because a constructor has no other way to fail - which is why connecting
/// is a factory function returning a Status instead.
class Status {
public:
    Status() = default;

    /// Named `success()` rather than `ok()` because `ok()` is already the
    /// predicate below, and a static factory cannot share a name with a const
    /// member function. absl solves the same collision with a free
    /// `absl::OkStatus()`; a differently-named factory keeps it on the type.
    static Status success() { return Status{}; }

    static Status make(StatusCode code, std::string message) {
        Status status;
        status.code_ = code;
        status.message_ = std::move(message);
        return status;
    }

    [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
    [[nodiscard]] StatusCode code() const noexcept { return code_; }

    /// Human-readable detail. Empty when ok().
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// Lets call sites read as `if (!status) { ... }`. Explicit so a Status
    /// never silently converts to bool anywhere else.
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    /// True when the item simply was not there, which most callers want to
    /// treat as a normal outcome rather than a failure.
    [[nodiscard]] bool is_not_found() const noexcept { return code_ == StatusCode::NotFound; }

private:
    StatusCode code_ = StatusCode::Ok;
    std::string message_;
};

}  // namespace lrd::sdk
