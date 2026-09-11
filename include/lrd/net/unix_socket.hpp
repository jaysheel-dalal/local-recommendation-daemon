#pragma once

#include "lrd/common/fd.hpp"
#include "lrd/net/io.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace lrd::net {

/// The largest path a UNIX domain socket address can hold.
///
/// sockaddr_un::sun_path is a fixed char array - 108 bytes on Linux, 104 on
/// macOS - and the kernel gives you no help if the path is longer: bind()
/// either truncates silently or fails with a confusing error. Validating the
/// length ourselves, up front, turns a bizarre runtime failure into a clear
/// startup error. The -1 leaves room for the NUL terminator.
inline constexpr std::size_t kMaxSocketPathLength = 107;

/// A connected UNIX-domain stream socket.
///
/// Separate from UnixListener on purpose. A listening socket and a connected
/// socket support disjoint operations - you accept() one and read() the other,
/// and doing it the other way round is always a bug. Giving them distinct
/// types moves that mistake from a runtime EINVAL to a compile error, which is
/// the sort of thing a type system is for. (One class with an `is_listening`
/// flag would be the C way, and would need runtime checks everywhere.)
class UnixStream {
public:
    /// Connects to a daemon listening at `path`. Throws SystemError on
    /// failure; ECONNREFUSED here means "nothing is listening", which is the
    /// normal answer when the daemon is not running.
    [[nodiscard]] static UnixStream connect(std::string_view path);

    /// Adopts an already-connected descriptor - used by UnixListener::accept.
    explicit UnixStream(Fd fd) noexcept : fd_(std::move(fd)) {}

    /// Reads exactly `size` bytes. See lrd/net/io.hpp for why "exactly"
    /// requires a loop.
    IoResult read_exact(void* buffer, std::size_t size) noexcept {
        return net::read_exact(fd_.get(), buffer, size);
    }

    IoResult write_all(const void* buffer, std::size_t size) noexcept {
        return net::write_all(fd_.get(), buffer, size);
    }

    /// Reads whatever has arrived, up to `size` bytes.
    IoResult read_some(void* buffer, std::size_t size) noexcept {
        return net::read_some(fd_.get(), buffer, size);
    }

    /// Applies receive and send timeouts to this socket.
    ///
    /// A zero duration means "no timeout", which is the socket default and what
    /// the daemon uses - a worker blocked on the client it is dedicated to has
    /// nothing better to do, and shutdown interrupts it by other means (see
    /// ConnectionRegistry). The SDK sets them, because a library that can hang a
    /// caller's thread indefinitely is not shippable.
    ///
    /// Throws SystemError if the option cannot be set.
    void set_timeouts(std::chrono::milliseconds receive, std::chrono::milliseconds send);

    /// Half-closes the write side, so the peer's next read returns 0 and it
    /// learns we are done. Distinct from closing: we can still read whatever
    /// the peer sends afterwards.
    void shutdown_write() noexcept;

    [[nodiscard]] int native_handle() const noexcept { return fd_.get(); }
    [[nodiscard]] bool valid() const noexcept { return fd_.valid(); }
    void close() noexcept { fd_.close(); }

private:
    Fd fd_;
};

/// A listening UNIX-domain socket, plus ownership of the socket file itself.
///
/// The destructor unlinks the path. A UNIX socket leaves a real filesystem
/// entry behind, and an abandoned one makes the *next* bind() fail with
/// EADDRINUSE - so a daemon that crashed once refuses to start again until
/// someone deletes the file by hand. Tying the unlink to the object's lifetime
/// means the normal shutdown path cleans up automatically, and bind() below
/// handles the abnormal one.
class UnixListener {
public:
    /// Creates the socket, binds it to `path` and starts listening.
    ///
    /// Removes a *stale* socket file if one is in the way - see the
    /// implementation for how "stale" is established without racing a live
    /// daemon. The socket file is created with mode 0600, so only the owning
    /// user can connect: an on-device daemon holding local signals should not
    /// be reachable by every account on the machine.
    [[nodiscard]] static UnixListener bind(std::string_view path, int backlog = 128);

    ~UnixListener();

    UnixListener(const UnixListener&) = delete;
    UnixListener& operator=(const UnixListener&) = delete;
    UnixListener(UnixListener&&) noexcept;
    UnixListener& operator=(UnixListener&&) noexcept;

    /// Blocks until a client connects. Throws SystemError on a real failure.
    /// EINTR is retried internally, so a signal does not surface as an error.
    [[nodiscard]] UnixStream accept();

    [[nodiscard]] int native_handle() const noexcept { return fd_.get(); }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    /// Closes the listening socket and unlinks the path early, before the
    /// destructor would. Used by the shutdown path so the socket file is gone
    /// the moment we stop serving.
    void close() noexcept;

private:
    UnixListener(Fd fd, std::string path) noexcept : fd_(std::move(fd)), path_(std::move(path)) {}

    Fd fd_;
    std::string path_;
};

}  // namespace lrd::net
