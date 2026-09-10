#pragma once

// Behaviour every Codec implementation must have, independent of its wire
// format. Run against BinaryCodec today and ProtobufCodec once it exists; a
// codec that passes this is substitutable for the other in the daemon.
//
// ## What is deliberately NOT here
//
// Error *detection* is format-specific, and pretending otherwise would force
// one of the two codecs to fake behaviour it does not naturally have:
//
//   * **Bad magic, version, reserved flags.** binary/v1 puts these in a fixed
//     header and rejects them. A protobuf envelope has no such header.
//   * **Trailing bytes.** binary/v1 treats extra bytes as a fatal disagreement
//     about message shape. Protobuf is required to *preserve* unknown fields -
//     that is precisely the mechanism that makes schema evolution work.
//   * **Truncation.** binary/v1 rejects a body cut at any offset. Protobuf
//     generally cannot: a message of optional fields truncated on a field
//     boundary parses cleanly as a valid shorter message.
//
// That last point is worth stating plainly, because it justifies a design
// decision made back in step 2: **the length prefix is not redundant with
// protobuf.** Protobuf cannot reliably tell you a message was cut short; the
// framing layer's length prefix is what makes truncation detectable at all.
//
// So this header covers round-trip semantics - what a message means - and each
// codec's own test file covers how its format detects corruption.

#include "lrd/proto/codec.hpp"
#include "lrd/proto/message.hpp"

#include "test_harness.hpp"

#include <cstdint>
#include <string>

namespace lrd::proto::conformance {

/// Encodes then decodes a request, returning the result by out-parameter so a
/// caller can assert on both the status and the round-tripped value.
inline DecodeError round_trip(const Codec& codec, const Request& original, Request& decoded) {
    ByteBuffer buffer;
    codec.encode(original, buffer);
    return codec.decode(buffer, decoded);
}

inline DecodeError round_trip(const Codec& codec, const Response& original, Response& decoded) {
    ByteBuffer buffer;
    codec.encode(original, buffer);
    return codec.decode(buffer, decoded);
}

inline void check_get_request(const Codec& codec) {
    Request original;
    original.type = MessageType::GetRequest;
    original.request_id = 42;
    original.key = "some/key";

    Request decoded;
    LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::None);
    LRD_CHECK(decoded.type == MessageType::GetRequest);
    LRD_CHECK_EQ(decoded.request_id, std::uint64_t{42});
    LRD_CHECK_EQ(decoded.key, original.key);
}

inline void check_put_request(const Codec& codec) {
    Request original;
    original.type = MessageType::PutRequest;
    // The full 64-bit range: a codec that narrows the id anywhere fails here.
    original.request_id = 0xFFFFFFFFFFFFFFFFULL;
    original.key = "key";
    // Embedded NULs: the reason both formats are length-prefixed rather than
    // NUL-terminated.
    original.value = std::string("binary\0value", 12);

    Request decoded;
    LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::None);
    LRD_CHECK_EQ(decoded.request_id, original.request_id);
    LRD_CHECK_EQ(decoded.key, original.key);
    LRD_CHECK_EQ(decoded.value, original.value);
}

inline void check_delete_request(const Codec& codec) {
    Request original;
    original.type = MessageType::DeleteRequest;
    original.request_id = 7;
    original.key = "doomed";

    Request decoded;
    LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::None);
    LRD_CHECK(decoded.type == MessageType::DeleteRequest);
    LRD_CHECK_EQ(decoded.key, original.key);
}

inline void check_stats_request(const Codec& codec) {
    Request original;
    original.type = MessageType::StatsRequest;
    original.request_id = 9;

    Request decoded;
    LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::None);
    LRD_CHECK(decoded.type == MessageType::StatsRequest);
    LRD_CHECK_EQ(decoded.request_id, std::uint64_t{9});
}

inline void check_get_response_hit(const Codec& codec) {
    Response original;
    original.type = MessageType::GetResponse;
    original.request_id = 1;
    original.status = StatusCode::Ok;
    original.value = "cached";

    Response decoded;
    LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::None);
    LRD_CHECK(decoded.status == StatusCode::Ok);
    LRD_CHECK_EQ(decoded.value, std::string("cached"));
}

inline void check_get_response_miss(const Codec& codec) {
    // A miss and an empty value are different answers, and both must survive.
    Response original;
    original.type = MessageType::GetResponse;
    original.status = StatusCode::NotFound;

    Response decoded;
    LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::None);
    LRD_CHECK(decoded.status == StatusCode::NotFound);
    LRD_CHECK(decoded.value.empty());

    Response empty_value;
    empty_value.type = MessageType::GetResponse;
    empty_value.status = StatusCode::Ok;
    empty_value.value = "";

    Response decoded_empty;
    LRD_CHECK(round_trip(codec, empty_value, decoded_empty) == DecodeError::None);
    LRD_CHECK(decoded_empty.status == StatusCode::Ok);
    LRD_CHECK(decoded_empty.value.empty());
}

inline void check_simple_responses(const Codec& codec) {
    for (const MessageType type : {MessageType::PutResponse, MessageType::DeleteResponse}) {
        for (const StatusCode status : {StatusCode::Ok, StatusCode::NotFound}) {
            Response original;
            original.type = type;
            original.request_id = 5;
            original.status = status;

            Response decoded;
            LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::None);
            LRD_CHECK(decoded.type == type);
            LRD_CHECK(decoded.status == status);
        }
    }
}

inline void check_stats_response(const Codec& codec) {
    Response original;
    original.type = MessageType::StatsResponse;
    original.status = StatusCode::Ok;
    original.stats = Stats{100, 60, 30, 10, 45, 15, 5, 20, 64};

    Response decoded;
    LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::None);
    LRD_CHECK_EQ(decoded.stats.requests, std::uint64_t{100});
    LRD_CHECK_EQ(decoded.stats.gets, std::uint64_t{60});
    LRD_CHECK_EQ(decoded.stats.puts, std::uint64_t{30});
    LRD_CHECK_EQ(decoded.stats.deletes, std::uint64_t{10});
    LRD_CHECK_EQ(decoded.stats.hits, std::uint64_t{45});
    LRD_CHECK_EQ(decoded.stats.misses, std::uint64_t{15});
    LRD_CHECK_EQ(decoded.stats.evictions, std::uint64_t{5});
    LRD_CHECK_EQ(decoded.stats.entries, std::uint64_t{20});
    LRD_CHECK_EQ(decoded.stats.capacity, std::uint64_t{64});
}

inline void check_error_response(const Codec& codec) {
    const Response original = make_error(9, StatusCode::InvalidRequest, "key too long");

    Response decoded;
    LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::None);
    LRD_CHECK(decoded.type == MessageType::ErrorResponse);
    LRD_CHECK(decoded.status == StatusCode::InvalidRequest);
    LRD_CHECK_EQ(decoded.value, std::string("key too long"));
}

inline void check_direction_is_enforced(const Codec& codec) {
    // A daemon must never process a GetResponse and a client must never process
    // a PutRequest. Both codecs have to catch a crossed wire, however they
    // encode the distinction.
    Response response;
    response.type = MessageType::GetResponse;
    response.request_id = 1;
    ByteBuffer response_bytes;
    codec.encode(response, response_bytes);

    Request as_request;
    LRD_CHECK(codec.decode(response_bytes, as_request) == DecodeError::WrongDirection);

    Request request;
    request.type = MessageType::GetRequest;
    request.key = "k";
    ByteBuffer request_bytes;
    codec.encode(request, request_bytes);

    Response as_response;
    LRD_CHECK(codec.decode(request_bytes, as_response) == DecodeError::WrongDirection);
}

inline void check_field_limits(const Codec& codec) {
    Request at_limit;
    at_limit.type = MessageType::GetRequest;
    at_limit.key = std::string(kMaxKeyLength, 'k');
    Request decoded_at_limit;
    LRD_CHECK(round_trip(codec, at_limit, decoded_at_limit) == DecodeError::None);
    LRD_CHECK_EQ(decoded_at_limit.key.size(), std::size_t{kMaxKeyLength});

    Request over_limit;
    over_limit.type = MessageType::GetRequest;
    over_limit.request_id = 3;
    over_limit.key = std::string(kMaxKeyLength + 1, 'k');
    Request decoded_over;
    LRD_CHECK(round_trip(codec, over_limit, decoded_over) == DecodeError::FieldTooLarge);
    // The id must survive, so the daemon can address its rejection to the right
    // request instead of dropping the connection.
    LRD_CHECK_EQ(decoded_over.request_id, std::uint64_t{3});
}

inline void check_reused_object_is_cleared(const Codec& codec) {
    // Request objects are reused for a connection's lifetime. A decoder that
    // only assigns the fields its own type uses would leave a previous
    // message's value attached to a later Get.
    Request put;
    put.type = MessageType::PutRequest;
    put.key = "k";
    put.value = "stale-value";
    ByteBuffer put_bytes;
    codec.encode(put, put_bytes);

    Request get;
    get.type = MessageType::GetRequest;
    get.key = "k";
    ByteBuffer get_bytes;
    codec.encode(get, get_bytes);

    Request reused;
    LRD_REQUIRE(codec.decode(put_bytes, reused) == DecodeError::None);
    LRD_REQUIRE(reused.value == "stale-value");

    LRD_REQUIRE(codec.decode(get_bytes, reused) == DecodeError::None);
    LRD_CHECK(reused.value.empty());
}

inline void check_encode_appends(const Codec& codec) {
    // write_message reserves the length prefix before encoding and patches it
    // afterwards, which only works because encode appends rather than replaces.
    // A codec that cleared the buffer would silently break the zero-copy write
    // path added in step 2.
    ByteBuffer buffer;
    buffer.push_back(std::byte{0xAA});
    buffer.push_back(std::byte{0xBB});

    Request request;
    request.type = MessageType::GetRequest;
    request.key = "k";
    codec.encode(request, buffer);

    LRD_REQUIRE(buffer.size() > 2);
    LRD_CHECK(buffer[0] == std::byte{0xAA});
    LRD_CHECK(buffer[1] == std::byte{0xBB});
}

/// Runs every conformance check. Each codec's test file calls this once.
inline void run_all(const Codec& codec) {
    check_get_request(codec);
    check_put_request(codec);
    check_delete_request(codec);
    check_stats_request(codec);
    check_get_response_hit(codec);
    check_get_response_miss(codec);
    check_simple_responses(codec);
    check_stats_response(codec);
    check_error_response(codec);
    check_direction_is_enforced(codec);
    check_field_limits(codec);
    check_reused_object_is_cleared(codec);
    check_encode_appends(codec);
}

}  // namespace lrd::proto::conformance
