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
#include <variant>
#include <vector>

using namespace lrd::proto;
namespace conformance = lrd::proto::conformance;

LRD_TEST("protobuf codec satisfies the shared codec conformance suite") {
    const ProtobufCodec codec;
    conformance::run_all(codec);
}

LRD_TEST("the two codecs agree on every field of a message") {
    // Stronger than both passing the suite separately: a field that *both* codecs
    // dropped identically would slip past the suite, but not past this.
    const BinaryCodec binary;
    const ProtobufCodec protobuf;

    Request original;
    original.request_id = 0xFEEDFACECAFEBEEFULL;
    original.body = PutItem{conformance::sample_item()};

    ByteBuffer binary_bytes;
    ByteBuffer protobuf_bytes;
    binary.encode(original, binary_bytes);
    protobuf.encode(original, protobuf_bytes);

    Request from_binary;
    Request from_protobuf;
    LRD_REQUIRE(binary.decode(binary_bytes, from_binary) == DecodeError::None);
    LRD_REQUIRE(protobuf.decode(protobuf_bytes, from_protobuf) == DecodeError::None);

    LRD_CHECK_EQ(from_binary.request_id, from_protobuf.request_id);
    const auto* a = std::get_if<PutItem>(&from_binary.body);
    const auto* b = std::get_if<PutItem>(&from_protobuf.body);
    LRD_REQUIRE(a != nullptr);
    LRD_REQUIRE(b != nullptr);
    LRD_CHECK_EQ(a->item.id, b->item.id);
    LRD_CHECK_EQ(a->item.category, b->item.category);
    LRD_CHECK_EQ(a->item.advertiser, b->item.advertiser);
    LRD_CHECK_EQ(a->item.base_score, b->item.base_score);
    LRD_CHECK_EQ(lrd::rank::to_epoch_millis(a->item.created_at),
                 lrd::rank::to_epoch_millis(b->item.created_at));
}

LRD_TEST("recommendations agree field for field across codecs") {
    const BinaryCodec binary;
    const ProtobufCodec protobuf;

    RecommendResult result;
    result.items = {lrd::rank::RankedItem{1, 0.5, "tech", "acme"},
                    lrd::rank::RankedItem{2, 0.25, "sport", "globex"}};

    Response original;
    original.request_id = 3;
    original.body = result;

    ByteBuffer binary_bytes;
    ByteBuffer protobuf_bytes;
    binary.encode(original, binary_bytes);
    protobuf.encode(original, protobuf_bytes);

    Response from_binary;
    Response from_protobuf;
    LRD_REQUIRE(binary.decode(binary_bytes, from_binary) == DecodeError::None);
    LRD_REQUIRE(protobuf.decode(protobuf_bytes, from_protobuf) == DecodeError::None);

    const auto* a = std::get_if<RecommendResult>(&from_binary.body);
    const auto* b = std::get_if<RecommendResult>(&from_protobuf.body);
    LRD_REQUIRE(a != nullptr);
    LRD_REQUIRE(b != nullptr);
    LRD_REQUIRE(a->items.size() == b->items.size());
    for (std::size_t i = 0; i < a->items.size(); ++i) {
        LRD_CHECK_EQ(a->items[i].id, b->items[i].id);
        LRD_CHECK_EQ(a->items[i].score, b->items[i].score);
        LRD_CHECK_EQ(a->items[i].category, b->items[i].category);
    }
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

LRD_TEST("a binary/v2 body is not silently accepted as protobuf") {
    // The cost of dropping the magic number: a mismatched pair has no header
    // check to catch it. In practice a binary/v2 body begins with 0x4C, which
    // protobuf reads as field 9 with wire type 4 - an invalid tag - so it is
    // still caught. Pinned here so the claim in protobuf_codec.hpp is tested.
    const BinaryCodec binary;
    const ProtobufCodec protobuf;

    Request original;
    original.request_id = 7;
    original.body = GetItem{123};

    ByteBuffer binary_bytes;
    binary.encode(original, binary_bytes);

    Request decoded;
    LRD_CHECK(protobuf.decode(binary_bytes, decoded) != DecodeError::None);
}

LRD_TEST("a protobuf body is rejected by the binary codec on magic") {
    const BinaryCodec binary;
    const ProtobufCodec protobuf;

    Request original;
    original.request_id = 7;
    original.body = GetItem{123};

    ByteBuffer protobuf_bytes;
    protobuf.encode(original, protobuf_bytes);

    Request decoded;
    LRD_CHECK(binary.decode(protobuf_bytes, decoded) == DecodeError::BadMagic);
}

LRD_TEST("unknown fields are preserved, not rejected") {
    // The schema-evolution property, tested rather than demonstrated by hand. An
    // envelope carrying a field this build has never heard of must still decode -
    // that is what lets an old daemon serve a new client, and it is why appending
    // three Stats fields in step 3 was a breaking change under binary and would
    // not be here.
    //
    // Field 999, wire type 2 (length-delimited), four bytes of payload:
    // tag = (999 << 3) | 2 = 7994, varint-encoded as 0xBA 0x3E.
    const ProtobufCodec codec;

    Request original;
    original.request_id = 11;
    original.body = GetItem{5};

    ByteBuffer buffer;
    codec.encode(original, buffer);

    buffer.push_back(std::byte{0xBA});
    buffer.push_back(std::byte{0x3E});
    buffer.push_back(std::byte{0x04});
    for (int i = 0; i < 4; ++i) {
        buffer.push_back(std::byte{0x61});
    }

    Request decoded;
    // binary/v2 would return TrailingBytes here and drop the connection.
    LRD_CHECK(codec.decode(buffer, decoded) == DecodeError::None);
    LRD_CHECK_EQ(decoded.request_id, std::uint64_t{11});
    const auto* get = std::get_if<GetItem>(&decoded.body);
    LRD_REQUIRE(get != nullptr);
    LRD_CHECK_EQ(get->item_id, lrd::rank::ItemId{5});
}

LRD_TEST("the codec is usable from several threads at once") {
    // The codec is shared by every worker and its methods are const. The reusable
    // envelope it keeps is thread_local precisely so that sharing is safe; this is
    // the case TSan is pointed at to prove it.
    const ProtobufCodec codec;
    std::atomic<int> mismatches{0};

    {
        std::vector<std::jthread> threads;
        threads.reserve(4);
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 2000; ++i) {
                    lrd::rank::Item item = conformance::sample_item();
                    item.id = static_cast<lrd::rank::ItemId>(t * 100000 + i);
                    item.category = "cat-" + std::to_string(t);

                    Request original;
                    original.request_id = item.id;
                    original.body = PutItem{item};

                    ByteBuffer buffer;
                    codec.encode(original, buffer);

                    Request decoded;
                    const auto* put = (codec.decode(buffer, decoded) == DecodeError::None)
                                          ? std::get_if<PutItem>(&decoded.body)
                                          : nullptr;
                    if (put == nullptr || put->item.id != item.id ||
                        put->item.category != item.category ||
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
