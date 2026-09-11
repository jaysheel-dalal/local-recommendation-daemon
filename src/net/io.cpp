#include "lrd/net/io.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace lrd::net {

namespace {

// std::byte would be the idiomatic "raw memory" type, but pointer arithmetic
// is what we need here and unsigned char is the type the socket API speaks.
// Either way the cast has to be reinterpret_cast: converting void* to a typed
// pointer is exactly the "trust me about the bytes" operation it exists for.
unsigned char* as_bytes(void* p) noexcept {
    return reinterpret_cast<unsigned char*>(p);
}

const unsigned char* as_bytes(const void* p) noexcept {
    return reinterpret_cast<const unsigned char*>(p);
}

}  // namespace

IoResult read_exact(int fd, void* buffer, std::size_t size) noexcept {
    IoResult result;
    unsigned char* cursor = as_bytes(buffer);

    while (result.transferred < size) {
        const std::size_t remaining = size - result.transferred;
        const ssize_t n = ::read(fd, cursor + result.transferred, remaining);

        if (n > 0) {
            // n is positive and bounded by `remaining`, so this narrowing is
            // safe - but -Wconversion is on for a reason, so it is written out
            // rather than left implicit.
            result.transferred += static_cast<std::size_t>(n);
            continue;
        }

        if (n == 0) {
            // Zero from read() on a stream socket means end-of-stream: the
            // peer called close() or shutdown(SHUT_WR). It is not an error and
            // errno is not set, so it must be checked separately - forgetting
            // this is how a reader ends up spinning on a dead socket.
            result.status = IoStatus::PeerClosed;
            return result;
        }

        // n < 0
        if (errno == EINTR) {
            // A signal arrived before any data was transferred. Nothing has
            // been lost; just reissue the call. Without this retry the daemon
            // would report spurious failures every time it is sent a signal.
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // SO_RCVTIMEO elapsed. Deliberately *not* retried: the point of
            // setting a timeout is that the caller wanted to stop waiting, and
            // looping here would silently restore the unbounded wait it was
            // configured to avoid.
            //
            // EAGAIN and EWOULDBLOCK are the same value on Linux but are not
            // required to be, so both are named.
            result.status = IoStatus::TimedOut;
            result.error = errno;
            return result;
        }

        result.status = IoStatus::Error;
        result.error = errno;
        return result;
    }

    return result;
}

IoResult read_some(int fd, void* buffer, std::size_t size) noexcept {
    IoResult result;

    for (;;) {
        const ssize_t n = ::read(fd, buffer, size);
        if (n > 0) {
            result.transferred = static_cast<std::size_t>(n);
            return result;
        }
        if (n == 0) {
            result.status = IoStatus::PeerClosed;
            return result;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            result.status = IoStatus::TimedOut;
            result.error = errno;
            return result;
        }
        result.status = IoStatus::Error;
        result.error = errno;
        return result;
    }
}

IoResult write_all(int fd, const void* buffer, std::size_t size) noexcept {
    IoResult result;
    const unsigned char* cursor = as_bytes(buffer);

    while (result.transferred < size) {
        const std::size_t remaining = size - result.transferred;

        // send() with MSG_NOSIGNAL rather than write().
        //
        // Writing to a socket whose peer has closed raises SIGPIPE, whose
        // default disposition is to terminate the process. A daemon that dies
        // because a client pressed Ctrl-C is not a daemon. MSG_NOSIGNAL
        // suppresses the signal for this call and makes the error visible as
        // EPIPE instead, which is what we can actually handle.
        //
        // The alternatives are worth knowing: signal(SIGPIPE, SIG_IGN)
        // process-wide works but reaches into global state a library has no
        // business owning, and on macOS/iOS the per-socket equivalent is
        // setsockopt(SO_NOSIGPIPE) because MSG_NOSIGNAL is Linux-specific.
        // Same idea, different spelling - worth noting since this code is
        // meant to map onto Darwin.
        const ssize_t n = ::send(fd, cursor + result.transferred, remaining, MSG_NOSIGNAL);

        if (n > 0) {
            result.transferred += static_cast<std::size_t>(n);
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n < 0 && errno == EPIPE) {
            // The peer is gone. Routine, not a fault: report it the same way
            // as a clean read-side shutdown so callers have one case to handle.
            result.status = IoStatus::PeerClosed;
            return result;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // SO_SNDTIMEO elapsed - the send buffer stayed full longer than the
            // caller was willing to wait.
            result.status = IoStatus::TimedOut;
            result.error = errno;
            return result;
        }

        result.status = IoStatus::Error;
        result.error = (n < 0) ? errno : 0;
        return result;
    }

    return result;
}

}  // namespace lrd::net
