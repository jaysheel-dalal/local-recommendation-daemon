#include "lrd/proto/codec.hpp"

#ifdef LRD_WITH_PROTOBUF
#include "lrd/proto/protobuf_codec.hpp"
#endif

#include <memory>

namespace lrd::proto {

namespace {

/// Shared by both decode() overloads: everything up to and including the
/// payload-independent validation.
DecodeError read_common(ByteView body, ByteReader& reader, Header& header, bool want_response) {
    const DecodeError error = decode_header(reader, header);
    if (error != DecodeError::None) {
        return error;
    }

    // A daemon must never find itself processing a GetResponse, and a client
    // must never process a PutRequest. Rejecting on direction catches a
    // crossed-wires bug immediately rather than as a confusing field mismatch
    // three layers down.
    if (is_response(header.type) != want_response) {
        return DecodeError::WrongDirection;
    }

    (void)body;
    return DecodeError::None;
}

/// Every message type must consume its payload exactly. Leftover bytes mean
/// the sender and receiver disagree about the shape of this message - a
/// version skew or a bug - and continuing would decode the next frame from the
/// wrong offset.
DecodeError finish(ByteReader& reader) noexcept {
    if (reader.failed()) {
        return DecodeError::Truncated;
    }
    if (!reader.exhausted()) {
        return DecodeError::TrailingBytes;
    }
    return DecodeError::None;
}

}  // namespace

std::unique_ptr<Codec> make_codec(std::string_view name) {
    if (name == "binary" || name == "binary/v1") {
        return std::make_unique<BinaryCodec>();
    }
#ifdef LRD_WITH_PROTOBUF
    if (name == "protobuf" || name == "protobuf/v1") {
        return std::make_unique<ProtobufCodec>();
    }
#endif
    return nullptr;
}

std::string_view available_codecs() noexcept {
#ifdef LRD_WITH_PROTOBUF
    return "binary, protobuf";
#else
    return "binary (this build has no protobuf support)";
#endif
}

void encode_header(const Header& header, ByteWriter& writer) {
    writer.u32(kMagic);
    writer.u8(header.version);
    writer.u8(static_cast<std::uint8_t>(header.type));
    writer.u16(header.flags);
    writer.u64(header.request_id);
}

DecodeError decode_header(ByteReader& reader, Header& out) noexcept {
    const std::uint32_t magic = reader.u32();
    if (reader.failed()) {
        return DecodeError::Truncated;
    }
    if (magic != kMagic) {
        // Checked before anything else: if the stream has desynchronised, every
        // field after this point is meaningless, and the sooner we say so the
        // less garbage gets interpreted.
        return DecodeError::BadMagic;
    }

    out.version = reader.u8();
    const std::uint8_t raw_type = reader.u8();
    out.flags = reader.u16();
    out.request_id = reader.u64();

    if (reader.failed()) {
        return DecodeError::Truncated;
    }
    if (out.version != kVersion) {
        return DecodeError::UnsupportedVersion;
    }
    // Rejecting non-zero reserved flags is what makes the field usable later:
    // if v1 daemons ignored unknown flags, a v2 client could not tell whether
    // its request had been honoured or silently downgraded.
    if (out.flags != 0) {
        return DecodeError::ReservedFlags;
    }
    if (!is_known_type(raw_type)) {
        return DecodeError::UnknownType;
    }

    out.type = static_cast<MessageType>(raw_type);
    return DecodeError::None;
}

// --------------------------------------------------------------------------
// Encoding
// --------------------------------------------------------------------------

void BinaryCodec::encode(const Request& request, ByteBuffer& out) const {
    ByteWriter writer(out);
    encode_header(Header{kVersion, request.type, 0, request.request_id}, writer);

    switch (request.type) {
        case MessageType::GetRequest:
        case MessageType::DeleteRequest:
            writer.string(request.key);
            break;
        case MessageType::PutRequest:
            writer.string(request.key);
            writer.string(request.value);
            break;
        case MessageType::StatsRequest:
            break;  // no payload
        default:
            // Encoding a response type through the request overload is a
            // programming error, not a wire condition. Emitting just the header
            // keeps this total rather than undefined; the receiver will reject
            // it as WrongDirection.
            break;
    }
}

void BinaryCodec::encode(const Response& response, ByteBuffer& out) const {
    ByteWriter writer(out);
    encode_header(Header{kVersion, response.type, 0, response.request_id}, writer);

    switch (response.type) {
        case MessageType::GetResponse:
            writer.u8(static_cast<std::uint8_t>(response.status));
            writer.string(response.value);
            break;
        case MessageType::PutResponse:
        case MessageType::DeleteResponse:
            writer.u8(static_cast<std::uint8_t>(response.status));
            break;
        case MessageType::StatsResponse:
            writer.u8(static_cast<std::uint8_t>(response.status));
            writer.u64(response.stats.requests);
            writer.u64(response.stats.gets);
            writer.u64(response.stats.puts);
            writer.u64(response.stats.deletes);
            writer.u64(response.stats.hits);
            writer.u64(response.stats.misses);
            writer.u64(response.stats.evictions);
            writer.u64(response.stats.entries);
            writer.u64(response.stats.capacity);
            break;
        case MessageType::ErrorResponse:
            writer.u8(static_cast<std::uint8_t>(response.status));
            writer.string(response.value);
            break;
        default:
            break;
    }
}

// --------------------------------------------------------------------------
// Decoding
// --------------------------------------------------------------------------

DecodeError BinaryCodec::decode(ByteView body, Request& out) const {
    ByteReader reader(body);
    Header header;
    if (const DecodeError error = read_common(body, reader, header, /*want_response=*/false);
        error != DecodeError::None) {
        return error;
    }

    out = Request{};
    out.type = header.type;
    out.request_id = header.request_id;

    switch (header.type) {
        case MessageType::GetRequest:
        case MessageType::DeleteRequest:
            out.key = reader.string();
            break;
        case MessageType::PutRequest:
            out.key = reader.string();
            out.value = reader.string();
            break;
        case MessageType::StatsRequest:
            break;
        default:
            return DecodeError::UnknownType;
    }

    if (const DecodeError error = finish(reader); error != DecodeError::None) {
        return error;
    }

    // Length caps are enforced on the way in as well as the way out. The
    // decoder cannot trust that the peer used our encoder - or our limits.
    if (out.key.size() > kMaxKeyLength || out.value.size() > kMaxValueLength) {
        return DecodeError::FieldTooLarge;
    }
    return DecodeError::None;
}

DecodeError BinaryCodec::decode(ByteView body, Response& out) const {
    ByteReader reader(body);
    Header header;
    if (const DecodeError error = read_common(body, reader, header, /*want_response=*/true);
        error != DecodeError::None) {
        return error;
    }

    out = Response{};
    out.type = header.type;
    out.request_id = header.request_id;
    out.status = static_cast<StatusCode>(reader.u8());

    switch (header.type) {
        case MessageType::GetResponse:
        case MessageType::ErrorResponse:
            out.value = reader.string();
            break;
        case MessageType::PutResponse:
        case MessageType::DeleteResponse:
            break;
        case MessageType::StatsResponse:
            out.stats.requests = reader.u64();
            out.stats.gets = reader.u64();
            out.stats.puts = reader.u64();
            out.stats.deletes = reader.u64();
            out.stats.hits = reader.u64();
            out.stats.misses = reader.u64();
            out.stats.evictions = reader.u64();
            out.stats.entries = reader.u64();
            out.stats.capacity = reader.u64();
            break;
        default:
            return DecodeError::UnknownType;
    }

    return finish(reader);
}

}  // namespace lrd::proto
