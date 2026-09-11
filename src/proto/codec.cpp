#include "lrd/proto/codec.hpp"

#ifdef LRD_WITH_PROTOBUF
#include "lrd/proto/protobuf_codec.hpp"
#endif

#include <bit>
#include <cstring>
#include <memory>
#include <utility>

namespace lrd::proto {

namespace {

/// Doubles on the wire as IEEE-754 bits in a fixed-width big-endian u64.
///
/// Not a decimal string, and not memcpy of the native representation. The bit
/// pattern is exact - a round trip returns the identical double, including
/// denormals and the sign of zero - whereas a text form loses precision unless
/// printed with enough digits, and would need locale-independent parsing.
///
/// std::bit_cast rather than a reinterpret_cast or a union: it is the only one of
/// the three that is not undefined behaviour. Type punning through a cast breaks
/// strict aliasing, and the union trick is legal in C but not in C++.
std::uint64_t double_to_bits(double value) noexcept {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    return std::bit_cast<std::uint64_t>(value);
}

double bits_to_double(std::uint64_t bits) noexcept {
    return std::bit_cast<double>(bits);
}

void write_item(const rank::Item& item, ByteWriter& writer) {
    writer.u64(item.id);
    writer.string(item.category);
    writer.string(item.advertiser);
    writer.u64(double_to_bits(item.base_score));
    writer.u64(static_cast<std::uint64_t>(rank::to_epoch_millis(item.created_at)));
    writer.u64(static_cast<std::uint64_t>(rank::to_epoch_millis(item.expires_at)));
}

/// Reads an Item. Bounds are checked by the caller via check_item below, after
/// the reader has reported whether it ran out of input - separating "ran out of
/// bytes" from "the values are unacceptable" keeps the two error codes distinct.
rank::Item read_item(ByteReader& reader) {
    rank::Item item;
    item.id = reader.u64();
    item.category = reader.string();
    item.advertiser = reader.string();
    item.base_score = bits_to_double(reader.u64());
    item.created_at = rank::from_epoch_millis(static_cast<std::int64_t>(reader.u64()));
    item.expires_at = rank::from_epoch_millis(static_cast<std::int64_t>(reader.u64()));
    return item;
}

bool item_within_limits(const rank::Item& item) noexcept {
    return item.category.size() <= kMaxCategoryLength &&
           item.advertiser.size() <= kMaxAdvertiserLength;
}

void write_signal(const rank::UserSignal& signal, ByteWriter& writer) {
    writer.u32(static_cast<std::uint32_t>(signal.affinities.size()));
    for (const rank::CategoryAffinity& affinity : signal.affinities) {
        writer.string(affinity.category);
        writer.u64(double_to_bits(affinity.weight));
    }
    writer.u32(static_cast<std::uint32_t>(signal.excluded_categories.size()));
    for (const std::string& category : signal.excluded_categories) {
        writer.string(category);
    }
}

/// Reads a UserSignal, bounding both counts before reserving.
///
/// The count checks matter for the same reason the frame length check does: these
/// are numbers a peer chose, and the loop below would otherwise iterate as many
/// times as asked. Checking before the loop rather than trusting the reader's
/// sticky failure keeps a hostile count from costing a million iterations of
/// failed reads.
bool read_signal(ByteReader& reader, rank::UserSignal& signal) {
    const std::uint32_t affinity_count = reader.u32();
    if (reader.failed() || affinity_count > kMaxAffinities) {
        return false;
    }
    signal.affinities.reserve(affinity_count);
    for (std::uint32_t i = 0; i < affinity_count; ++i) {
        rank::CategoryAffinity affinity;
        affinity.category = reader.string();
        affinity.weight = bits_to_double(reader.u64());
        if (affinity.category.size() > kMaxCategoryLength) {
            return false;
        }
        signal.affinities.push_back(std::move(affinity));
    }

    const std::uint32_t excluded_count = reader.u32();
    if (reader.failed() || excluded_count > kMaxExcluded) {
        return false;
    }
    signal.excluded_categories.reserve(excluded_count);
    for (std::uint32_t i = 0; i < excluded_count; ++i) {
        std::string category = reader.string();
        if (category.size() > kMaxCategoryLength) {
            return false;
        }
        signal.excluded_categories.push_back(std::move(category));
    }
    return true;
}

void write_ranked(const rank::RankedItem& ranked, ByteWriter& writer) {
    writer.u64(ranked.id);
    writer.u64(double_to_bits(ranked.score));
    writer.string(ranked.category);
    writer.string(ranked.advertiser);
}

/// Every message type must consume its payload exactly. Leftover bytes mean the
/// sender and receiver disagree about the shape of this message - a version skew
/// or a bug - and continuing would decode the next frame from the wrong offset.
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
    if (name == "binary" || name == "binary/v2") {
        return std::make_unique<BinaryCodec>();
    }
#ifdef LRD_WITH_PROTOBUF
    if (name == "protobuf" || name == "protobuf/v2") {
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
        // field after this point is meaningless.
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
    encode_header(Header{kVersion, type_of(request.body), 0, request.request_id}, writer);

    std::visit(Overloaded{
                   [&](const GetItem& get) { writer.u64(get.item_id); },
                   [&](const PutItem& put) { write_item(put.item, writer); },
                   [&](const DeleteItem& del) { writer.u64(del.item_id); },
                   [&](const GetStats&) {},
                   [&](const Recommend& recommend) {
                       write_signal(recommend.signal, writer);
                       writer.u32(recommend.count);
                       writer.u8(recommend.dry_run ? 1 : 0);
                   },
               },
               request.body);
}

void BinaryCodec::encode(const Response& response, ByteBuffer& out) const {
    ByteWriter writer(out);
    encode_header(Header{kVersion, type_of(response.body), 0, response.request_id}, writer);

    std::visit(Overloaded{
                   [&](const GetItemResult& result) {
                       writer.u8(static_cast<std::uint8_t>(result.status));
                       // The item is written only when it means something. A
                       // NotFound response is a handful of bytes rather than a
                       // zeroed item, which is both smaller and unambiguous.
                       if (result.status == StatusCode::Ok) {
                           write_item(result.item, writer);
                       }
                   },
                   [&](const PutItemResult& result) {
                       writer.u8(static_cast<std::uint8_t>(result.status));
                   },
                   [&](const DeleteItemResult& result) {
                       writer.u8(static_cast<std::uint8_t>(result.status));
                   },
                   [&](const StatsResult& result) {
                       writer.u8(static_cast<std::uint8_t>(result.status));
                       writer.u64(result.stats.requests);
                       writer.u64(result.stats.gets);
                       writer.u64(result.stats.puts);
                       writer.u64(result.stats.deletes);
                       writer.u64(result.stats.recommends);
                       writer.u64(result.stats.hits);
                       writer.u64(result.stats.misses);
                       writer.u64(result.stats.evictions);
                       writer.u64(result.stats.entries);
                       writer.u64(result.stats.capacity);
                   },
                   [&](const RecommendResult& result) {
                       writer.u8(static_cast<std::uint8_t>(result.status));
                       writer.u32(static_cast<std::uint32_t>(result.items.size()));
                       for (const rank::RankedItem& ranked : result.items) {
                           write_ranked(ranked, writer);
                       }
                   },
                   [&](const Failure& failure) {
                       writer.u8(static_cast<std::uint8_t>(failure.status));
                       writer.string(failure.message);
                   },
               },
               response.body);
}

// --------------------------------------------------------------------------
// Decoding
// --------------------------------------------------------------------------

DecodeError BinaryCodec::decode(ByteView body, Request& out) const {
    ByteReader reader(body);
    Header header;
    if (const DecodeError error = decode_header(reader, header); error != DecodeError::None) {
        return error;
    }
    if (is_response(header.type)) {
        return DecodeError::WrongDirection;
    }

    out = Request{};
    out.request_id = header.request_id;

    switch (header.type) {
        case MessageType::GetItemRequest:
            out.body = GetItem{reader.u64()};
            break;
        case MessageType::DeleteItemRequest:
            out.body = DeleteItem{reader.u64()};
            break;
        case MessageType::PutItemRequest: {
            rank::Item item = read_item(reader);
            if (const DecodeError error = finish(reader); error != DecodeError::None) {
                return error;
            }
            if (!item_within_limits(item)) {
                return DecodeError::FieldTooLarge;
            }
            out.body = PutItem{std::move(item)};
            return DecodeError::None;
        }
        case MessageType::StatsRequest:
            out.body = GetStats{};
            break;
        case MessageType::RecommendRequest: {
            Recommend recommend;
            if (!read_signal(reader, recommend.signal)) {
                return reader.failed() ? DecodeError::Truncated : DecodeError::FieldTooLarge;
            }
            recommend.count = reader.u32();
            recommend.dry_run = reader.u8() != 0;
            if (const DecodeError error = finish(reader); error != DecodeError::None) {
                return error;
            }
            if (recommend.count > kMaxRecommendCount) {
                return DecodeError::FieldTooLarge;
            }
            out.body = std::move(recommend);
            return DecodeError::None;
        }
        default:
            return DecodeError::UnknownType;
    }

    return finish(reader);
}

DecodeError BinaryCodec::decode(ByteView body, Response& out) const {
    ByteReader reader(body);
    Header header;
    if (const DecodeError error = decode_header(reader, header); error != DecodeError::None) {
        return error;
    }
    if (!is_response(header.type)) {
        return DecodeError::WrongDirection;
    }

    out = Response{};
    out.request_id = header.request_id;
    const auto status = static_cast<StatusCode>(reader.u8());

    switch (header.type) {
        case MessageType::GetItemResponse: {
            GetItemResult result;
            result.status = status;
            if (status == StatusCode::Ok) {
                result.item = read_item(reader);
            }
            if (const DecodeError error = finish(reader); error != DecodeError::None) {
                return error;
            }
            if (!item_within_limits(result.item)) {
                return DecodeError::FieldTooLarge;
            }
            out.body = std::move(result);
            return DecodeError::None;
        }
        case MessageType::PutItemResponse:
            out.body = PutItemResult{status};
            break;
        case MessageType::DeleteItemResponse:
            out.body = DeleteItemResult{status};
            break;
        case MessageType::StatsResponse: {
            StatsResult result;
            result.status = status;
            result.stats.requests = reader.u64();
            result.stats.gets = reader.u64();
            result.stats.puts = reader.u64();
            result.stats.deletes = reader.u64();
            result.stats.recommends = reader.u64();
            result.stats.hits = reader.u64();
            result.stats.misses = reader.u64();
            result.stats.evictions = reader.u64();
            result.stats.entries = reader.u64();
            result.stats.capacity = reader.u64();
            out.body = result;
            break;
        }
        case MessageType::RecommendResponse: {
            RecommendResult result;
            result.status = status;
            const std::uint32_t count = reader.u32();
            // Bounded before reserving, for the same reason as the signal counts.
            if (reader.failed() || count > kMaxRecommendCount) {
                return reader.failed() ? DecodeError::Truncated : DecodeError::FieldTooLarge;
            }
            result.items.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                rank::RankedItem ranked;
                ranked.id = reader.u64();
                ranked.score = bits_to_double(reader.u64());
                ranked.category = reader.string();
                ranked.advertiser = reader.string();
                result.items.push_back(std::move(ranked));
            }
            if (const DecodeError error = finish(reader); error != DecodeError::None) {
                return error;
            }
            out.body = std::move(result);
            return DecodeError::None;
        }
        case MessageType::ErrorResponse: {
            Failure failure;
            failure.status = status;
            failure.message = reader.string();
            if (const DecodeError error = finish(reader); error != DecodeError::None) {
                return error;
            }
            if (failure.message.size() > kMaxMessageLength) {
                return DecodeError::FieldTooLarge;
            }
            out.body = std::move(failure);
            return DecodeError::None;
        }
        default:
            return DecodeError::UnknownType;
    }

    return finish(reader);
}

}  // namespace lrd::proto
