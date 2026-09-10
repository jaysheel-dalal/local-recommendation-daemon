#include "lrd/daemon/handler.hpp"

#include <format>
#include <utility>

namespace lrd::daemon {

using proto::MessageType;
using proto::Response;
using proto::StatusCode;

Response Handler::handle(const proto::Request& request) {
    ++stats_.requests;

    switch (request.type) {
        case MessageType::GetRequest:
            return handle_get(request);
        case MessageType::PutRequest:
            return handle_put(request);
        case MessageType::DeleteRequest:
            return handle_delete(request);
        case MessageType::StatsRequest:
            return handle_stats(request);
        default:
            // The codec rejects response types before we get here, so this is
            // unreachable in practice - but a handler that returns a valid
            // response on every path is easier to reason about than one with an
            // implicit "cannot happen".
            return proto::make_error(request.request_id, StatusCode::InvalidRequest,
                                     std::format("unexpected message type {}",
                                                 proto::to_string(request.type)));
    }
}

Response Handler::handle_get(const proto::Request& request) {
    ++stats_.gets;

    Response response;
    response.type = MessageType::GetResponse;
    response.request_id = request.request_id;

    const auto it = store_.find(request.key);
    if (it == store_.end()) {
        ++stats_.misses;
        response.status = StatusCode::NotFound;
        return response;
    }

    ++stats_.hits;
    response.status = StatusCode::Ok;
    response.value = it->second;
    return response;
}

Response Handler::handle_put(const proto::Request& request) {
    ++stats_.puts;

    Response response;
    response.type = MessageType::PutResponse;
    response.request_id = request.request_id;

    if (request.key.empty()) {
        // An empty key is rejected rather than accepted as a valid entry: it is
        // almost always a client bug, and allowing it makes cache dumps
        // ambiguous to read.
        return proto::make_error(request.request_id, StatusCode::InvalidRequest,
                                 "key must not be empty");
    }

    // insert_or_assign rather than operator[] followed by assignment: it avoids
    // default-constructing a std::string that is immediately overwritten, and
    // it says at the call site that overwriting is intended rather than
    // accidental.
    store_.insert_or_assign(request.key, request.value);
    response.status = StatusCode::Ok;
    return response;
}

Response Handler::handle_delete(const proto::Request& request) {
    ++stats_.deletes;

    Response response;
    response.type = MessageType::DeleteResponse;
    response.request_id = request.request_id;
    response.status = (store_.erase(request.key) > 0) ? StatusCode::Ok : StatusCode::NotFound;
    return response;
}

Response Handler::handle_stats(const proto::Request& request) const {
    Response response;
    response.type = MessageType::StatsResponse;
    response.request_id = request.request_id;
    response.status = StatusCode::Ok;
    response.stats = stats_;
    return response;
}

}  // namespace lrd::daemon
