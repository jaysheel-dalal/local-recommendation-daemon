#include "lrd/proto/codec.hpp"

#include "codec_conformance.hpp"
#include "test_harness.hpp"

#include <string>

using namespace lrd::proto;
namespace conformance = lrd::proto::conformance;

namespace {

/// Builds a syntactically valid frame body with overridable header fields, so
/// each header rejection can be tested in isolation.
ByteBuffer make_body(std::uint32_t magic = kMagic, std::uint8_t version = kVersion,
                     std::uint8_t type = static_cast<std::uint8_t>(MessageType::GetRequest),
                     std::uint16_t flags = 0, std::uint64_t request_id = 1,
                     const std::string& key = "k") {
    ByteBuffer buffer;
    ByteWriter writer(buffer);
    writer.u32(magic);
    writer.u8(version);
    writer.u8(type);
    writer.u16(flags);
    writer.u64(request_id);
    writer.string(key);
    return buffer;
}

}  // namespace

// Round-trip semantics live in codec_conformance.hpp, so that ProtobufCodec is
// held to exactly the same behaviour rather than to a suite written around the
// binary format. What stays in this file is what is specific to binary/v1:
// how *this* format detects corruption.

LRD_TEST("binary codec satisfies the shared codec conformance suite") {
    const BinaryCodec codec;
    conformance::run_all(codec);
}

LRD_TEST("a get request is exactly the header plus a length-prefixed key") {
    // Format-specific on purpose: it pins the byte layout documented in
    // docs/protocol.md, which a round-trip test cannot.
    const BinaryCodec codec;
    Request original;
    original.type = MessageType::GetRequest;
    original.request_id = 42;
    original.key = "some/key";

    ByteBuffer buffer;
    codec.encode(original, buffer);
    LRD_CHECK_EQ(buffer.size(), kHeaderSize + 4 + original.key.size());
}

LRD_TEST("a stats request is header-only") {
    const BinaryCodec codec;
    Request original;
    original.type = MessageType::StatsRequest;
    original.request_id = 7;

    ByteBuffer buffer;
    codec.encode(original, buffer);
    LRD_CHECK_EQ(buffer.size(), kHeaderSize);
}

// --------------------------------------------------------------------------
// Header rejection. Each of these is a frame the daemon must refuse.
// --------------------------------------------------------------------------

LRD_TEST("bad magic is rejected") {
    const BinaryCodec codec;
    const ByteBuffer body = make_body(0xDEADBEEF);
    Request decoded;
    LRD_CHECK(codec.decode(body, decoded) == DecodeError::BadMagic);
}

LRD_TEST("an unsupported version is rejected rather than guessed at") {
    const BinaryCodec codec;
    const ByteBuffer body = make_body(kMagic, 99);
    Request decoded;
    LRD_CHECK(codec.decode(body, decoded) == DecodeError::UnsupportedVersion);
}

LRD_TEST("non-zero reserved flags are rejected") {
    // If v1 ignored unknown flags, a future client could not tell whether its
    // request had been honoured or silently downgraded.
    const BinaryCodec codec;
    const ByteBuffer body =
        make_body(kMagic, kVersion, static_cast<std::uint8_t>(MessageType::GetRequest), 0x0001);
    Request decoded;
    LRD_CHECK(codec.decode(body, decoded) == DecodeError::ReservedFlags);
}

LRD_TEST("an unknown message type is rejected") {
    const BinaryCodec codec;
    const ByteBuffer body = make_body(kMagic, kVersion, 0x77);
    Request decoded;
    LRD_CHECK(codec.decode(body, decoded) == DecodeError::UnknownType);
}

LRD_TEST("a response decoded as a request is rejected") {
    // A daemon should never be processing a GetResponse. Catching the direction
    // at the header means a crossed-wires bug surfaces immediately rather than
    // as a puzzling field mismatch later.
    const BinaryCodec codec;
    const ByteBuffer body =
        make_body(kMagic, kVersion, static_cast<std::uint8_t>(MessageType::GetResponse));
    Request decoded;
    LRD_CHECK(codec.decode(body, decoded) == DecodeError::WrongDirection);
}

LRD_TEST("a request decoded as a response is rejected") {
    const BinaryCodec codec;
    const ByteBuffer body = make_body();  // GetRequest
    Response decoded;
    LRD_CHECK(codec.decode(body, decoded) == DecodeError::WrongDirection);
}

LRD_TEST("a truncated body is rejected at every prefix length") {
    // Sweeping every truncation point is worth more than picking one: it proves
    // no field length is read without a bounds check, including inside the
    // header.
    const BinaryCodec codec;
    Request original;
    original.type = MessageType::PutRequest;
    original.request_id = 5;
    original.key = "key";
    original.value = "value";

    ByteBuffer full;
    codec.encode(original, full);

    for (std::size_t prefix = 0; prefix < full.size(); ++prefix) {
        const ByteView truncated(full.data(), prefix);
        Request decoded;
        const DecodeError error = codec.decode(truncated, decoded);
        LRD_CHECK(error != DecodeError::None);
    }

    // The untruncated frame must still be accepted, or the loop above proves
    // nothing.
    Request decoded;
    LRD_CHECK(codec.decode(full, decoded) == DecodeError::None);
}

LRD_TEST("trailing bytes are rejected") {
    // Extra bytes mean sender and receiver disagree about this message's shape.
    // Accepting them would leave the next frame decoded from the wrong offset.
    const BinaryCodec codec;
    Request original;
    original.type = MessageType::GetRequest;
    original.key = "k";

    ByteBuffer buffer;
    codec.encode(original, buffer);
    buffer.push_back(std::byte{0x00});

    Request decoded;
    LRD_CHECK(codec.decode(buffer, decoded) == DecodeError::TrailingBytes);
}

LRD_TEST("an over-long key is rejected on the way in") {
    // The decoder cannot assume the peer used our encoder, or our limits.
    const BinaryCodec codec;
    Request original;
    original.type = MessageType::GetRequest;
    original.request_id = 3;
    original.key = std::string(kMaxKeyLength + 1, 'k');

    ByteBuffer buffer;
    codec.encode(original, buffer);

    Request decoded;
    LRD_CHECK(codec.decode(buffer, decoded) == DecodeError::FieldTooLarge);
    // request_id survives, so the daemon can address its error response to the
    // right request rather than dropping the connection.
    LRD_CHECK_EQ(decoded.request_id, std::uint64_t{3});
}

LRD_TEST("a key exactly at the limit is accepted") {
    const BinaryCodec codec;
    Request original;
    original.type = MessageType::GetRequest;
    original.key = std::string(kMaxKeyLength, 'k');

    ByteBuffer buffer;
    codec.encode(original, buffer);

    Request decoded;
    LRD_CHECK(codec.decode(buffer, decoded) == DecodeError::None);
    LRD_CHECK_EQ(decoded.key.size(), std::size_t{kMaxKeyLength});
}

LRD_TEST("decoding into a reused object clears stale fields") {
    // Request objects are reused across a connection's lifetime. A decoder that
    // only assigned the fields its type uses would leave a previous message's
    // value attached to a later Get.
    const BinaryCodec codec;

    Request put;
    put.type = MessageType::PutRequest;
    put.key = "k";
    put.value = "stale-value";
    ByteBuffer put_buffer;
    codec.encode(put, put_buffer);

    Request get;
    get.type = MessageType::GetRequest;
    get.key = "k";
    ByteBuffer get_buffer;
    codec.encode(get, get_buffer);

    Request reused;
    LRD_REQUIRE(codec.decode(put_buffer, reused) == DecodeError::None);
    LRD_REQUIRE(reused.value == "stale-value");

    LRD_REQUIRE(codec.decode(get_buffer, reused) == DecodeError::None);
    LRD_CHECK(reused.value.empty());
}
