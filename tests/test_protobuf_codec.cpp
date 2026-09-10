#include "test_harness.hpp"

#ifndef LRD_WITH_PROTOBUF

// Built without protobuf. One passing case rather than an empty binary, so the
// suite reports "protobuf not built" instead of silently having no coverage.
LRD_TEST("protobuf codec not built in this configuration") {
    LRD_CHECK(true);
}

#else

#include "lrd/proto/codec.hpp"
#include "lrd/proto/protobuf_codec.hpp"

#include "codec_conformance.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace lrd::proto;
namespace conformance = lrd::proto::conformance;

// The point of the shared suite: ProtobufCodec is held to exactly the same
// round-trip semantics as BinaryCodec, from the same source file, so
// "substitutable" is a tested property rather than an intention.
LRD_TEST("protobuf codec satisfies the shared codec conformance suite") {
    const ProtobufCodec codec;
    conformance::run_all(codec);
}

LRD_TEST("the two codecs agree on every message's meaning") {
    // Stronger than both passing the suite separately: encode with one, decode
    // with the other's *semantics*, and check the decoded structs match. A
    // field that both codecs dropped identically would slip past the suite;
    // this catches a field one carries and the other silently loses.
    const BinaryCodec binary;
    const ProtobufCodec protobuf;

    Request original;
    original.type = MessageType::PutRequest;
    original.request_id = 0xFEEDFACECAFEBEEFULL;
    original.key = std::string("key\0with\0nuls", 13);
    original.value = std::string(500, 'v');

    ByteBuffer binary_bytes;
    ByteBuffer protobuf_bytes;
    binary.encode(original, binary_bytes);
    protobuf.encode(original, protobuf_bytes);

    Request from_binary;
    Request from_protobuf;
    LRD_REQUIRE(binary.decode(binary_bytes, from_binary) == DecodeError::None);
    LRD_REQUIRE(protobuf.decode(protobuf_bytes, from_protobuf) == DecodeError::None);

    LRD_CHECK(from_binary.type == from_protobuf.type);
    LRD_CHECK_EQ(from_binary.request_id, from_protobuf.request_id);
    LRD_CHECK_EQ(from_binary.key, from_protobuf.key);
    LRD_CHECK_EQ(from_binary.value, from_protobuf.value);
}

LRD_TEST("non-UTF8 keys survive, because the schema uses bytes not string") {
    // A protobuf `string` field must be valid UTF-8 and implementations may
    // reject anything else. Our keys are arbitrary bytes. Declaring them
    // `string` would compile and pass casual testing, then corrupt real
    // payloads - so this pins the schema decision.
    const ProtobufCodec codec;

    Request original;
    original.type = MessageType::PutRequest;
    // Bytes given as unsigned hex constants rather than character escapes.
    // Two reasons, both real:
    //   * A hex escape in C++ is unbounded, so "\x80binary" reads "80b" as one
    //     escape - out of range, and not what anyone meant.
    //   * char is signed here, so a byte above 0x7F in a char initialiser is a
    //     narrowing conversion. -Wnarrowing catches it; unsigned char does not
    //     have the problem at all.
    static const unsigned char kKeyBytes[] = {0xFF, 0xFE, 0x00, 0x80,
                                              'b', 'i', 'n', 'a', 'r', 'y'};
    static const unsigned char kValueBytes[] = {0xC0, 0xC1, 0xF5, 0xFF};

    original.key = std::string(reinterpret_cast<const char*>(kKeyBytes), sizeof(kKeyBytes));
    original.value =
        std::string(reinterpret_cast<const char*>(kValueBytes), sizeof(kValueBytes));

    ByteBuffer buffer;
    codec.encode(original, buffer);

    Request decoded;
    LRD_REQUIRE(codec.decode(buffer, decoded) == DecodeError::None);
    LRD_CHECK_EQ(decoded.key, original.key);
    LRD_CHECK_EQ(decoded.value, original.value);
}

LRD_TEST("an empty body decodes as UnknownType rather than crashing") {
    // A zero-length envelope is a valid protobuf message with no oneof arm set.
    // It parses successfully and means nothing, which has to be distinguished
    // from a parse failure.
    const ProtobufCodec codec;
    const ByteBuffer empty;

    Request decoded;
    LRD_CHECK(codec.decode(empty, decoded) == DecodeError::UnknownType);
}

LRD_TEST("garbage is rejected rather than parsed into nonsense") {
    const ProtobufCodec codec;
    ByteBuffer garbage;
    for (int i = 0; i < 64; ++i) {
        garbage.push_back(static_cast<std::byte>(0xFF));
    }

    Request decoded;
    LRD_CHECK(codec.decode(garbage, decoded) != DecodeError::None);
}

LRD_TEST("a binary/v1 body is not silently accepted as protobuf") {
    // The cost of dropping the magic number: a mismatched pair (binary client,
    // protobuf daemon) has no header check to catch it. In practice a binary/v1
    // body begins with 0x4C, which protobuf reads as field 9 with wire type 4 -
    // an invalid tag - so it is still caught. Pinning that here so the claim in
    // protobuf_codec.hpp is tested rather than asserted.
    const BinaryCodec binary;
    const ProtobufCodec protobuf;

    Request original;
    original.type = MessageType::GetRequest;
    original.request_id = 7;
    original.key = "some/key";

    ByteBuffer binary_bytes;
    binary.encode(original, binary_bytes);

    Request decoded;
    LRD_CHECK(protobuf.decode(binary_bytes, decoded) != DecodeError::None);
}

LRD_TEST("a protobuf body is rejected by the binary codec on magic") {
    // The mirror image, and the stronger direction: binary/v1's magic check
    // catches this outright.
    const BinaryCodec binary;
    const ProtobufCodec protobuf;

    Request original;
    original.type = MessageType::GetRequest;
    original.request_id = 7;
    original.key = "some/key";

    ByteBuffer protobuf_bytes;
    protobuf.encode(original, protobuf_bytes);

    Request decoded;
    LRD_CHECK(binary.decode(protobuf_bytes, decoded) == DecodeError::BadMagic);
}

LRD_TEST("unknown fields are preserved, not rejected") {
    // The schema-evolution property, tested directly rather than demonstrated
    // by hand. An envelope carrying a field this build has never heard of must
    // still decode - that is what lets an old daemon serve a new client.
    //
    // Field 999 with wire type 2 (length-delimited), holding four bytes:
    // tag = (999 << 3) | 2 = 7994, varint-encoded.
    const ProtobufCodec codec;

    Request original;
    original.type = MessageType::GetRequest;
    original.request_id = 11;
    original.key = "key";

    ByteBuffer buffer;
    codec.encode(original, buffer);

    // Append the unknown field.
    buffer.push_back(std::byte{0xBA});  // varint tag, byte 1
    buffer.push_back(std::byte{0x3E});  // varint tag, byte 2
    buffer.push_back(std::byte{0x04});  // length 4
    for (int i = 0; i < 4; ++i) {
        buffer.push_back(std::byte{0x61});
    }

    Request decoded;
    // binary/v1 would return TrailingBytes here and drop the connection.
    LRD_CHECK(codec.decode(buffer, decoded) == DecodeError::None);
    LRD_CHECK_EQ(decoded.request_id, std::uint64_t{11});
    LRD_CHECK_EQ(decoded.key, std::string("key"));
}

LRD_TEST("the codec is usable from several threads at once") {
    // The codec is shared by every worker and its methods are const. The
    // reusable envelope it keeps is thread_local precisely so that sharing is
    // safe; this is the case TSan is pointed at to prove it.
    const ProtobufCodec codec;
    std::atomic<int> mismatches{0};

    {
        std::vector<std::jthread> threads;
        threads.reserve(4);
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 2000; ++i) {
                    Request original;
                    original.type = MessageType::PutRequest;
                    original.request_id = static_cast<std::uint64_t>(t * 100000 + i);
                    original.key = "key-" + std::to_string(t) + "-" + std::to_string(i);
                    original.value = std::string(64, static_cast<char>('a' + t));

                    ByteBuffer buffer;
                    codec.encode(original, buffer);

                    Request decoded;
                    if (codec.decode(buffer, decoded) != DecodeError::None ||
                        decoded.key != original.key || decoded.value != original.value ||
                        decoded.request_id != original.request_id) {
                        mismatches.fetch_add(1);
                    }
                }
            });
        }
    }

    LRD_CHECK_EQ(mismatches.load(), 0);
}

#endif  // LRD_WITH_PROTOBUF
