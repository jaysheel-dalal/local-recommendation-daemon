#include "lrd/proto/message.hpp"

#include <utility>

namespace lrd::proto {

bool is_known_type(std::uint8_t raw) noexcept {
    switch (static_cast<MessageType>(raw)) {
        case MessageType::GetItemRequest:
        case MessageType::PutItemRequest:
        case MessageType::DeleteItemRequest:
        case MessageType::StatsRequest:
        case MessageType::RecommendRequest:
        case MessageType::GetItemResponse:
        case MessageType::PutItemResponse:
        case MessageType::DeleteItemResponse:
        case MessageType::StatsResponse:
        case MessageType::RecommendResponse:
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
        case MessageType::GetItemRequest: return "GetItemRequest";
        case MessageType::PutItemRequest: return "PutItemRequest";
        case MessageType::DeleteItemRequest: return "DeleteItemRequest";
        case MessageType::StatsRequest: return "StatsRequest";
        case MessageType::RecommendRequest: return "RecommendRequest";
        case MessageType::GetItemResponse: return "GetItemResponse";
        case MessageType::PutItemResponse: return "PutItemResponse";
        case MessageType::DeleteItemResponse: return "DeleteItemResponse";
        case MessageType::StatsResponse: return "StatsResponse";
        case MessageType::RecommendResponse: return "RecommendResponse";
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

// std::visit with an Overloaded set rather than a chain of
// std::holds_alternative checks: adding an alternative to RequestBody makes this
// fail to compile until a handler for it is added, which is the safety the
// variant was chosen for.
MessageType type_of(const RequestBody& body) noexcept {
    return std::visit(Overloaded{
                          [](const GetItem&) { return MessageType::GetItemRequest; },
                          [](const PutItem&) { return MessageType::PutItemRequest; },
                          [](const DeleteItem&) { return MessageType::DeleteItemRequest; },
                          [](const GetStats&) { return MessageType::StatsRequest; },
                          [](const Recommend&) { return MessageType::RecommendRequest; },
                      },
                      body);
}

MessageType type_of(const ResponseBody& body) noexcept {
    return std::visit(Overloaded{
                          [](const GetItemResult&) { return MessageType::GetItemResponse; },
                          [](const PutItemResult&) { return MessageType::PutItemResponse; },
                          [](const DeleteItemResult&) { return MessageType::DeleteItemResponse; },
                          [](const StatsResult&) { return MessageType::StatsResponse; },
                          [](const RecommendResult&) { return MessageType::RecommendResponse; },
                          [](const Failure&) { return MessageType::ErrorResponse; },
                      },
                      body);
}

StatusCode status_of(const ResponseBody& body) noexcept {
    // Every alternative happens to expose `status`, so one generic lambda covers
    // them all. Kept as a single auto lambda rather than six explicit ones: if a
    // future alternative lacks a status field this stops compiling, which is the
    // right moment to decide what its status should be.
    return std::visit([](const auto& result) { return result.status; }, body);
}

Response make_error(std::uint64_t request_id, StatusCode status, std::string message) {
    Response response;
    response.request_id = request_id;
    response.body = Failure{status, std::move(message)};
    return response;
}

}  // namespace lrd::proto
