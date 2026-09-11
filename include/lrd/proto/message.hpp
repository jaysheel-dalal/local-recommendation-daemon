#pragma once

#include "lrd/rank/item.hpp"
#include "lrd/rank/signal.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace lrd::proto {

/// Protocol constants. See docs/protocol.md for the byte-level spec.
inline constexpr std::uint32_t kMagic = 0x4C524431;  // 'L' 'R' 'D' '1'

/// Bumped to 2 in step 9. The v1 message set was a string key-value store; v2
/// replaces it with item metadata and recommendations. That is not an additive
/// change - the same type bytes now mean different things - so the version had
/// to move. A v1 peer talking to a v2 daemon gets UnsupportedVersion, which is
/// exactly the loud failure the header check exists to produce.
inline constexpr std::uint8_t kVersion = 2;

/// length prefix (4) is *not* counted here; the header is what follows it.
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::size_t kLengthPrefixSize = 4;

/// A frame length is an allocation request from a peer. This cap is what stops
/// four bytes of 0xFF from asking the daemon to reserve 4 GiB.
inline constexpr std::uint32_t kMaxFrameSize = 1024 * 1024;

/// Smallest frame body the framing layer will accept.
///
/// Deliberately 1, not kHeaderSize. The framing layer originally rejected
/// anything shorter than binary/v1's 16-byte header - a codec-specific constant
/// living in a codec-agnostic layer - and it stayed invisible until a second
/// codec existed, whose small responses framing then refused as Oversized.
///
/// Framing's job is to find message boundaries and bound allocation. Whether the
/// bytes inside are long enough to mean anything is the codec's judgement.
inline constexpr std::uint32_t kMinFrameSize = 1;

/// Field limits, enforced by both encoder and decoder.
inline constexpr std::uint32_t kMaxCategoryLength = 64;
inline constexpr std::uint32_t kMaxAdvertiserLength = 64;
inline constexpr std::uint32_t kMaxAffinities = 64;
inline constexpr std::uint32_t kMaxExcluded = 64;
inline constexpr std::uint32_t kMaxRecommendCount = 100;
inline constexpr std::uint32_t kMaxMessageLength = 1024;

/// The high bit marks a response, so a receiver can reject a wrongly-directed
/// message before it parses any payload.
inline constexpr std::uint8_t kResponseBit = 0x80;

enum class MessageType : std::uint8_t {
    GetItemRequest = 0x01,
    PutItemRequest = 0x02,
    DeleteItemRequest = 0x03,
    StatsRequest = 0x04,
    RecommendRequest = 0x05,

    GetItemResponse = 0x81,
    PutItemResponse = 0x82,
    DeleteItemResponse = 0x83,
    StatsResponse = 0x84,
    RecommendResponse = 0x85,
    ErrorResponse = 0xFF,
};

[[nodiscard]] constexpr bool is_response(MessageType type) noexcept {
    return (static_cast<std::uint8_t>(type) & kResponseBit) != 0;
}

[[nodiscard]] bool is_known_type(std::uint8_t raw) noexcept;
[[nodiscard]] const char* to_string(MessageType type) noexcept;

enum class StatusCode : std::uint8_t {
    Ok = 0,
    NotFound = 1,
    InvalidRequest = 2,
    Internal = 3,
};

[[nodiscard]] const char* to_string(StatusCode status) noexcept;

enum class DecodeError : std::uint8_t {
    None = 0,
    Truncated,
    BadMagic,
    UnsupportedVersion,
    ReservedFlags,
    UnknownType,
    WrongDirection,
    TrailingBytes,
    FieldTooLarge,
};

[[nodiscard]] const char* to_string(DecodeError error) noexcept;

struct Header {
    std::uint8_t version = kVersion;
    MessageType type = MessageType::GetItemRequest;
    std::uint16_t flags = 0;
    std::uint64_t request_id = 0;
};

struct Stats {
    std::uint64_t requests = 0;
    std::uint64_t gets = 0;
    std::uint64_t puts = 0;
    std::uint64_t deletes = 0;
    std::uint64_t recommends = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t entries = 0;
    std::uint64_t capacity = 0;
};

// ---------------------------------------------------------------------------
// Request bodies
// ---------------------------------------------------------------------------

struct GetItem {
    rank::ItemId item_id = 0;
};

struct PutItem {
    rank::Item item;
};

struct DeleteItem {
    rank::ItemId item_id = 0;
};

struct GetStats {};

struct Recommend {
    rank::UserSignal signal;
    std::uint32_t count = 0;

    /// Rank and return without recording anything. Inert in step 9 - nothing is
    /// recorded yet - and load bearing in step 10, where a recommendation
    /// reserves exposure against each item's cap. Declared now so that adding
    /// the behaviour later is not a protocol change.
    bool dry_run = false;
};

/// One request, as a discriminated union.
///
/// ## Why std::variant here, having rejected it in v1
///
/// Step 2 used a tagged struct with `key` and `value` fields, and said so
/// explicitly: with four message types sharing two fields, std::visit and the
/// overload-set boilerplate cost more clarity than the safety bought. That note
/// ended "worth revisiting in Phase 2 when item metadata and signal payloads
/// arrive" - and they have.
///
/// The tagged-struct version of v2 would carry an ItemId, an Item, a UserSignal,
/// a count and a flag: five fields, of which at most two are meaningful for any
/// given message. Nothing in the type would say which, so both codecs and every
/// handler would rely on convention, and "a GetItem with a UserSignal attached"
/// would be representable and silently meaningless.
///
/// The variant makes that unrepresentable. The costs are real and worth naming:
/// std::visit needs an overload set (see Overloaded below), the variant occupies
/// as much space as its largest alternative, and a reader has to know how visit
/// works. In exchange, adding an alternative makes every visit site fail to
/// compile until it is handled - which is precisely the mistake a switch over an
/// enum waves through.
using RequestBody = std::variant<GetItem, PutItem, DeleteItem, GetStats, Recommend>;

struct Request {
    std::uint64_t request_id = 0;
    RequestBody body;
};

// ---------------------------------------------------------------------------
// Response bodies
// ---------------------------------------------------------------------------

struct GetItemResult {
    StatusCode status = StatusCode::Ok;
    rank::Item item;  ///< Meaningful only when status == Ok.
};

struct PutItemResult {
    StatusCode status = StatusCode::Ok;
};

struct DeleteItemResult {
    StatusCode status = StatusCode::Ok;  ///< NotFound if the id was absent.
};

struct StatsResult {
    StatusCode status = StatusCode::Ok;
    Stats stats;
};

struct RecommendResult {
    StatusCode status = StatusCode::Ok;
    std::vector<rank::RankedItem> items;
};

struct Failure {
    StatusCode status = StatusCode::Internal;
    std::string message;
};

using ResponseBody = std::variant<GetItemResult, PutItemResult, DeleteItemResult, StatsResult,
                                  RecommendResult, Failure>;

struct Response {
    std::uint64_t request_id = 0;
    ResponseBody body;
};

// ---------------------------------------------------------------------------
// Type discrimination
// ---------------------------------------------------------------------------

/// The wire type byte for a body.
///
/// Deliberately *not* the variant's index(). index() would make the wire format
/// depend on the declaration order of the alternatives, so reordering them for
/// readability would silently break every deployed peer. An explicit mapping
/// lets the two move independently.
[[nodiscard]] MessageType type_of(const RequestBody& body) noexcept;
[[nodiscard]] MessageType type_of(const ResponseBody& body) noexcept;

/// The status carried by any response body.
[[nodiscard]] StatusCode status_of(const ResponseBody& body) noexcept;

/// Builds the overload set std::visit needs out of a pack of lambdas.
///
/// Each lambda is a distinct class with its own operator(); inheriting from all
/// of them and pulling every operator() into one scope with a using-declaration
/// turns them into a single overload set that std::visit can resolve against.
template <typename... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};

[[nodiscard]] Response make_error(std::uint64_t request_id, StatusCode status,
                                  std::string message);

}  // namespace lrd::proto
