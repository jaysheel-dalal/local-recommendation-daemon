#include "lrd/proto/message.hpp"

#include <utility>

namespace lrd::proto {

bool is_known_type(std::uint8_t raw) noexcept {
    switch (static_cast<MessageType>(raw)) {
        case MessageType::GetRequest:
        case MessageType::PutRequest:
        case MessageType::DeleteRequest:
        case MessageType::StatsRequest:
        case MessageType::GetResponse:
        case MessageType::PutResponse:
        case MessageType::DeleteResponse:
        case MessageType::StatsResponse:
        case MessageType::ErrorResponse:
            return true;
    }
    // Reached for any byte not listed above. Note there is no `default:` label:
    // that way adding a value to the enum makes the compiler warn here
    // (-Wswitch) instead of silently treating the new type as unknown.
    return false;
}

const char* to_string(MessageType type) noexcept {
    switch (type) {
        case MessageType::GetRequest: return "GetRequest";
        case MessageType::PutRequest: return "PutRequest";
        case MessageType::DeleteRequest: return "DeleteRequest";
        case MessageType::StatsRequest: return "StatsRequest";
        case MessageType::GetResponse: return "GetResponse";
        case MessageType::PutResponse: return "PutResponse";
        case MessageType::DeleteResponse: return "DeleteResponse";
        case MessageType::StatsResponse: return "StatsResponse";
        case MessageType::ErrorResponse: return "ErrorResponse";
    }
    return "Unknown";
}

const char* to_string(StatusCode status) noexcept {
    switch (status) {
        case StatusCode::Ok: return "Ok";
        case StatusCode::NotFound: return "NotFound";
        case StatusCode::InvalidRequest: return "InvalidRequest";
        case StatusCode::Internal: return "Internal";
    }
    return "Unknown";
}

const char* to_string(DecodeError error) noexcept {
    switch (error) {
        case DecodeError::None: return "None";
        case DecodeError::Truncated: return "Truncated";
        case DecodeError::BadMagic: return "BadMagic";
        case DecodeError::UnsupportedVersion: return "UnsupportedVersion";
        case DecodeError::ReservedFlags: return "ReservedFlags";
        case DecodeError::UnknownType: return "UnknownType";
        case DecodeError::WrongDirection: return "WrongDirection";
        case DecodeError::TrailingBytes: return "TrailingBytes";
        case DecodeError::FieldTooLarge: return "FieldTooLarge";
    }
    return "Unknown";
}

Response make_error(std::uint64_t request_id, StatusCode status, std::string message) {
    Response response;
    response.type = MessageType::ErrorResponse;
    response.request_id = request_id;
    response.status = status;
    response.value = std::move(message);
    return response;
}

}  // namespace lrd::proto
