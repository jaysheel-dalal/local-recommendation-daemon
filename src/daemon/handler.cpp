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

    // get() is not const: a hit reorders the recency list. That is the whole
    // reason a shared_mutex cannot simply be dropped over this cache in step 5 -
    // see docs/concurrency.md.
    const auto value = cache_.get(request.key);
    if (!value) {
        ++stats_.misses;
        response.status = StatusCode::NotFound;
        return response;
    }

    ++stats_.hits;
    response.status = StatusCode::Ok;
    // The shared_ptr keeps the value alive independently of the cache, so this
    // copy could happen after a lock is released rather than while holding it.
    response.value = *value;
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

    // May evict the least-recently-used entry. A PUT that succeeds is therefore
    // not a promise that a later GET will hit - the defining difference between
    // a cache and a store, and something a client has to be written to expect.
    cache_.put(request.key, request.value);
    response.status = StatusCode::Ok;
    return response;
}

Response Handler::handle_delete(const proto::Request& request) {
    ++stats_.deletes;

    Response response;
    response.type = MessageType::DeleteResponse;
    response.request_id = request.request_id;
    response.status = cache_.erase(request.key) ? StatusCode::Ok : StatusCode::NotFound;
    return response;
}

Response Handler::handle_stats(const proto::Request& request) const {
    Response response;
    response.type = MessageType::StatsResponse;
    response.request_id = request.request_id;
    response.status = StatusCode::Ok;
    response.stats = stats_;
    // Cache-owned numbers are read at reporting time rather than mirrored into
    // stats_ on every operation: one source of truth, and no chance of the two
    // drifting apart.
    response.stats.evictions = cache_.metrics().evictions;
    response.stats.entries = cache_.size();
    response.stats.capacity = cache_.capacity();
    return response;
}

}  // namespace lrd::daemon
