#include "lrd/proto/codec.hpp"

#include "test_harness.hpp"

#include <string>

using namespace lrd::proto;

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

LRD_TEST("get request round-trips") {
    const BinaryCodec codec;
    Request original;
    original.type = MessageType::GetRequest;
    original.request_id = 42;
    original.key = "some/key";

    ByteBuffer buffer;
    codec.encode(original, buffer);
    // Header is 16 bytes; payload is a 4-byte length prefix plus the key.
    LRD_CHECK_EQ(buffer.size(), kHeaderSize + 4 + original.key.size());

    Request decoded;
    LRD_REQUIRE(codec.decode(buffer, decoded) == DecodeError::None);
    LRD_CHECK(decoded.type == MessageType::GetRequest);
    LRD_CHECK_EQ(decoded.request_id, std::uint64_t{42});
    LRD_CHECK_EQ(decoded.key, original.key);
}

LRD_TEST("put request round-trips key and value") {
    const BinaryCodec codec;
    Request original;
    original.type = MessageType::PutRequest;
    original.request_id = 0xFFFFFFFFFFFFFFFFULL;  // exercises the full 64-bit id
    original.key = "key";
    original.value = std::string("binary\0value", 12);

    ByteBuffer buffer;
    codec.encode(original, buffer);

    Request decoded;
    LRD_REQUIRE(codec.decode(buffer, decoded) == DecodeError::None);
    LRD_CHECK_EQ(decoded.request_id, original.request_id);
    LRD_CHECK_EQ(decoded.key, original.key);
    LRD_CHECK_EQ(decoded.value, original.value);
}

LRD_TEST("stats request has an empty payload") {
    const BinaryCodec codec;
    Request original;
    original.type = MessageType::StatsRequest;
    original.request_id = 7;

    ByteBuffer buffer;
    codec.encode(original, buffer);
    LRD_CHECK_EQ(buffer.size(), kHeaderSize);

    Request decoded;
    LRD_CHECK(codec.decode(buffer, decoded) == DecodeError::None);
    LRD_CHECK(decoded.type == MessageType::StatsRequest);
}

LRD_TEST("every response type round-trips") {
    const BinaryCodec codec;

    {
        Response original;
        original.type = MessageType::GetResponse;
        original.request_id = 1;
        original.status = StatusCode::Ok;
        original.value = "cached";

        ByteBuffer buffer;
        codec.encode(original, buffer);
        Response decoded;
        LRD_REQUIRE(codec.decode(buffer, decoded) == DecodeError::None);
        LRD_CHECK(decoded.status == StatusCode::Ok);
        LRD_CHECK_EQ(decoded.value, std::string("cached"));
    }
    {
        Response original;
        original.type = MessageType::GetResponse;
        original.status = StatusCode::NotFound;

        ByteBuffer buffer;
        codec.encode(original, buffer);
        Response decoded;
        LRD_REQUIRE(codec.decode(buffer, decoded) == DecodeError::None);
        LRD_CHECK(decoded.status == StatusCode::NotFound);
        LRD_CHECK(decoded.value.empty());
    }
    {
        Response original;
        original.type = MessageType::StatsResponse;
        original.status = StatusCode::Ok;
        original.stats = Stats{100, 60, 30, 10, 45, 15};

        ByteBuffer buffer;
        codec.encode(original, buffer);
        Response decoded;
        LRD_REQUIRE(codec.decode(buffer, decoded) == DecodeError::None);
        LRD_CHECK_EQ(decoded.stats.requests, std::uint64_t{100});
        LRD_CHECK_EQ(decoded.stats.gets, std::uint64_t{60});
        LRD_CHECK_EQ(decoded.stats.puts, std::uint64_t{30});
        LRD_CHECK_EQ(decoded.stats.deletes, std::uint64_t{10});
        LRD_CHECK_EQ(decoded.stats.hits, std::uint64_t{45});
        LRD_CHECK_EQ(decoded.stats.misses, std::uint64_t{15});
    }
    {
        const Response original = make_error(9, StatusCode::InvalidRequest, "key too long");
        ByteBuffer buffer;
        codec.encode(original, buffer);
        Response decoded;
        LRD_REQUIRE(codec.decode(buffer, decoded) == DecodeError::None);
        LRD_CHECK(decoded.type == MessageType::ErrorResponse);
        LRD_CHECK_EQ(decoded.value, std::string("key too long"));
    }
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
