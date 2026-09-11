#include "lrd/proto/protobuf_codec.hpp"

#include "lrd.pb.h"

#include <string>
#include <utility>

namespace lrd::proto {

namespace {

namespace pb = lrd::wire;

/// A reusable Envelope, one per thread.
///
/// Constructing a protobuf message per call allocates, for the message and again
/// for each string field. Reusing one and calling Clear() keeps the capacity
/// already grown, so a warm thread does no allocation on the encode path.
///
/// thread_local rather than a member, because Codec methods are const and the
/// codec is shared by every worker: a mutable member would be a data race.
pb::Envelope& scratch_envelope() {
    static thread_local pb::Envelope envelope;
    envelope.Clear();
    return envelope;
}

pb::Status to_wire_status(StatusCode status) noexcept {
    switch (status) {
        case StatusCode::Ok: return pb::STATUS_OK;
        case StatusCode::NotFound: return pb::STATUS_NOT_FOUND;
        case StatusCode::InvalidRequest: return pb::STATUS_INVALID_REQUEST;
        case StatusCode::Internal: return pb::STATUS_INTERNAL;
    }
    return pb::STATUS_INTERNAL;
}

/// proto3 enums are *open*: a value outside the declared set is preserved and
/// handed back as-is rather than rejected. Deliberate - it lets a newer peer
/// introduce a status an older one has never heard of - but it means the value
/// cannot be blindly cast into our closed enum.
StatusCode from_wire_status(pb::Status status) noexcept {
    switch (status) {
        case pb::STATUS_OK: return StatusCode::Ok;
        case pb::STATUS_NOT_FOUND: return StatusCode::NotFound;
        case pb::STATUS_INVALID_REQUEST: return StatusCode::InvalidRequest;
        case pb::STATUS_INTERNAL: return StatusCode::Internal;
        default: return StatusCode::Internal;
    }
}

void to_wire(const rank::Item& item, pb::Item& out) {
    out.set_id(item.id);
    out.set_category(item.category);
    out.set_advertiser(item.advertiser);
    out.set_base_score(item.base_score);
    out.set_created_at_ms(rank::to_epoch_millis(item.created_at));
    out.set_expires_at_ms(rank::to_epoch_millis(item.expires_at));
}

rank::Item from_wire(const pb::Item& in) {
    rank::Item item;
    item.id = in.id();
    item.category = in.category();
    item.advertiser = in.advertiser();
    item.base_score = in.base_score();
    item.created_at = rank::from_epoch_millis(in.created_at_ms());
    item.expires_at = rank::from_epoch_millis(in.expires_at_ms());
    return item;
}

bool item_within_limits(const rank::Item& item) noexcept {
    return item.category.size() <= kMaxCategoryLength &&
           item.advertiser.size() <= kMaxAdvertiserLength;
}

void to_wire(const rank::UserSignal& signal, pb::UserSignal& out) {
    for (const rank::CategoryAffinity& affinity : signal.affinities) {
        pb::CategoryAffinity* entry = out.add_affinities();
        entry->set_category(affinity.category);
        entry->set_weight(affinity.weight);
    }
    for (const std::string& category : signal.excluded_categories) {
        out.add_excluded_categories(category);
    }
}

/// Converts a signal, enforcing the same bounds the binary codec does.
///
/// The decoder cannot assume the peer used our encoder, or our limits - and
/// protobuf's `repeated` is unbounded by construction, so a peer can send a
/// million affinities inside a legal 1 MiB frame. The frame cap bounds the
/// memory; this bounds the work.
bool from_wire(const pb::UserSignal& in, rank::UserSignal& signal) {
    if (in.affinities_size() > static_cast<int>(kMaxAffinities) ||
        in.excluded_categories_size() > static_cast<int>(kMaxExcluded)) {
        return false;
    }
    signal.affinities.reserve(static_cast<std::size_t>(in.affinities_size()));
    for (const pb::CategoryAffinity& entry : in.affinities()) {
        if (entry.category().size() > kMaxCategoryLength) {
            return false;
        }
        signal.affinities.push_back(rank::CategoryAffinity{entry.category(), entry.weight()});
    }
    signal.excluded_categories.reserve(static_cast<std::size_t>(in.excluded_categories_size()));
    for (const std::string& category : in.excluded_categories()) {
        if (category.size() > kMaxCategoryLength) {
            return false;
        }
        signal.excluded_categories.push_back(category);
    }
    return true;
}

/// Appends the envelope's serialisation without disturbing what is already in
/// `out`. The framing layer reserves the four length-prefix bytes before calling
/// encode and patches the length in afterwards, which removes a copy of the
/// whole body - a codec that replaced the buffer would silently break that.
void append_serialized(const pb::Envelope& envelope, ByteBuffer& out) {
    const std::size_t size = envelope.ByteSizeLong();
    const std::size_t offset = out.size();
    out.resize(offset + size);
    if (size > 0) {
        envelope.SerializeToArray(out.data() + offset, static_cast<int>(size));
    }
}

bool parse(ByteView body, pb::Envelope& envelope) {
    return envelope.ParseFromArray(body.data(), static_cast<int>(body.size()));
}

}  // namespace

// --------------------------------------------------------------------------
// Encoding
// --------------------------------------------------------------------------

void ProtobufCodec::encode(const Request& request, ByteBuffer& out) const {
    pb::Envelope& envelope = scratch_envelope();
    envelope.set_request_id(request.request_id);

    std::visit(Overloaded{
                   [&](const GetItem& get) {
                       envelope.mutable_get_item_request()->set_item_id(get.item_id);
                   },
                   [&](const PutItem& put) {
                       to_wire(put.item, *envelope.mutable_put_item_request()->mutable_item());
                   },
                   [&](const DeleteItem& del) {
                       envelope.mutable_delete_item_request()->set_item_id(del.item_id);
                   },
                   [&](const GetStats&) { envelope.mutable_stats_request(); },
                   [&](const Recommend& recommend) {
                       pb::RecommendRequest* out_request = envelope.mutable_recommend_request();
                       to_wire(recommend.signal, *out_request->mutable_signal());
                       out_request->set_count(recommend.count);
                       out_request->set_dry_run(recommend.dry_run);
                   },
               },
               request.body);

    append_serialized(envelope, out);
}

void ProtobufCodec::encode(const Response& response, ByteBuffer& out) const {
    pb::Envelope& envelope = scratch_envelope();
    envelope.set_request_id(response.request_id);

    std::visit(Overloaded{
                   [&](const GetItemResult& result) {
                       pb::GetItemResponse* out_response = envelope.mutable_get_item_response();
                       out_response->set_status(to_wire_status(result.status));
                       if (result.status == StatusCode::Ok) {
                           to_wire(result.item, *out_response->mutable_item());
                       }
                   },
                   [&](const PutItemResult& result) {
                       envelope.mutable_put_item_response()->set_status(
                           to_wire_status(result.status));
                   },
                   [&](const DeleteItemResult& result) {
                       envelope.mutable_delete_item_response()->set_status(
                           to_wire_status(result.status));
                   },
                   [&](const StatsResult& result) {
                       pb::StatsResponse* out_response = envelope.mutable_stats_response();
                       out_response->set_status(to_wire_status(result.status));
                       pb::Stats* stats = out_response->mutable_stats();
                       stats->set_requests(result.stats.requests);
                       stats->set_gets(result.stats.gets);
                       stats->set_puts(result.stats.puts);
                       stats->set_deletes(result.stats.deletes);
                       stats->set_recommends(result.stats.recommends);
                       stats->set_hits(result.stats.hits);
                       stats->set_misses(result.stats.misses);
                       stats->set_evictions(result.stats.evictions);
                       stats->set_entries(result.stats.entries);
                       stats->set_capacity(result.stats.capacity);
                       stats->set_policy_allowed(result.stats.policy_allowed);
                       stats->set_policy_exposure_blocked(result.stats.policy_exposure_blocked);
                       stats->set_policy_frequency_blocked(result.stats.policy_frequency_blocked);
                       stats->set_policy_store_full(result.stats.policy_store_full);
                       stats->set_policy_tracked(result.stats.policy_tracked);
                   },
                   [&](const RecommendResult& result) {
                       pb::RecommendResponse* out_response =
                           envelope.mutable_recommend_response();
                       out_response->set_status(to_wire_status(result.status));
                       for (const rank::RankedItem& ranked : result.items) {
                           pb::RankedItem* entry = out_response->add_items();
                           entry->set_id(ranked.id);
                           entry->set_score(ranked.score);
                           entry->set_category(ranked.category);
                           entry->set_advertiser(ranked.advertiser);
                       }
                   },
                   [&](const Failure& failure) {
                       pb::ErrorResponse* out_response = envelope.mutable_error_response();
                       out_response->set_status(to_wire_status(failure.status));
                       out_response->set_message(failure.message);
                   },
               },
               response.body);

    append_serialized(envelope, out);
}

// --------------------------------------------------------------------------
// Decoding
// --------------------------------------------------------------------------

DecodeError ProtobufCodec::decode(ByteView body, Request& out) const {
    pb::Envelope& envelope = scratch_envelope();
    if (!parse(body, envelope)) {
        // Protobuf does not distinguish "ran out of bytes" from "these bytes are
        // not a message", so both map to Truncated. The framing layer has already
        // guaranteed the body is complete, so that ambiguity costs nothing.
        return DecodeError::Truncated;
    }

    out = Request{};
    out.request_id = envelope.request_id();

    switch (envelope.body_case()) {
        case pb::Envelope::kGetItemRequest:
            out.body = GetItem{envelope.get_item_request().item_id()};
            return DecodeError::None;

        case pb::Envelope::kDeleteItemRequest:
            out.body = DeleteItem{envelope.delete_item_request().item_id()};
            return DecodeError::None;

        case pb::Envelope::kPutItemRequest: {
            rank::Item item = from_wire(envelope.put_item_request().item());
            if (!item_within_limits(item)) {
                return DecodeError::FieldTooLarge;
            }
            out.body = PutItem{std::move(item)};
            return DecodeError::None;
        }

        case pb::Envelope::kStatsRequest:
            out.body = GetStats{};
            return DecodeError::None;

        case pb::Envelope::kRecommendRequest: {
            Recommend recommend;
            const pb::RecommendRequest& in = envelope.recommend_request();
            if (!from_wire(in.signal(), recommend.signal)) {
                return DecodeError::FieldTooLarge;
            }
            recommend.count = in.count();
            recommend.dry_run = in.dry_run();
            if (recommend.count > kMaxRecommendCount) {
                return DecodeError::FieldTooLarge;
            }
            out.body = std::move(recommend);
            return DecodeError::None;
        }

        // A response where a request was expected. The oneof makes this one
        // check on the case, before any payload is touched.
        case pb::Envelope::kGetItemResponse:
        case pb::Envelope::kPutItemResponse:
        case pb::Envelope::kDeleteItemResponse:
        case pb::Envelope::kStatsResponse:
        case pb::Envelope::kRecommendResponse:
        case pb::Envelope::kErrorResponse:
            return DecodeError::WrongDirection;

        case pb::Envelope::BODY_NOT_SET:
            // Either a malformed sender or a newer peer using a oneof arm this
            // build does not know.
            return DecodeError::UnknownType;
    }
    return DecodeError::UnknownType;
}

DecodeError ProtobufCodec::decode(ByteView body, Response& out) const {
    pb::Envelope& envelope = scratch_envelope();
    if (!parse(body, envelope)) {
        return DecodeError::Truncated;
    }

    out = Response{};
    out.request_id = envelope.request_id();

    switch (envelope.body_case()) {
        case pb::Envelope::kGetItemResponse: {
            GetItemResult result;
            result.status = from_wire_status(envelope.get_item_response().status());
            if (result.status == StatusCode::Ok) {
                result.item = from_wire(envelope.get_item_response().item());
                if (!item_within_limits(result.item)) {
                    return DecodeError::FieldTooLarge;
                }
            }
            out.body = std::move(result);
            return DecodeError::None;
        }

        case pb::Envelope::kPutItemResponse:
            out.body = PutItemResult{from_wire_status(envelope.put_item_response().status())};
            return DecodeError::None;

        case pb::Envelope::kDeleteItemResponse:
            out.body =
                DeleteItemResult{from_wire_status(envelope.delete_item_response().status())};
            return DecodeError::None;

        case pb::Envelope::kStatsResponse: {
            StatsResult result;
            result.status = from_wire_status(envelope.stats_response().status());
            const pb::Stats& stats = envelope.stats_response().stats();
            result.stats.requests = stats.requests();
            result.stats.gets = stats.gets();
            result.stats.puts = stats.puts();
            result.stats.deletes = stats.deletes();
            result.stats.recommends = stats.recommends();
            result.stats.hits = stats.hits();
            result.stats.misses = stats.misses();
            result.stats.evictions = stats.evictions();
            result.stats.entries = stats.entries();
            result.stats.capacity = stats.capacity();
            result.stats.policy_allowed = stats.policy_allowed();
            result.stats.policy_exposure_blocked = stats.policy_exposure_blocked();
            result.stats.policy_frequency_blocked = stats.policy_frequency_blocked();
            result.stats.policy_store_full = stats.policy_store_full();
            result.stats.policy_tracked = stats.policy_tracked();
            out.body = result;
            return DecodeError::None;
        }

        case pb::Envelope::kRecommendResponse: {
            RecommendResult result;
            const pb::RecommendResponse& in = envelope.recommend_response();
            result.status = from_wire_status(in.status());
            if (in.items_size() > static_cast<int>(kMaxRecommendCount)) {
                return DecodeError::FieldTooLarge;
            }
            result.items.reserve(static_cast<std::size_t>(in.items_size()));
            for (const pb::RankedItem& entry : in.items()) {
                rank::RankedItem ranked;
                ranked.id = entry.id();
                ranked.score = entry.score();
                ranked.category = entry.category();
                ranked.advertiser = entry.advertiser();
                result.items.push_back(std::move(ranked));
            }
            out.body = std::move(result);
            return DecodeError::None;
        }

        case pb::Envelope::kErrorResponse: {
            Failure failure;
            failure.status = from_wire_status(envelope.error_response().status());
            failure.message = envelope.error_response().message();
            if (failure.message.size() > kMaxMessageLength) {
                return DecodeError::FieldTooLarge;
            }
            out.body = std::move(failure);
            return DecodeError::None;
        }

        case pb::Envelope::kGetItemRequest:
        case pb::Envelope::kPutItemRequest:
        case pb::Envelope::kDeleteItemRequest:
        case pb::Envelope::kStatsRequest:
        case pb::Envelope::kRecommendRequest:
            return DecodeError::WrongDirection;

        case pb::Envelope::BODY_NOT_SET:
            return DecodeError::UnknownType;
    }
    return DecodeError::UnknownType;
}

}  // namespace lrd::proto
