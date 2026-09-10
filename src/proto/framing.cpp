#include "lrd/proto/framing.hpp"

#include "lrd/net/io.hpp"

#include <array>
#include <cstring>

namespace lrd::proto {

namespace {

std::uint32_t load_be_u32(const std::array<std::byte, 4>& bytes) noexcept {
    std::uint32_t value = 0;
    for (const std::byte b : bytes) {
        value = (value << 8) | static_cast<std::uint32_t>(b);
    }
    return value;
}

FrameResult from_io(const net::IoResult& io) noexcept {
    FrameResult result;
    switch (io.status) {
        case net::IoStatus::Ok:
            result.status = FrameStatus::Ok;
            break;
        case net::IoStatus::PeerClosed:
            // Zero bytes in means the peer hung up cleanly between frames -
            // routine. Any bytes in means it died mid-frame, and the stream can
            // no longer be resynchronised. Same syscall outcome, two very
            // different situations, which is why IoResult carries `transferred`.
            result.status = (io.transferred == 0) ? FrameStatus::PeerClosed : FrameStatus::Truncated;
            break;
        case net::IoStatus::Error:
            result.status = FrameStatus::IoError;
            result.io_error = io.error;
            break;
    }
    return result;
}

}  // namespace

const char* to_string(FrameStatus status) noexcept {
    switch (status) {
        case FrameStatus::Ok: return "Ok";
        case FrameStatus::PeerClosed: return "PeerClosed";
        case FrameStatus::Truncated: return "Truncated";
        case FrameStatus::Oversized: return "Oversized";
        case FrameStatus::IoError: return "IoError";
    }
    return "Unknown";
}

FrameResult read_frame(net::UnixStream& stream, ByteBuffer& body) {
    std::array<std::byte, kLengthPrefixSize> prefix{};

    const net::IoResult header_io = stream.read_exact(prefix.data(), prefix.size());
    if (!header_io) {
        return from_io(header_io);
    }

    const std::uint32_t length = load_be_u32(prefix);

    // Validate before allocating. This is the whole point of splitting the read
    // in two: `length` is a number chosen by a peer we do not trust, and the
    // next statement would otherwise hand it straight to resize().
    //
    // A frame shorter than the header cannot be valid either - catching it here
    // means the codec never has to defend against a body too small to hold the
    // fields it is about to read.
    if (length < kHeaderSize || length > kMaxFrameSize) {
        FrameResult result;
        result.status = FrameStatus::Oversized;
        result.length = length;
        return result;
    }

    body.resize(length);
    const net::IoResult body_io = stream.read_exact(body.data(), body.size());
    if (!body_io) {
        FrameResult result = from_io(body_io);
        // The length prefix arrived, so the peer promised a body. Failing to
        // deliver it is a truncation even if read() reported a clean EOF with
        // zero body bytes read.
        if (result.status == FrameStatus::PeerClosed) {
            result.status = FrameStatus::Truncated;
        }
        return result;
    }

    FrameResult result;
    result.length = length;
    return result;
}

namespace detail {

FrameResult write_prefixed(net::UnixStream& stream, ByteBuffer& framed) {
    const std::size_t body_size = framed.size() - kLengthPrefixSize;

    if (body_size < kHeaderSize || body_size > kMaxFrameSize) {
        FrameResult result;
        result.status = FrameStatus::Oversized;
        result.length = static_cast<std::uint32_t>(body_size);
        return result;
    }

    const auto length = static_cast<std::uint32_t>(body_size);
    for (std::size_t i = 0; i < kLengthPrefixSize; ++i) {
        const unsigned shift = static_cast<unsigned>((kLengthPrefixSize - 1 - i) * 8);
        framed[i] = static_cast<std::byte>((length >> shift) & 0xFF);
    }

    FrameResult result = from_io(stream.write_all(framed.data(), framed.size()));
    result.length = length;
    return result;
}

}  // namespace detail

FrameResult write_frame(net::UnixStream& stream, ByteView body) {
    if (body.size() < kHeaderSize || body.size() > kMaxFrameSize) {
        FrameResult result;
        result.status = FrameStatus::Oversized;
        result.length = static_cast<std::uint32_t>(body.size());
        return result;
    }

    // One buffer, one write. See the header comment for why the prefix is not
    // sent as a separate syscall.
    ByteBuffer framed;
    framed.reserve(kLengthPrefixSize + body.size());

    const auto length = static_cast<std::uint32_t>(body.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        framed.push_back(static_cast<std::byte>((length >> shift) & 0xFF));
    }
    framed.insert(framed.end(), body.begin(), body.end());

    FrameResult result = from_io(stream.write_all(framed.data(), framed.size()));
    result.length = length;
    return result;
}

}  // namespace lrd::proto
