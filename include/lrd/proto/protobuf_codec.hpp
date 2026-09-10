#pragma once

#include "lrd/proto/codec.hpp"

namespace lrd::proto {

/// The Codec implementation backed by proto/lrd.proto.
///
/// ## The public header deliberately mentions no protobuf types
///
/// Nothing here includes lrd.pb.h. That matters: a consumer linking the client
/// library gets `<lrd/proto/protobuf_codec.hpp>` without needing protobuf's
/// headers on their include path, and without protobuf's generated code
/// appearing in their build. The generated types are an implementation detail
/// confined to the .cpp.
///
/// It is possible because the codec is stateless - every method is const and
/// keeps no protobuf object as a member.
///
/// ## What differs from BinaryCodec, and why it is not a defect
///
/// Both satisfy the shared conformance suite in tests/codec_conformance.hpp,
/// which covers round-trip semantics. The formats differ in what corruption
/// they can *detect*:
///
///   * **No magic, version or reserved-flags check.** binary/v1 puts those in a
///     16-byte fixed header. An envelope has none, so a desynchronised stream is
///     caught later here - usually still caught, because a binary/v1 body starts
///     with 0x4C, which decodes as an invalid protobuf wire type.
///   * **Unknown fields are preserved, not rejected.** binary/v1 treats trailing
///     bytes as a fatal disagreement about message shape; protobuf is required
///     to keep them. That is exactly the property that makes schema evolution
///     work, and the reason adding three Stats fields in step 3 was a breaking
///     change under binary/v1 and would not be here.
///   * **Truncation is not reliably detectable.** A message of optional fields
///     cut on a field boundary parses cleanly as a valid shorter message. This
///     is why the length prefix in the framing layer is load-bearing rather than
///     redundant - it, not the codec, is what catches a short read.
///
/// Choosing between them is therefore not "which is faster" but "which failure
/// mode do you want": binary/v1 fails loudly on anything unexpected, protobuf
/// tolerates the unexpected so that versions can drift apart safely.
class ProtobufCodec final : public Codec {
public:
    [[nodiscard]] const char* name() const noexcept override { return "protobuf/v1"; }

    void encode(const Request& request, ByteBuffer& out) const override;
    void encode(const Response& response, ByteBuffer& out) const override;

    [[nodiscard]] DecodeError decode(ByteView body, Request& out) const override;
    [[nodiscard]] DecodeError decode(ByteView body, Response& out) const override;
};

}  // namespace lrd::proto
