#pragma once

// Behaviour every Codec implementation must have, independent of its wire
// format. Run against BinaryCodec and ProtobufCodec; a codec that passes this is
// substitutable for the other in the daemon.
//
// ## What is deliberately NOT here
//
// Error *detection* is format-specific, and pretending otherwise would force one
// of the codecs to fake behaviour it does not naturally have:
//
//   * **Bad magic, version, reserved flags.** binary/v2 puts these in a fixed
//     header and rejects them. A protobuf envelope has no such header.
//   * **Trailing bytes.** binary/v2 treats extra bytes as a fatal disagreement
//     about message shape; protobuf is required to *preserve* unknown fields -
//     the mechanism that makes schema evolution work.
//   * **Truncation.** binary/v2 rejects a body cut at any offset. Protobuf
//     generally cannot: a message of optional fields truncated on a field
//     boundary parses cleanly as a valid shorter message.
//
// That last point justifies a decision made in step 2: **the length prefix is
// not redundant with protobuf.** Protobuf cannot reliably tell you a message was
// cut short; the framing layer's length prefix is what makes truncation
// detectable at all.
//
// So this header covers round-trip semantics - what a message means - and each
// codec's own test file covers how its format detects corruption.

#include "lrd/proto/codec.hpp"
#include "lrd/proto/message.hpp"
#include "lrd/rank/item.hpp"
#include "lrd/rank/signal.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace lrd::proto::conformance {

inline rank::Item sample_item() {
    rank::Item item;
    item.id = 0xFEEDFACECAFEBEEFULL;  // exercises the full 64-bit id
    item.category = "tech";
    item.advertiser = "acme";
    item.base_score = 0.8125;  // exactly representable, so equality is meaningful
    item.created_at = rank::from_epoch_millis(1'700'000'000'000);
    item.expires_at = rank::from_epoch_millis(1'800'000'000'000);
    return item;
}

inline void check_item_equal(const rank::Item& actual, const rank::Item& expected) {
    LRD_CHECK_EQ(actual.id, expected.id);
    LRD_CHECK_EQ(actual.category, expected.category);
    LRD_CHECK_EQ(actual.advertiser, expected.advertiser);
    LRD_CHECK_EQ(actual.base_score, expected.base_score);
    LRD_CHECK_EQ(rank::to_epoch_millis(actual.created_at),
                 rank::to_epoch_millis(expected.created_at));
    LRD_CHECK_EQ(rank::to_epoch_millis(actual.expires_at),
                 rank::to_epoch_millis(expected.expires_at));
}

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

// --------------------------------------------------------------------------
// Requests
// --------------------------------------------------------------------------

inline void check_get_item(const Codec& codec) {
    Request original;
    original.request_id = 42;
    original.body = GetItem{12345};

    Request decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    LRD_CHECK_EQ(decoded.request_id, std::uint64_t{42});
    const auto* get = std::get_if<GetItem>(&decoded.body);
    LRD_REQUIRE(get != nullptr);
    LRD_CHECK_EQ(get->item_id, rank::ItemId{12345});
}

inline void check_put_item(const Codec& codec) {
    Request original;
    original.request_id = 0xFFFFFFFFFFFFFFFFULL;
    original.body = PutItem{sample_item()};

    Request decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    LRD_CHECK_EQ(decoded.request_id, original.request_id);
    const auto* put = std::get_if<PutItem>(&decoded.body);
    LRD_REQUIRE(put != nullptr);
    check_item_equal(put->item, sample_item());
}

inline void check_delete_item(const Codec& codec) {
    Request original;
    original.request_id = 7;
    original.body = DeleteItem{999};

    Request decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    const auto* del = std::get_if<DeleteItem>(&decoded.body);
    LRD_REQUIRE(del != nullptr);
    LRD_CHECK_EQ(del->item_id, rank::ItemId{999});
}

inline void check_stats_request(const Codec& codec) {
    Request original;
    original.request_id = 9;
    original.body = GetStats{};

    Request decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    LRD_CHECK(std::holds_alternative<GetStats>(decoded.body));
    LRD_CHECK_EQ(decoded.request_id, std::uint64_t{9});
}

inline void check_recommend(const Codec& codec) {
    Recommend recommend;
    recommend.signal.affinities = {{"tech", 0.75}, {"sport", 0.25}};
    recommend.signal.excluded_categories = {"gambling", "politics"};
    recommend.count = 7;
    recommend.dry_run = true;

    Request original;
    original.request_id = 11;
    original.body = recommend;

    Request decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    const auto* out = std::get_if<Recommend>(&decoded.body);
    LRD_REQUIRE(out != nullptr);
    LRD_CHECK_EQ(out->count, std::uint32_t{7});
    LRD_CHECK(out->dry_run);
    LRD_REQUIRE(out->signal.affinities.size() == 2);
    LRD_CHECK_EQ(out->signal.affinities[0].category, std::string("tech"));
    LRD_CHECK_EQ(out->signal.affinities[0].weight, 0.75);
    LRD_CHECK_EQ(out->signal.affinities[1].category, std::string("sport"));
    LRD_REQUIRE(out->signal.excluded_categories.size() == 2);
    LRD_CHECK_EQ(out->signal.excluded_categories[0], std::string("gambling"));
    LRD_CHECK_EQ(out->signal.excluded_categories[1], std::string("politics"));
}

inline void check_empty_signal(const Codec& codec) {
    // A brand-new user sends nothing. Both codecs must round-trip that rather
    // than producing a degenerate frame.
    Request original;
    original.request_id = 12;
    original.body = Recommend{rank::UserSignal{}, 3, false};

    Request decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    const auto* out = std::get_if<Recommend>(&decoded.body);
    LRD_REQUIRE(out != nullptr);
    LRD_CHECK(out->signal.affinities.empty());
    LRD_CHECK(out->signal.excluded_categories.empty());
    LRD_CHECK(!out->dry_run);
}

// --------------------------------------------------------------------------
// Responses
// --------------------------------------------------------------------------

inline void check_get_item_hit(const Codec& codec) {
    Response original;
    original.request_id = 1;
    original.body = GetItemResult{StatusCode::Ok, sample_item()};

    Response decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    const auto* result = std::get_if<GetItemResult>(&decoded.body);
    LRD_REQUIRE(result != nullptr);
    LRD_CHECK(result->status == StatusCode::Ok);
    check_item_equal(result->item, sample_item());
}

inline void check_get_item_miss(const Codec& codec) {
    Response original;
    original.request_id = 2;
    original.body = GetItemResult{StatusCode::NotFound, {}};

    Response decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    const auto* result = std::get_if<GetItemResult>(&decoded.body);
    LRD_REQUIRE(result != nullptr);
    LRD_CHECK(result->status == StatusCode::NotFound);
}

inline void check_simple_responses(const Codec& codec) {
    for (const StatusCode status : {StatusCode::Ok, StatusCode::NotFound}) {
        {
            Response original;
            original.body = PutItemResult{status};
            Response decoded;
            LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
            const auto* result = std::get_if<PutItemResult>(&decoded.body);
            LRD_REQUIRE(result != nullptr);
            LRD_CHECK(result->status == status);
        }
        {
            Response original;
            original.body = DeleteItemResult{status};
            Response decoded;
            LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
            const auto* result = std::get_if<DeleteItemResult>(&decoded.body);
            LRD_REQUIRE(result != nullptr);
            LRD_CHECK(result->status == status);
        }
    }
}

inline void check_stats_response(const Codec& codec) {
    Response original;
    original.body = StatsResult{StatusCode::Ok, Stats{100, 60, 30, 10, 5, 45, 15, 4, 20, 64}};

    Response decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    const auto* result = std::get_if<StatsResult>(&decoded.body);
    LRD_REQUIRE(result != nullptr);
    LRD_CHECK_EQ(result->stats.requests, std::uint64_t{100});
    LRD_CHECK_EQ(result->stats.gets, std::uint64_t{60});
    LRD_CHECK_EQ(result->stats.puts, std::uint64_t{30});
    LRD_CHECK_EQ(result->stats.deletes, std::uint64_t{10});
    LRD_CHECK_EQ(result->stats.recommends, std::uint64_t{5});
    LRD_CHECK_EQ(result->stats.hits, std::uint64_t{45});
    LRD_CHECK_EQ(result->stats.misses, std::uint64_t{15});
    LRD_CHECK_EQ(result->stats.evictions, std::uint64_t{4});
    LRD_CHECK_EQ(result->stats.entries, std::uint64_t{20});
    LRD_CHECK_EQ(result->stats.capacity, std::uint64_t{64});
}

inline void check_recommend_response(const Codec& codec) {
    RecommendResult result;
    result.status = StatusCode::Ok;
    result.items = {
        rank::RankedItem{10, 0.875, "tech", "acme"},
        rank::RankedItem{20, 0.5, "sport", "globex"},
        rank::RankedItem{30, 0.0625, "food", "initech"},
    };

    Response original;
    original.request_id = 5;
    original.body = result;

    Response decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    const auto* out = std::get_if<RecommendResult>(&decoded.body);
    LRD_REQUIRE(out != nullptr);
    LRD_REQUIRE(out->items.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        LRD_CHECK_EQ(out->items[i].id, result.items[i].id);
        // Exact equality: scores drive ordering, so an approximate encoding
        // would silently reorder near-ties.
        LRD_CHECK_EQ(out->items[i].score, result.items[i].score);
        LRD_CHECK_EQ(out->items[i].category, result.items[i].category);
        LRD_CHECK_EQ(out->items[i].advertiser, result.items[i].advertiser);
    }
}

inline void check_empty_recommend_response(const Codec& codec) {
    Response original;
    original.body = RecommendResult{StatusCode::Ok, {}};

    Response decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    const auto* out = std::get_if<RecommendResult>(&decoded.body);
    LRD_REQUIRE(out != nullptr);
    LRD_CHECK(out->items.empty());
}

inline void check_failure(const Codec& codec) {
    Response original;
    original.request_id = 9;
    original.body = Failure{StatusCode::InvalidRequest, "category must not be empty"};

    Response decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    const auto* failure = std::get_if<Failure>(&decoded.body);
    LRD_REQUIRE(failure != nullptr);
    LRD_CHECK(failure->status == StatusCode::InvalidRequest);
    LRD_CHECK_EQ(failure->message, std::string("category must not be empty"));
}

// --------------------------------------------------------------------------
// Cross-cutting properties
// --------------------------------------------------------------------------

inline void check_direction_is_enforced(const Codec& codec) {
    // A daemon must never process a GetItemResponse and a client must never
    // process a PutItemRequest.
    Response response;
    response.request_id = 1;
    response.body = GetItemResult{StatusCode::Ok, sample_item()};
    ByteBuffer response_bytes;
    codec.encode(response, response_bytes);

    Request as_request;
    LRD_CHECK(codec.decode(response_bytes, as_request) == DecodeError::WrongDirection);

    Request request;
    request.body = GetItem{1};
    ByteBuffer request_bytes;
    codec.encode(request, request_bytes);

    Response as_response;
    LRD_CHECK(codec.decode(request_bytes, as_response) == DecodeError::WrongDirection);
}

inline void check_field_limits(const Codec& codec) {
    {
        rank::Item item = sample_item();
        item.category = std::string(kMaxCategoryLength, 'c');
        Request original;
        original.body = PutItem{item};
        Request decoded;
        LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::None);
    }
    {
        rank::Item item = sample_item();
        item.category = std::string(kMaxCategoryLength + 1, 'c');
        Request original;
        original.request_id = 3;
        original.body = PutItem{item};
        Request decoded;
        LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::FieldTooLarge);
        // The id survives, so the daemon can address its rejection to the right
        // request rather than dropping the connection.
        LRD_CHECK_EQ(decoded.request_id, std::uint64_t{3});
    }
    {
        Recommend recommend;
        recommend.count = kMaxRecommendCount + 1;
        Request original;
        original.body = recommend;
        Request decoded;
        LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::FieldTooLarge);
    }
    {
        // Unbounded repeated fields are the protobuf-shaped version of the same
        // hazard the frame-length cap exists for: a legal frame can still ask for
        // a great deal of work.
        Recommend recommend;
        recommend.count = 1;
        recommend.signal.affinities.resize(kMaxAffinities + 1);
        Request original;
        original.body = recommend;
        Request decoded;
        LRD_CHECK(round_trip(codec, original, decoded) == DecodeError::FieldTooLarge);
    }
}

inline void check_binary_content_survives(const Codec& codec) {
    // Categories and advertisers are byte strings, not text. A protobuf `string`
    // field would be required to be valid UTF-8 and could reject this; `bytes`
    // does not.
    static const unsigned char kRaw[] = {0xFF, 0xFE, 0x00, 0x80, 'x'};

    rank::Item item = sample_item();
    item.category = std::string(reinterpret_cast<const char*>(kRaw), sizeof(kRaw));
    item.advertiser = std::string("a\0b", 3);

    Request original;
    original.body = PutItem{item};
    Request decoded;
    LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
    const auto* put = std::get_if<PutItem>(&decoded.body);
    LRD_REQUIRE(put != nullptr);
    LRD_CHECK_EQ(put->item.category, item.category);
    LRD_CHECK_EQ(put->item.advertiser, item.advertiser);
}

inline void check_extreme_doubles(const Codec& codec) {
    // Scores are compared for ordering, so the encoding has to be exact - not
    // merely close. Denormals and the sign of zero are the cases a decimal-text
    // encoding loses.
    for (const double score : {0.0, -0.0, 1.0, 0.1, 1e-300, 1e300, 0.30000000000000004}) {
        rank::Item item = sample_item();
        item.base_score = score;

        Request original;
        original.body = PutItem{item};
        Request decoded;
        LRD_REQUIRE(round_trip(codec, original, decoded) == DecodeError::None);
        const auto* put = std::get_if<PutItem>(&decoded.body);
        LRD_REQUIRE(put != nullptr);
        LRD_CHECK_EQ(put->item.base_score, score);
    }
}

inline void check_encode_appends(const Codec& codec) {
    // write_message reserves the length prefix before encoding and patches it
    // afterwards, which only works because encode appends rather than replaces.
    ByteBuffer buffer;
    buffer.push_back(std::byte{0xAA});
    buffer.push_back(std::byte{0xBB});

    Request request;
    request.body = GetItem{1};
    codec.encode(request, buffer);

    LRD_REQUIRE(buffer.size() > 2);
    LRD_CHECK(buffer[0] == std::byte{0xAA});
    LRD_CHECK(buffer[1] == std::byte{0xBB});
}

inline void check_reused_object_is_cleared(const Codec& codec) {
    // Request objects are reused for a connection's lifetime. With a variant this
    // is largely structural - assigning a new alternative destroys the old one -
    // but it is exactly the bug the tagged-struct version had, so it stays tested.
    Request put;
    put.body = PutItem{sample_item()};
    ByteBuffer put_bytes;
    codec.encode(put, put_bytes);

    Request get;
    get.body = GetItem{5};
    ByteBuffer get_bytes;
    codec.encode(get, get_bytes);

    Request reused;
    LRD_REQUIRE(codec.decode(put_bytes, reused) == DecodeError::None);
    LRD_REQUIRE(std::holds_alternative<PutItem>(reused.body));

    LRD_REQUIRE(codec.decode(get_bytes, reused) == DecodeError::None);
    LRD_CHECK(std::holds_alternative<GetItem>(reused.body));
}

/// Runs every conformance check. Each codec's test file calls this once.
inline void run_all(const Codec& codec) {
    check_get_item(codec);
    check_put_item(codec);
    check_delete_item(codec);
    check_stats_request(codec);
    check_recommend(codec);
    check_empty_signal(codec);

    check_get_item_hit(codec);
    check_get_item_miss(codec);
    check_simple_responses(codec);
    check_stats_response(codec);
    check_recommend_response(codec);
    check_empty_recommend_response(codec);
    check_failure(codec);

    check_direction_is_enforced(codec);
    check_field_limits(codec);
    check_binary_content_survives(codec);
    check_extreme_doubles(codec);
    check_encode_appends(codec);
    check_reused_object_is_cleared(codec);
}

}  // namespace lrd::proto::conformance
