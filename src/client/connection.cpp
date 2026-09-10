#include "lrd/client/connection.hpp"

#include "lrd/proto/framing.hpp"

#include <format>
#include <utility>

namespace lrd::client {

const char* to_string(CallStatus status) noexcept {
    switch (status) {
        case CallStatus::Ok: return "Ok";
        case CallStatus::NotFound: return "NotFound";
        case CallStatus::InvalidRequest: return "InvalidRequest";
        case CallStatus::ServerError: return "ServerError";
        case CallStatus::ConnectionLost: return "ConnectionLost";
        case CallStatus::ProtocolError: return "ProtocolError";
    }
    return "Unknown";
}

Connection::Connection(net::UnixStream stream) noexcept : stream_(std::move(stream)) {}

Connection Connection::connect(std::string_view socket_path) {
    return Connection(net::UnixStream::connect(socket_path));
}

CallStatus Connection::fail(CallStatus status, std::string message) {
    last_error_ = std::move(message);
    return status;
}

CallStatus Connection::call(const proto::Request& request, proto::Response& response) {
    last_error_.clear();

    const proto::FrameResult written =
        proto::write_message(stream_, codec_, request, write_buffer_);
    if (!written) {
        // A failed write leaves the daemon's view of the stream unknown - it may
        // have received half a frame. The connection cannot be reused, so it is
        // closed rather than left in a state where the next call would silently
        // desynchronise.
        stream_.close();
        return fail(CallStatus::ConnectionLost,
                    std::format("send failed: {}", proto::to_string(written.status)));
    }

    const proto::FrameResult read = proto::read_frame(stream_, read_buffer_);
    if (!read) {
        stream_.close();
        return fail(CallStatus::ConnectionLost,
                    std::format("receive failed: {}", proto::to_string(read.status)));
    }

    if (const proto::DecodeError error = codec_.decode(read_buffer_, response);
        error != proto::DecodeError::None) {
        // The daemon sent something we cannot parse. Same reasoning as the
        // daemon's own framing errors: the stream can no longer be trusted.
        stream_.close();
        return fail(CallStatus::ProtocolError,
                    std::format("decode failed: {}", proto::to_string(error)));
    }

    // This is what request_id is for. With one request in flight it is only a
    // consistency check, but it becomes load bearing the moment responses may
    // arrive out of order - and a mismatch here means a desynchronised stream,
    // which is far better caught than acted upon.
    if (response.request_id != request.request_id) {
        stream_.close();
        return fail(CallStatus::ProtocolError,
                    std::format("response id {} does not match request id {}",
                                response.request_id, request.request_id));
    }

    if (response.type == proto::MessageType::ErrorResponse) {
        const CallStatus status = (response.status == proto::StatusCode::InvalidRequest)
                                      ? CallStatus::InvalidRequest
                                      : CallStatus::ServerError;
        return fail(status, response.value);
    }

    return CallStatus::Ok;
}

CallStatus Connection::get(std::string_view key, std::string& value) {
    proto::Request request;
    request.type = proto::MessageType::GetRequest;
    request.request_id = next_request_id_++;
    request.key = std::string(key);

    proto::Response response;
    if (const CallStatus status = call(request, response); status != CallStatus::Ok) {
        return status;
    }
    if (response.type != proto::MessageType::GetResponse) {
        return fail(CallStatus::ProtocolError,
                    std::format("expected GetResponse, got {}", proto::to_string(response.type)));
    }
    if (response.status == proto::StatusCode::NotFound) {
        return CallStatus::NotFound;
    }

    value = std::move(response.value);
    return CallStatus::Ok;
}

CallStatus Connection::put(std::string_view key, std::string_view value) {
    // Checked client-side as well as server-side. The daemon enforces these
    // limits regardless - it cannot trust us - but failing here saves a round
    // trip and gives a message that names the caller's own mistake.
    if (key.size() > proto::kMaxKeyLength) {
        return fail(CallStatus::InvalidRequest,
                    std::format("key is {} bytes, limit is {}", key.size(), proto::kMaxKeyLength));
    }
    if (value.size() > proto::kMaxValueLength) {
        return fail(CallStatus::InvalidRequest, std::format("value is {} bytes, limit is {}",
                                                            value.size(), proto::kMaxValueLength));
    }

    proto::Request request;
    request.type = proto::MessageType::PutRequest;
    request.request_id = next_request_id_++;
    request.key = std::string(key);
    request.value = std::string(value);

    proto::Response response;
    if (const CallStatus status = call(request, response); status != CallStatus::Ok) {
        return status;
    }
    if (response.type != proto::MessageType::PutResponse) {
        return fail(CallStatus::ProtocolError,
                    std::format("expected PutResponse, got {}", proto::to_string(response.type)));
    }
    return CallStatus::Ok;
}

CallStatus Connection::remove(std::string_view key) {
    proto::Request request;
    request.type = proto::MessageType::DeleteRequest;
    request.request_id = next_request_id_++;
    request.key = std::string(key);

    proto::Response response;
    if (const CallStatus status = call(request, response); status != CallStatus::Ok) {
        return status;
    }
    if (response.type != proto::MessageType::DeleteResponse) {
        return fail(CallStatus::ProtocolError, std::format("expected DeleteResponse, got {}",
                                                           proto::to_string(response.type)));
    }
    return response.status == proto::StatusCode::NotFound ? CallStatus::NotFound : CallStatus::Ok;
}

CallStatus Connection::fetch_stats(proto::Stats& stats) {
    proto::Request request;
    request.type = proto::MessageType::StatsRequest;
    request.request_id = next_request_id_++;

    proto::Response response;
    if (const CallStatus status = call(request, response); status != CallStatus::Ok) {
        return status;
    }
    if (response.type != proto::MessageType::StatsResponse) {
        return fail(CallStatus::ProtocolError,
                    std::format("expected StatsResponse, got {}", proto::to_string(response.type)));
    }

    stats = response.stats;
    return CallStatus::Ok;
}

}  // namespace lrd::client
