#include "lrd/proto/codec.hpp"

#include "codec_conformance.hpp"
#include "test_harness.hpp"

#include <string>
#include <variant>

using namespace lrd::proto;
namespace conformance = lrd::proto::conformance;

namespace {

/// Builds a syntactically valid frame body with overridable header fields, so
/// each header rejection can be tested in isolation.
ByteBuffer make_body(std::uint32_t magic = kMagic, std::uint8_t version = kVersion,
                     std::uint8_t type = static_cast<std::uint8_t>(MessageType::GetItemRequest),
                     std::uint16_t flags = 0, std::uint64_t request_id = 1) {
    ByteBuffer buffer;
    ByteWriter writer(buffer);
    writer.u32(magic);
    writer.u8(version);
    writer.u8(type);
    writer.u16(flags);
    writer.u64(request_id);
    writer.u64(1234);  // GetItemRequest payload: the item id
    return buffer;
}

}  // namespace

// Round-trip semantics live in codec_conformance.hpp, so ProtobufCodec is held to
// exactly the same behaviour rather than to a suite written around the binary
// format. What stays here is specific to binary/v2: how *this* format detects
// corruption, and its byte layout.

LRD_TEST("binary codec satisfies the shared codec conformance suite") {
    const BinaryCodec codec;
    conformance::run_all(codec);
}

LRD_TEST("a get-item request is exactly the header plus an item id") {
    // Format-specific on purpose: pins the byte layout documented in
    // docs/protocol.md, which a round-trip test cannot.
    const BinaryCodec codec;
    Request original;
    original.request_id = 42;
    original.body = GetItem{7};

    ByteBuffer buffer;
    codec.encode(original, buffer);
    LRD_CHECK_EQ(buffer.size(), kHeaderSize + 8);
}

LRD_TEST("a stats request is header-only") {
    const BinaryCodec codec;
    Request original;
    original.request_id = 7;
    original.body = GetStats{};

    ByteBuffer buffer;
    codec.encode(original, buffer);
    LRD_CHECK_EQ(buffer.size(), kHeaderSize);
}

LRD_TEST("a not-found get-item response omits the item entirely") {
    // One status byte, no zeroed item. Smaller, and unambiguous about whether an
    // item is present.
    const BinaryCodec codec;
    Response original;
    original.body = GetItemResult{StatusCode::NotFound, {}};

    ByteBuffer buffer;
    codec.encode(original, buffer);
    LRD_CHECK_EQ(buffer.size(), kHeaderSize + 1);
}

LRD_TEST("bad magic is rejected") {
    const BinaryCodec codec;
    Request decoded;
    LRD_CHECK(codec.decode(make_body(0xDEADBEEF), decoded) == DecodeError::BadMagic);
}

LRD_TEST("a v1 peer is rejected on version, not misread") {
    // The reason the version moved to 2 in step 9: v1's type bytes still exist
    // but now mean different things, so a v1 frame must fail loudly.
    const BinaryCodec codec;
    Request decoded;
    LRD_CHECK(codec.decode(make_body(kMagic, 1), decoded) == DecodeError::UnsupportedVersion);
}

LRD_TEST("non-zero reserved flags are rejected") {
    const BinaryCodec codec;
    Request decoded;
    const ByteBuffer body =
        make_body(kMagic, kVersion, static_cast<std::uint8_t>(MessageType::GetItemRequest), 0x0001);
    LRD_CHECK(codec.decode(body, decoded) == DecodeError::ReservedFlags);
}

LRD_TEST("an unknown message type is rejected") {
    const BinaryCodec codec;
    Request decoded;
    LRD_CHECK(codec.decode(make_body(kMagic, kVersion, 0x77), decoded) == DecodeError::UnknownType);
}

LRD_TEST("a truncated body is rejected at every prefix length") {
    // Sweeping every truncation point proves no field length is read without a
    // bounds check, including inside the header.
    const BinaryCodec codec;
    Request original;
    original.request_id = 5;
    original.body = PutItem{conformance::sample_item()};

    ByteBuffer full;
    codec.encode(original, full);

    for (std::size_t prefix = 0; prefix < full.size(); ++prefix) {
        Request decoded;
        LRD_CHECK(codec.decode(ByteView(full.data(), prefix), decoded) != DecodeError::None);
    }

    Request decoded;
    LRD_CHECK(codec.decode(full, decoded) == DecodeError::None);
}

LRD_TEST("trailing bytes are rejected") {
    // Extra bytes mean sender and receiver disagree about this message's shape.
    // Accepting them would decode the next frame from the wrong offset. Protobuf
    // deliberately does the opposite - see test_protobuf_codec.
    const BinaryCodec codec;
    Request original;
    original.body = GetItem{1};

    ByteBuffer buffer;
    codec.encode(original, buffer);
    buffer.push_back(std::byte{0x00});

    Request decoded;
    LRD_CHECK(codec.decode(buffer, decoded) == DecodeError::TrailingBytes);
}

LRD_TEST("a hostile affinity count is rejected before it is reserved") {
    // Hand-built: a Recommend frame claiming four billion affinities. The count
    // must be bounded before the read loop, or a legal 1 MiB frame buys a very
    // long loop of failed reads.
    const BinaryCodec codec;
    ByteBuffer buffer;
    ByteWriter writer(buffer);
    writer.u32(kMagic);
    writer.u8(kVersion);
    writer.u8(static_cast<std::uint8_t>(MessageType::RecommendRequest));
    writer.u16(0);
    writer.u64(1);
    writer.u32(0xFFFFFFFFu);  // claimed affinity count

    Request decoded;
    LRD_CHECK(codec.decode(buffer, decoded) == DecodeError::FieldTooLarge);
}
