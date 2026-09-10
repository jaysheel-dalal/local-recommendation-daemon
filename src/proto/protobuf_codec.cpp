#include "lrd/proto/protobuf_codec.hpp"

#include "lrd.pb.h"

#include <string>
#include <utility>

namespace lrd::proto {

namespace {

namespace pb = lrd::wire;

/// A reusable Envelope, one per thread.
///
/// Constructing a protobuf message per call allocates - for the message and
/// again for each string field. Reusing one and calling Clear() keeps the
/// capacity that was already grown, so a warm thread does no allocation at all
/// on the encode path. That is the same reasoning behind the scratch ByteBuffer
/// the framing layer reuses per connection.
///
/// thread_local rather than a member, because Codec methods are const and the
/// codec is shared by every worker: a mutable member would be a data race.
/// thread_local gives each worker its own without any synchronisation, and the
/// memory is bounded by thread count rather than by request rate.
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
/// handed back as-is rather than rejected. That is deliberate - it lets a newer
/// peer introduce a status an older one has never heard of - but it means the
/// value cannot be blindly cast into our closed enum. Anything unrecognised
/// becomes Internal, which is the honest reading of "something went wrong that
/// this build does not understand".
StatusCode from_wire_status(pb::Status status) noexcept {
    switch (status) {
        case pb::STATUS_OK: return StatusCode::Ok;
        case pb::STATUS_NOT_FOUND: return StatusCode::NotFound;
        case pb::STATUS_INVALID_REQUEST: return StatusCode::InvalidRequest;
        case pb::STATUS_INTERNAL: return StatusCode::Internal;
        default: return StatusCode::Internal;
    }
}

/// Appends the envelope's serialisation to `out` without disturbing what is
/// already there.
///
/// Appending is a contract the framing layer depends on: write_message reserves
/// the four length-prefix bytes *before* calling encode and patches the real
/// length in afterwards, which removes a copy of the whole body. A codec that
/// replaced the buffer would silently break that optimisation, so the
/// conformance suite checks it.
void append_serialized(const pb::Envelope& envelope, ByteBuffer& out) {
    const std::size_t size = envelope.ByteSizeLong();
    const std::size_t offset = out.size();
    out.resize(offset + size);
    if (size > 0) {
        envelope.SerializeToArray(out.data() + offset, static_cast<int>(size));
    }
}

/// Parses into a per-thread envelope. Returns false if the bytes are not a
/// well-formed message.
bool parse(ByteView body, pb::Envelope& envelope) {
    // ParseFromArray clears first, so reusing the envelope carries no risk of
    // fields left over from the previous message.
    return envelope.ParseFromArray(body.data(), static_cast<int>(body.size()));
}

std::string to_std_string(const std::string& value) {
    return value;
}

}  // namespace

// --------------------------------------------------------------------------
// Encoding
// --------------------------------------------------------------------------

void ProtobufCodec::encode(const Request& request, ByteBuffer& out) const {
    pb::Envelope& envelope = scratch_envelope();
    envelope.set_request_id(request.request_id);

    switch (request.type) {
        case MessageType::GetRequest:
            envelope.mutable_get_request()->set_key(request.key);
            break;
        case MessageType::PutRequest: {
            pb::PutRequest* put = envelope.mutable_put_request();
            put->set_key(request.key);
            put->set_value(request.value);
            break;
        }
        case MessageType::DeleteRequest:
            envelope.mutable_delete_request()->set_key(request.key);
            break;
        case MessageType::StatsRequest:
            envelope.mutable_stats_request();
            break;
        default:
            // Encoding a response through the request overload is a programming
            // error rather than a wire condition. Leaving the oneof unset keeps
            // this total; the receiver rejects it as UnknownType.
            break;
    }

    append_serialized(envelope, out);
}

void ProtobufCodec::encode(const Response& response, ByteBuffer& out) const {
    pb::Envelope& envelope = scratch_envelope();
    envelope.set_request_id(response.request_id);
    const pb::Status status = to_wire_status(response.status);

    switch (response.type) {
        case MessageType::GetResponse: {
            pb::GetResponse* get = envelope.mutable_get_response();
            get->set_status(status);
            get->set_value(response.value);
            break;
        }
        case MessageType::PutResponse:
            envelope.mutable_put_response()->set_status(status);
            break;
        case MessageType::DeleteResponse:
            envelope.mutable_delete_response()->set_status(status);
            break;
        case MessageType::StatsResponse: {
            pb::StatsResponse* stats_response = envelope.mutable_stats_response();
            stats_response->set_status(status);
            pb::Stats* stats = stats_response->mutable_stats();
            stats->set_requests(response.stats.requests);
            stats->set_gets(response.stats.gets);
            stats->set_puts(response.stats.puts);
            stats->set_deletes(response.stats.deletes);
            stats->set_hits(response.stats.hits);
            stats->set_misses(response.stats.misses);
            stats->set_evictions(response.stats.evictions);
            stats->set_entries(response.stats.entries);
            stats->set_capacity(response.stats.capacity);
            break;
        }
        case MessageType::ErrorResponse: {
            pb::ErrorResponse* error = envelope.mutable_error_response();
            error->set_status(status);
            error->set_message(response.value);
            break;
        }
        default:
            break;
    }

    append_serialized(envelope, out);
}

// --------------------------------------------------------------------------
// Decoding
// --------------------------------------------------------------------------

DecodeError ProtobufCodec::decode(ByteView body, Request& out) const {
    pb::Envelope& envelope = scratch_envelope();
    if (!parse(body, envelope)) {
        // Protobuf does not distinguish "ran out of bytes" from "these bytes
        // are not a message", so both map to Truncated. In practice the framing
        // layer has already guaranteed the body is complete, which is why that
        // ambiguity does not cost us anything here.
        return DecodeError::Truncated;
    }

    out = Request{};
    out.request_id = envelope.request_id();

    switch (envelope.body_case()) {
        case pb::Envelope::kGetRequest:
            out.type = MessageType::GetRequest;
            out.key = to_std_string(envelope.get_request().key());
            break;
        case pb::Envelope::kPutRequest:
            out.type = MessageType::PutRequest;
            out.key = to_std_string(envelope.put_request().key());
            out.value = to_std_string(envelope.put_request().value());
            break;
        case pb::Envelope::kDeleteRequest:
            out.type = MessageType::DeleteRequest;
            out.key = to_std_string(envelope.delete_request().key());
            break;
        case pb::Envelope::kStatsRequest:
            out.type = MessageType::StatsRequest;
            break;

        // A response arriving where a request was expected. The oneof makes
        // this a single check on the case, before any payload is touched.
        case pb::Envelope::kGetResponse:
        case pb::Envelope::kPutResponse:
        case pb::Envelope::kDeleteResponse:
        case pb::Envelope::kStatsResponse:
        case pb::Envelope::kErrorResponse:
            return DecodeError::WrongDirection;

        case pb::Envelope::BODY_NOT_SET:
            // Either a malformed sender or a newer peer using a oneof arm this
            // build does not know. Unknown fields are preserved rather than
            // rejected, so the message survives - we simply cannot act on it.
            return DecodeError::UnknownType;
    }

    // Enforced on the way in as well as the way out: the decoder cannot assume
    // the peer used our encoder, or our limits.
    if (out.key.size() > kMaxKeyLength || out.value.size() > kMaxValueLength) {
        return DecodeError::FieldTooLarge;
    }
    return DecodeError::None;
}

DecodeError ProtobufCodec::decode(ByteView body, Response& out) const {
    pb::Envelope& envelope = scratch_envelope();
    if (!parse(body, envelope)) {
        return DecodeError::Truncated;
    }

    out = Response{};
    out.request_id = envelope.request_id();

    switch (envelope.body_case()) {
        case pb::Envelope::kGetResponse:
            out.type = MessageType::GetResponse;
            out.status = from_wire_status(envelope.get_response().status());
            out.value = to_std_string(envelope.get_response().value());
            break;
        case pb::Envelope::kPutResponse:
            out.type = MessageType::PutResponse;
            out.status = from_wire_status(envelope.put_response().status());
            break;
        case pb::Envelope::kDeleteResponse:
            out.type = MessageType::DeleteResponse;
            out.status = from_wire_status(envelope.delete_response().status());
            break;
        case pb::Envelope::kStatsResponse: {
            out.type = MessageType::StatsResponse;
            out.status = from_wire_status(envelope.stats_response().status());
            const pb::Stats& stats = envelope.stats_response().stats();
            out.stats.requests = stats.requests();
            out.stats.gets = stats.gets();
            out.stats.puts = stats.puts();
            out.stats.deletes = stats.deletes();
            out.stats.hits = stats.hits();
            out.stats.misses = stats.misses();
            out.stats.evictions = stats.evictions();
            out.stats.entries = stats.entries();
            out.stats.capacity = stats.capacity();
            break;
        }
        case pb::Envelope::kErrorResponse:
            out.type = MessageType::ErrorResponse;
            out.status = from_wire_status(envelope.error_response().status());
            out.value = to_std_string(envelope.error_response().message());
            break;

        case pb::Envelope::kGetRequest:
        case pb::Envelope::kPutRequest:
        case pb::Envelope::kDeleteRequest:
        case pb::Envelope::kStatsRequest:
            return DecodeError::WrongDirection;

        case pb::Envelope::BODY_NOT_SET:
            return DecodeError::UnknownType;
    }

    return DecodeError::None;
}

}  // namespace lrd::proto
