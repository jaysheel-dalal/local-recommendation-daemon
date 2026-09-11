#pragma once

#include <cstddef>
#include <cstdint>

namespace lrd::net {

enum class IoStatus : std::uint8_t {
    Ok,          ///< The full requested byte count was transferred.
    PeerClosed,  ///< The peer performed an orderly shutdown mid-transfer.

    /// A socket timeout elapsed (SO_RCVTIMEO / SO_SNDTIMEO).
    ///
    /// Distinct from Error because the socket is still perfectly healthy - the
    /// peer was merely slow. What makes it dangerous is `transferred`: a timeout
    /// part-way through a message leaves the stream desynchronised, because the
    /// bytes already consumed cannot be put back. Callers must treat a
    /// mid-message timeout as fatal to the connection even though nothing is
    /// broken at the socket level.
    TimedOut,

    Error,  ///< A syscall failed; see IoResult::error for the errno value.
};

/// Result of a whole-buffer transfer.
///
/// `transferred` matters even on failure: it tells a caller how far into the
/// buffer the connection died, which is what distinguishes "the peer closed
/// cleanly between messages" (transferred == 0, normal) from "the peer died
/// halfway through a message" (transferred > 0, a truncated frame).
struct IoResult {
    IoStatus status = IoStatus::Ok;
    int error = 0;  ///< errno value when status == Error, otherwise 0.
    std::size_t transferred = 0;

    [[nodiscard]] bool ok() const noexcept { return status == IoStatus::Ok; }

    /// Lets call sites write `if (auto r = read_exact(...); !r) { ... }`.
    /// explicit so an IoResult never silently converts to bool/int elsewhere.
    explicit operator bool() const noexcept { return ok(); }
};

/// Reads exactly `size` bytes into `buffer`, looping until it has them all.
///
/// A single read() on a SOCK_STREAM socket is allowed to return fewer bytes
/// than requested - that is not an error, it is the normal consequence of the
/// data still being in flight, split across packets, or larger than the socket
/// buffer. Code that treats one read() as one message works perfectly on
/// localhost with small payloads and then corrupts data in production. Every
/// stream read in this codebase goes through this function.
///
/// Returns PeerClosed if read() returns 0 (orderly shutdown by the peer)
/// before `size` bytes have arrived. EINTR is retried transparently.
IoResult read_exact(int fd, void* buffer, std::size_t size) noexcept;

/// Writes exactly `size` bytes from `buffer`, looping until they are all gone.
///
/// The symmetric hazard: write() may accept fewer bytes than offered once the
/// socket send buffer fills. EINTR is retried; EPIPE is reported as
/// PeerClosed rather than Error, because a peer that hung up is a routine
/// event and not a fault of ours.
IoResult write_all(int fd, const void* buffer, std::size_t size) noexcept;

/// Reads *up to* `size` bytes, returning as soon as any data is available.
///
/// This is the raw shape of read(): one call, however many bytes happened to
/// have arrived. Framed protocol code should use read_exact instead - this
/// exists for byte-stream consumers (the step 1 echo server) that genuinely
/// have no message boundaries to respect, and to make the partial-read
/// behaviour that read_exact papers over visible and testable.
///
/// Returns PeerClosed with transferred == 0 at end of stream.
IoResult read_some(int fd, void* buffer, std::size_t size) noexcept;

}  // namespace lrd::net
