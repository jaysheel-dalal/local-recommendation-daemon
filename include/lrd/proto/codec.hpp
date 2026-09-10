#pragma once

#include "lrd/proto/message.hpp"
#include "lrd/proto/wire.hpp"

namespace lrd::proto {

/// Turns messages into frame bodies and back.
///
/// "Frame body" means the header plus payload - everything after the 4-byte
/// length prefix. The length prefix belongs to the framing layer (see
/// lrd/proto/framing.hpp), which is why nothing here knows about it. That split
/// is what lets the same codec work over a socket, a file, or a unit test's
/// std::vector.
///
/// This is an abstract base class purely to establish the seam promised in the
/// build plan: Phase 2 adds a ProtobufCodec implementing the same interface,
/// and the benchmark runs both over an otherwise identical daemon. Without the
/// seam, that comparison would mean editing the server.
///
/// The cost of the seam is one virtual call per message. That is a handful of
/// nanoseconds against a syscall costing hundreds, so it is invisible here -
/// but it is worth being able to say *why* it is invisible rather than
/// assuming it. If profiling ever disagreed, the fix would be a compile-time
/// policy parameter instead of runtime dispatch, at the cost of not being able
/// to switch codecs at runtime.
class Codec {
public:
    Codec() = default;
    virtual ~Codec() = default;

    // A polymorphic base that is copyable by default is a slicing hazard, so
    // the copy and move operations are deleted rather than left implicit.
    Codec(const Codec&) = delete;
    Codec& operator=(const Codec&) = delete;

    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// Appends the encoded body to `out`. Appends rather than replaces so a
    /// caller can reserve the length prefix first and fill it in afterwards,
    /// which saves a copy on the write path.
    virtual void encode(const Request& request, ByteBuffer& out) const = 0;
    virtual void encode(const Response& response, ByteBuffer& out) const = 0;

    /// Parses a complete frame body. Returns DecodeError::None on success.
    [[nodiscard]] virtual DecodeError decode(ByteView body, Request& out) const = 0;
    [[nodiscard]] virtual DecodeError decode(ByteView body, Response& out) const = 0;
};

/// The hand-written binary format described in docs/protocol.md.
class BinaryCodec final : public Codec {
public:
    [[nodiscard]] const char* name() const noexcept override { return "binary/v1"; }

    void encode(const Request& request, ByteBuffer& out) const override;
    void encode(const Response& response, ByteBuffer& out) const override;

    [[nodiscard]] DecodeError decode(ByteView body, Request& out) const override;
    [[nodiscard]] DecodeError decode(ByteView body, Response& out) const override;
};

/// Validates the fixed header and leaves the reader positioned at the payload.
/// Exposed for tests, which want to assert on header rejection independently
/// of any particular payload.
[[nodiscard]] DecodeError decode_header(ByteReader& reader, Header& out) noexcept;

void encode_header(const Header& header, ByteWriter& writer);

}  // namespace lrd::proto
