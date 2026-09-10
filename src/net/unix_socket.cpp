#include "lrd/net/unix_socket.hpp"

#include "lrd/common/errors.hpp"
#include "lrd/common/log.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <utility>

namespace lrd::net {

namespace {

/// Fills a sockaddr_un for `path`, validating the length first.
sockaddr_un make_address(std::string_view path) {
    if (path.empty()) {
        throw SystemError(EINVAL, "socket path is empty");
    }
    if (path.size() > kMaxSocketPathLength) {
        throw SystemError(ENAMETOOLONG,
                          std::format("socket path is {} bytes, limit is {}", path.size(),
                                      kMaxSocketPathLength));
    }

    sockaddr_un addr{};
    // Zeroing the whole struct matters: sun_path must end up NUL-terminated,
    // and value-initialisation ({}) does that for every byte we do not write.
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.data(), path.size());
    return addr;
}

/// The address length to hand to bind()/connect().
socklen_t address_length(const sockaddr_un& addr) noexcept {
    // offsetof + strlen + 1 rather than sizeof(sockaddr_un): passing the exact
    // length is what the kernel expects for pathname sockets, and it is the
    // form that also works for abstract-namespace sockets should we ever want
    // them. sizeof() happens to work on Linux but says "108 bytes of path"
    // when we mean "this many".
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                  std::strlen(addr.sun_path) + 1);
}

/// SOCK_CLOEXEC on creation, not fcntl(FD_CLOEXEC) afterwards.
///
/// The two-step version has a race: between socket() and fcntl(), another
/// thread can fork+exec and the child inherits the descriptor. For a daemon
/// holding a listening socket that means a completely unrelated child process
/// keeps the socket alive after the daemon exits. Creating with the flag set
/// closes the window - there is no window.
Fd make_socket() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw_errno("socket(AF_UNIX, SOCK_STREAM)");
    }
    return Fd(fd);
}

/// Decides whether an existing socket file is a live daemon or debris.
///
/// The naive fix for EADDRINUSE is to unlink the path and bind again. That is
/// dangerous: if a daemon really is running, the unlink silently steals its
/// address - the old process keeps its socket open but no new client can ever
/// reach it, and you have two daemons with one of them invisible.
///
/// So we ask instead of assuming. Connecting to the path is the only reliable
/// probe: if something accepts, a daemon is alive and we must refuse to start.
/// ECONNREFUSED means the file exists but nothing is bound to it - a leftover
/// from a process that died without cleaning up - and that one is safe to
/// remove.
bool socket_file_is_stale(std::string_view path) {
    const sockaddr_un addr = make_address(path);
    const Fd probe = make_socket();

    if (::connect(probe.get(), reinterpret_cast<const sockaddr*>(&addr), address_length(addr)) ==
        0) {
        return false;  // someone is listening
    }
    return errno == ECONNREFUSED;
}

}  // namespace

// --------------------------------------------------------------------------
// UnixStream
// --------------------------------------------------------------------------

UnixStream UnixStream::connect(std::string_view path) {
    const sockaddr_un addr = make_address(path);
    Fd fd = make_socket();

    while (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), address_length(addr)) !=
           0) {
        if (errno == EINTR) {
            continue;
        }
        throw_errno(std::format("connect({})", path));
    }

    return UnixStream(std::move(fd));
}

void UnixStream::shutdown_write() noexcept {
    if (fd_.valid()) {
        // ENOTCONN here is expected and uninteresting: it just means the peer
        // already went away, which is exactly when we would be shutting down.
        ::shutdown(fd_.get(), SHUT_WR);
    }
}

// --------------------------------------------------------------------------
// UnixListener
// --------------------------------------------------------------------------

UnixListener UnixListener::bind(std::string_view path, int backlog) {
    if (::access(std::string(path).c_str(), F_OK) == 0) {
        if (!socket_file_is_stale(path)) {
            throw SystemError(EADDRINUSE,
                              std::format("{} is already served by a running daemon", path));
        }
        log_warn("removing stale socket file {}", path);
        if (::unlink(std::string(path).c_str()) != 0 && errno != ENOENT) {
            throw_errno(std::format("unlink({})", path));
        }
    }

    const sockaddr_un addr = make_address(path);
    Fd fd = make_socket();

    // Permissions on the socket file decide who may connect. We want 0600:
    // this daemon holds on-device signals and only its own user should reach
    // it. There is no fchmod-before-bind for UNIX sockets - the file does not
    // exist until bind() creates it - so the choices are umask around bind()
    // (no window where the socket is world-accessible) or chmod() after bind()
    // (a brief window where it is). We take the umask route.
    //
    // The honest caveat: umask is process-global and not thread-safe. This is
    // safe here only because bind() happens once during startup, before any
    // worker threads exist. If that ever stops being true, this needs to move
    // into a private directory whose permissions do the work instead.
    const mode_t previous_umask = ::umask(0177);
    const int bind_rc =
        ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), address_length(addr));
    const int bind_errno = errno;
    ::umask(previous_umask);

    if (bind_rc != 0) {
        throw_errno(bind_errno, std::format("bind({})", path));
    }

    if (::listen(fd.get(), backlog) != 0) {
        const int listen_errno = errno;
        // bind() already created the file; if we fail now, do not leave it
        // behind to confuse the next startup.
        ::unlink(std::string(path).c_str());
        throw_errno(listen_errno, std::format("listen({})", path));
    }

    log_info("listening on {} (backlog {})", path, backlog);
    return UnixListener(std::move(fd), std::string(path));
}

UnixListener::UnixListener(UnixListener&& other) noexcept
    : fd_(std::move(other.fd_)), path_(std::move(other.path_)) {
    // Clearing the moved-from path is what stops its destructor from unlinking
    // the socket file that this object now owns. std::string's own move leaves
    // the source unspecified-but-valid, which is not the same as empty.
    other.path_.clear();
}

UnixListener& UnixListener::operator=(UnixListener&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::move(other.fd_);
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

UnixListener::~UnixListener() {
    close();
}

void UnixListener::close() noexcept {
    fd_.close();
    if (!path_.empty()) {
        ::unlink(path_.c_str());
        path_.clear();
    }
}

UnixStream UnixListener::accept() {
    for (;;) {
        // accept4 rather than accept, for the same SOCK_CLOEXEC reason as
        // make_socket(): the accepted descriptor does not inherit the
        // listener's flags, so without this every connection is a fresh chance
        // to leak a descriptor into a child process.
        const int client = ::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (client >= 0) {
            return UnixStream(Fd(client));
        }

        if (errno == EINTR) {
            continue;
        }

        // ECONNABORTED means this particular client vanished between the
        // kernel queuing it and us accepting it. The listener is fine; the
        // caller decides whether to loop. Reported as an exception so it is
        // never silently swallowed.
        throw_errno("accept4");
    }
}

}  // namespace lrd::net
