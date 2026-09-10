#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace lrd::daemon {

/// Tracks live connections so shutdown can interrupt them.
///
/// ## The problem it solves
///
/// A worker serving a connection spends almost all its time blocked in read(),
/// waiting for the next request. Nothing about setting a "please stop" flag
/// wakes it: the thread is in the kernel, and it will stay there until the
/// client sends something or disconnects. A daemon that waits for that on
/// shutdown is a daemon that hangs until its last client gets bored.
///
/// The fix is to make the read fail. `shutdown(fd, SHUT_RDWR)` on a socket
/// another thread is blocked reading causes that read to return 0, exactly as
/// if the peer had hung up - which the connection loop already knows how to
/// handle. So shutdown needs a list of the descriptors to do that to, and that
/// list is this class.
///
/// Note it is `shutdown()` and not `close()`. Closing a descriptor that another
/// thread is actively using is a use-after-free with extra steps: the number
/// can be reissued by the kernel to a completely unrelated open file the
/// instant it is freed, and the blocked thread would then be reading someone
/// else's socket. `shutdown()` breaks the connection while leaving the
/// descriptor owned by its Fd, which closes it in the normal way afterwards.
///
/// ## Why the mutex is held across the shutdown() syscall
///
/// It closes the same race from the other side. A connection deregisters
/// *before* its Fd is closed, and both steps take this mutex, so
/// `stop_all()` can never be looking at a descriptor number that has already
/// been closed and recycled.
class ConnectionRegistry {
public:
    /// Registers `fd` and returns a non-zero token for remove().
    ///
    /// Returns 0 if the registry is already stopping, meaning the caller should
    /// abandon this connection immediately. That case is real: a connection can
    /// be popped off the thread pool's queue microseconds after stop_all() has
    /// already walked the list, and without this check it would be served
    /// happily while the daemon is trying to exit.
    [[nodiscard]] std::uint64_t add(int fd);

    void remove(std::uint64_t token) noexcept;

    /// Marks the registry stopped and breaks every live connection.
    void stop_all() noexcept;

    [[nodiscard]] bool stopping() const;
    [[nodiscard]] std::size_t active() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, int> connections_;
    std::uint64_t next_token_ = 1;
    bool stopping_ = false;
};

/// RAII registration. Registers on construction, deregisters on destruction -
/// including on the error paths out of a connection loop, of which there are
/// several.
class ConnectionGuard {
public:
    ConnectionGuard(ConnectionRegistry& registry, int fd)
        : registry_(registry), token_(registry.add(fd)) {}

    ~ConnectionGuard() {
        if (token_ != 0) {
            registry_.remove(token_);
        }
    }

    ConnectionGuard(const ConnectionGuard&) = delete;
    ConnectionGuard& operator=(const ConnectionGuard&) = delete;

    /// False if the daemon is shutting down and this connection should not be
    /// served at all.
    [[nodiscard]] explicit operator bool() const noexcept { return token_ != 0; }

private:
    ConnectionRegistry& registry_;
    std::uint64_t token_;
};

}  // namespace lrd::daemon
