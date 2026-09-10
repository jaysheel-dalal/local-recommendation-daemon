#pragma once

#include "lrd/net/unix_socket.hpp"
#include "lrd/proto/message.hpp"
#include "lrd/proto/wire.hpp"

#include <cstdint>

namespace lrd::proto {

/// Outcome of a frame-level operation.
///
/// Deliberately separate from IoStatus and from DecodeError, because the three
/// layers fail for different reasons and the daemon reacts to each differently:
///
///   IoError      - the socket broke. Nothing to say; drop the connection.
///   PeerClosed   - a tidy disconnect between frames. Normal.
///   Truncated    - the peer vanished *mid-frame*. The stream is unusable.
///   Oversized    - the peer asked us to allocate more than kMaxFrameSize.
///
/// Note what is absent: there is no "malformed message" here. Framing's only
/// job is to find message boundaries; whether the bytes inside make sense is
/// the codec's problem.
enum class FrameStatus : std::uint8_t {
    Ok,
    PeerClosed,
    Truncated,
    Oversized,
    IoError,
};

[[nodiscard]] const char* to_string(FrameStatus status) noexcept;

struct FrameResult {
    FrameStatus status = FrameStatus::Ok;
    int io_error = 0;          ///< errno, when status == IoError.
    std::uint32_t length = 0;  ///< Frame body length, when known.

    [[nodiscard]] bool ok() const noexcept { return status == FrameStatus::Ok; }
    explicit operator bool() const noexcept { return ok(); }
};

/// Reads one complete frame body into `body`, replacing its contents.
///
/// "Body" is the header plus payload - everything the length prefix counts.
/// The 4-byte prefix itself is consumed and not stored.
///
/// The read happens in two steps for a reason worth stating plainly: the length
/// prefix is read first and validated *before* `body` is resized. A length
/// prefix is an allocation instruction from a peer, and four bytes of 0xFF
/// would otherwise ask for 4 GiB before a single payload byte has arrived.
///
/// `body`'s capacity is reused across calls, so a connection serving many
/// requests settles into zero allocations per request after the first few.
[[nodiscard]] FrameResult read_frame(net::UnixStream& stream, ByteBuffer& body);

/// Writes a length prefix followed by `body`.
///
/// Prefix and body go out in a single write_all over one contiguous buffer.
/// Two separate writes would work, but they would double the syscalls and -
/// worse - risk Nagle-style interactions where a tiny 4-byte prefix packet is
/// delayed waiting for a peer acknowledgement that the body is waiting to
/// trigger. UNIX sockets do not suffer that particular pathology, but the
/// habit of not splitting a message across writes is the right one to keep.
[[nodiscard]] FrameResult write_frame(net::UnixStream& stream, ByteView body);

namespace detail {

/// Writes a buffer whose first kLengthPrefixSize bytes are a placeholder, after
/// patching the real body length into them. Exposed only for write_message.
[[nodiscard]] FrameResult write_prefixed(net::UnixStream& stream, ByteBuffer& framed);

}  // namespace detail

/// Encodes `message` and writes it as one frame, with no copy of the body.
///
/// The trick is reserving the length prefix *before* encoding: the codec
/// appends the body directly after the placeholder, and the real length is
/// patched in once it is known. write_frame, by contrast, has to build a second
/// buffer and copy the body into it, because by then the body already exists
/// somewhere else.
///
/// That copy is not free. Measured on this machine, going through write_frame
/// costs an allocation plus a full body copy per message; reserving the prefix
/// removes both. `scratch` is reused across calls, so after the first few
/// messages a connection does no allocation at all on the write path.
template <typename Message, typename CodecT>
[[nodiscard]] FrameResult write_message(net::UnixStream& stream, const CodecT& codec,
                                        const Message& message, ByteBuffer& scratch) {
    scratch.clear();
    scratch.resize(kLengthPrefixSize);  // placeholder, patched by write_prefixed
    codec.encode(message, scratch);     // appends the body in place
    return detail::write_prefixed(stream, scratch);
}

}  // namespace lrd::proto
