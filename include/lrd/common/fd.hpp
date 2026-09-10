#pragma once

#include <utility>

namespace lrd {

/// Owning handle for a POSIX file descriptor.
///
/// A raw `int fd` is a resource with no destructor: every early return, every
/// thrown exception and every forgotten branch between open() and close() is a
/// leak. Wrapping it in a class ties the descriptor's lifetime to a scope, so
/// the compiler emits the close() for us on every exit path. This is RAII -
/// the same idea as std::unique_ptr, applied to a descriptor instead of memory.
///
/// Fd is *move-only*, deliberately. Copying would give two objects that each
/// believe they own the same descriptor, and the second destructor would close
/// a descriptor number that the kernel may already have handed out to an
/// unrelated open file - the classic double-close bug, which is genuinely
/// nasty because it corrupts an innocent file rather than crashing.
///
/// The alternative to deleting the copy would be a copy constructor that calls
/// dup(). That compiles and is even safe, but it makes an expensive syscall
/// invisible at the call site. Deleting the copy forces the caller to write
/// std::move (cheap, explicit transfer) or an explicit dup (expensive,
/// explicit duplication), which is the honest API.
class Fd {
public:
    static constexpr int kInvalid = -1;

    /// Constructs an empty handle owning nothing.
    Fd() noexcept = default;

    /// Takes ownership of `fd`. `explicit` so an int never converts to an
    /// owning Fd by accident - that would silently hand ownership to a
    /// temporary that closes the descriptor at the end of the statement.
    explicit Fd(int fd) noexcept : fd_(fd) {}

    ~Fd() { close(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    /// std::exchange sets `other` to kInvalid and returns its old value in one
    /// step, which is exactly the "steal and disarm" that a move needs: the
    /// moved-from object must not close the descriptor we just took.
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, kInvalid)) {}

    Fd& operator=(Fd&& other) noexcept {
        // Self-move must be a no-op. Without this guard, `a = std::move(a)`
        // would close the descriptor and then assign the now-invalid value
        // back to itself.
        if (this != &other) {
            close();  // release whatever we currently hold before overwriting
            fd_ = std::exchange(other.fd_, kInvalid);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ != kInvalid; }

    /// `explicit` so an Fd cannot silently decay to bool (and from there to
    /// int) in arithmetic or comparisons; it still works in `if (fd)`.
    explicit operator bool() const noexcept { return valid(); }

    /// Gives up ownership without closing, returning the raw descriptor.
    /// For the cases where a C API takes over the lifetime.
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, kInvalid); }

    /// Closes the current descriptor and adopts `fd`.
    void reset(int fd = kInvalid) noexcept {
        if (fd_ != fd) {
            close();
            fd_ = fd;
        }
    }

    /// Closes the descriptor if one is held. Safe to call repeatedly.
    void close() noexcept;

private:
    int fd_ = kInvalid;
};

}  // namespace lrd
