#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lrd::proto {

/// Protocol constants. See docs/protocol.md for the byte-level spec.
inline constexpr std::uint32_t kMagic = 0x4C524431;  // 'L' 'R' 'D' '1'
inline constexpr std::uint8_t kVersion = 1;

/// length prefix (4) is *not* counted here; the header is what follows it.
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::size_t kLengthPrefixSize = 4;

/// A frame length is an allocation request from a peer. These caps are what
/// stop four bytes of 0xFF from asking the daemon to reserve 4 GiB.
inline constexpr std::uint32_t kMaxFrameSize = 1024 * 1024;
inline constexpr std::uint32_t kMaxKeyLength = 1024;
inline constexpr std::uint32_t kMaxValueLength = 512 * 1024;

/// The high bit marks a response, so a receiver can reject a wrongly-directed
/// message before it parses any payload - a daemon should never be processing
/// a GetResponse, and saying so in the type space is cheaper than checking in
/// every handler.
inline constexpr std::uint8_t kResponseBit = 0x80;

enum class MessageType : std::uint8_t {
    GetRequest = 0x01,
    PutRequest = 0x02,
    DeleteRequest = 0x03,
    StatsRequest = 0x04,

    GetResponse = 0x81,
    PutResponse = 0x82,
    DeleteResponse = 0x83,
    StatsResponse = 0x84,
    ErrorResponse = 0xFF,
};

[[nodiscard]] constexpr bool is_response(MessageType type) noexcept {
    return (static_cast<std::uint8_t>(type) & kResponseBit) != 0;
}

[[nodiscard]] bool is_known_type(std::uint8_t raw) noexcept;

/// Human-readable name, for logs and test failure messages.
[[nodiscard]] const char* to_string(MessageType type) noexcept;

enum class StatusCode : std::uint8_t {
    Ok = 0,
    NotFound = 1,
    InvalidRequest = 2,
    Internal = 3,
};

[[nodiscard]] const char* to_string(StatusCode status) noexcept;

/// Why the decoder rejected a buffer. Distinct values rather than a bool
/// because the daemon's reaction differs: a bad key length is answered with an
/// error response and the connection continues, whereas bad magic means
/// framing is lost and the connection has to be closed.
enum class DecodeError : std::uint8_t {
    None = 0,
    Truncated,           ///< Ran out of bytes mid-field.
    BadMagic,            ///< Not an lrd frame, or the stream desynchronised.
    UnsupportedVersion,  ///< A protocol version this build does not speak.
    ReservedFlags,       ///< Non-zero flags: a future extension we cannot honour.
    UnknownType,         ///< Type byte not in the table.
    WrongDirection,      ///< A response arrived where a request was expected.
    TrailingBytes,       ///< Payload longer than the type accounts for.
    FieldTooLarge,       ///< Key or value beyond its documented cap.
};

[[nodiscard]] const char* to_string(DecodeError error) noexcept;

/// Fixed part of every frame.
struct Header {
    std::uint8_t version = kVersion;
    MessageType type = MessageType::GetRequest;
    std::uint16_t flags = 0;
    std::uint64_t request_id = 0;
};

/// Counters returned by StatsRequest.
///
/// Step 3 added the last three. Appending fields to a message is a *breaking*
/// change under this protocol: the decoder rejects trailing bytes, so a step 2
/// client talking to a step 3 daemon gets DecodeError::TrailingBytes rather
/// than silently misreading the reply. That strictness is the intended
/// behaviour - it turns version skew into a loud, immediate failure instead of
/// plausible-looking wrong numbers. Nothing is deployed, so v1 is edited in
/// place; a shipped protocol would have bumped kVersion here.
struct Stats {
    std::uint64_t requests = 0;
    std::uint64_t gets = 0;
    std::uint64_t puts = 0;
    std::uint64_t deletes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;  ///< Entries dropped by the LRU policy.
    std::uint64_t entries = 0;    ///< Current occupancy.
    std::uint64_t capacity = 0;   ///< Maximum occupancy.
};

/// One request in decoded form.
///
/// A tagged struct rather than std::variant<GetRequest, PutRequest, ...>.
/// The variant version is more type-safe - it makes "a GetRequest with a value
/// set" unrepresentable - and it is what I would reach for if the message set
/// were larger or the payloads more varied. With four request types that share
/// two fields, std::visit and the overload-set boilerplate would cost more
/// clarity than the extra safety buys, and the codec is the only code that
/// touches these fields. Worth revisiting in Phase 2 when item metadata and
/// signal payloads arrive.
struct Request {
    MessageType type = MessageType::GetRequest;
    std::uint64_t request_id = 0;
    std::string key;    ///< Get, Put, Delete.
    std::string value;  ///< Put only.
};

struct Response {
    MessageType type = MessageType::GetResponse;
    std::uint64_t request_id = 0;
    StatusCode status = StatusCode::Ok;
    std::string value;    ///< GetResponse payload, or ErrorResponse message.
    Stats stats;          ///< StatsResponse only.
};

/// Convenience builders, so handlers read as intent rather than field wiring.
[[nodiscard]] Response make_error(std::uint64_t request_id, StatusCode status,
                                  std::string message);

}  // namespace lrd::proto
