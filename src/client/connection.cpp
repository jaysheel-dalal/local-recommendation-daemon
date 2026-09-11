#include "lrd/client/connection.hpp"

#include "lrd/proto/framing.hpp"

#include <format>
#include <stdexcept>
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

Connection::Connection(net::UnixStream stream, std::string_view codec_name)
    : stream_(std::move(stream)), codec_(proto::make_codec(codec_name)) {
    if (!codec_) {
        throw std::invalid_argument(std::format("unknown codec '{}'; available: {}", codec_name,
                                                proto::available_codecs()));
    }
}

Connection Connection::connect(std::string_view socket_path, std::string_view codec_name) {
    return Connection(net::UnixStream::connect(socket_path), codec_name);
}

CallStatus Connection::fail(CallStatus status, std::string message) {
    last_error_ = std::move(message);
    return status;
}

CallStatus Connection::call(const proto::Request& request, proto::Response& response) {
    last_error_.clear();

    const proto::FrameResult written =
        proto::write_message(stream_, *codec_, request, write_buffer_);
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

    if (const proto::DecodeError error = codec_->decode(read_buffer_, response);
        error != proto::DecodeError::None) {
        stream_.close();
        return fail(CallStatus::ProtocolError,
                    std::format("decode failed: {}", proto::to_string(error)));
    }

    // What request_id is for. With one request in flight it is a consistency
    // check; it becomes load bearing the moment responses may arrive out of
    // order, and a mismatch means a desynchronised stream.
    if (response.request_id != request.request_id) {
        stream_.close();
        return fail(CallStatus::ProtocolError,
                    std::format("response id {} does not match request id {}",
                                response.request_id, request.request_id));
    }

    if (const auto* failure = std::get_if<proto::Failure>(&response.body); failure != nullptr) {
        const CallStatus status = (failure->status == proto::StatusCode::InvalidRequest)
                                      ? CallStatus::InvalidRequest
                                      : CallStatus::ServerError;
        return fail(status, failure->message);
    }

    return CallStatus::Ok;
}

template <typename Expected>
const Expected* Connection::expect(const proto::Response& response, CallStatus& status) {
    const Expected* result = std::get_if<Expected>(&response.body);
    if (result == nullptr) {
        status = fail(CallStatus::ProtocolError,
                      std::format("unexpected reply type {}",
                                  proto::to_string(proto::type_of(response.body))));
        return nullptr;
    }
    return result;
}

CallStatus Connection::put_item(const rank::Item& item) {
    // Checked client-side as well as server-side. The daemon enforces these
    // regardless - it cannot trust us - but failing here saves a round trip and
    // names the caller's own mistake.
    if (item.category.size() > proto::kMaxCategoryLength) {
        return fail(CallStatus::InvalidRequest,
                    std::format("category is {} bytes, limit is {}", item.category.size(),
                                proto::kMaxCategoryLength));
    }
    if (item.advertiser.size() > proto::kMaxAdvertiserLength) {
        return fail(CallStatus::InvalidRequest,
                    std::format("advertiser is {} bytes, limit is {}", item.advertiser.size(),
                                proto::kMaxAdvertiserLength));
    }

    proto::Request request;
    request.request_id = next_request_id_++;
    request.body = proto::PutItem{item};

    proto::Response response;
    if (const CallStatus status = call(request, response); status != CallStatus::Ok) {
        return status;
    }
    CallStatus status = CallStatus::Ok;
    if (expect<proto::PutItemResult>(response, status) == nullptr) {
        return status;
    }
    return CallStatus::Ok;
}

CallStatus Connection::get_item(rank::ItemId id, rank::Item& item) {
    proto::Request request;
    request.request_id = next_request_id_++;
    request.body = proto::GetItem{id};

    proto::Response response;
    if (const CallStatus status = call(request, response); status != CallStatus::Ok) {
        return status;
    }

    CallStatus status = CallStatus::Ok;
    const auto* result = expect<proto::GetItemResult>(response, status);
    if (result == nullptr) {
        return status;
    }
    if (result->status == proto::StatusCode::NotFound) {
        return CallStatus::NotFound;
    }

    item = result->item;
    return CallStatus::Ok;
}

CallStatus Connection::delete_item(rank::ItemId id) {
    proto::Request request;
    request.request_id = next_request_id_++;
    request.body = proto::DeleteItem{id};

    proto::Response response;
    if (const CallStatus status = call(request, response); status != CallStatus::Ok) {
        return status;
    }

    CallStatus status = CallStatus::Ok;
    const auto* result = expect<proto::DeleteItemResult>(response, status);
    if (result == nullptr) {
        return status;
    }
    return result->status == proto::StatusCode::NotFound ? CallStatus::NotFound : CallStatus::Ok;
}

CallStatus Connection::recommend(const rank::UserSignal& signal, std::uint32_t count,
                                 std::vector<rank::RankedItem>& items, bool dry_run) {
    if (count == 0 || count > proto::kMaxRecommendCount) {
        return fail(CallStatus::InvalidRequest,
                    std::format("count must be between 1 and {}", proto::kMaxRecommendCount));
    }

    proto::Request request;
    request.request_id = next_request_id_++;
    request.body = proto::Recommend{signal, count, dry_run};

    proto::Response response;
    if (const CallStatus status = call(request, response); status != CallStatus::Ok) {
        return status;
    }

    CallStatus status = CallStatus::Ok;
    const auto* result = expect<proto::RecommendResult>(response, status);
    if (result == nullptr) {
        return status;
    }

    items = result->items;
    return CallStatus::Ok;
}

CallStatus Connection::fetch_stats(proto::Stats& stats) {
    proto::Request request;
    request.request_id = next_request_id_++;
    request.body = proto::GetStats{};

    proto::Response response;
    if (const CallStatus status = call(request, response); status != CallStatus::Ok) {
        return status;
    }

    CallStatus status = CallStatus::Ok;
    const auto* result = expect<proto::StatsResult>(response, status);
    if (result == nullptr) {
        return status;
    }

    stats = result->stats;
    return CallStatus::Ok;
}

}  // namespace lrd::client
