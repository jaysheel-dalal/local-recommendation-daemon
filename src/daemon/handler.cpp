#include "lrd/daemon/handler.hpp"

#include <format>
#include <utility>

namespace lrd::daemon {

using proto::MessageType;
using proto::Response;
using proto::StatusCode;

namespace {
// Pure counters: nothing reads them to decide anything, so there is no
// happens-before relationship to establish and no reason to pay for one.
constexpr std::memory_order kCount = std::memory_order_relaxed;
}  // namespace

Response Handler::handle(const proto::Request& request) {
    requests_.fetch_add(1, kCount);

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
    gets_.fetch_add(1, kCount);

    Response response;
    response.type = MessageType::GetResponse;
    response.request_id = request.request_id;

    // The cache's mutex is taken and released entirely within this call.
    const auto value = cache_.get(request.key);
    if (!value) {
        misses_.fetch_add(1, kCount);
        response.status = StatusCode::NotFound;
        return response;
    }

    hits_.fetch_add(1, kCount);
    response.status = StatusCode::Ok;
    // This copy happens with no lock held. That is precisely why the cache
    // hands out a shared_ptr instead of the value: copying a 400 KB payload
    // inside the critical section would stall every other worker behind it.
    response.value = *value;
    return response;
}

Response Handler::handle_put(const proto::Request& request) {
    puts_.fetch_add(1, kCount);

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
    deletes_.fetch_add(1, kCount);

    Response response;
    response.type = MessageType::DeleteResponse;
    response.request_id = request.request_id;
    response.status = cache_.erase(request.key) ? StatusCode::Ok : StatusCode::NotFound;
    return response;
}

proto::Stats Handler::stats() const {
    proto::Stats stats;
    stats.requests = requests_.load(kCount);
    stats.gets = gets_.load(kCount);
    stats.puts = puts_.load(kCount);
    stats.deletes = deletes_.load(kCount);
    stats.hits = hits_.load(kCount);
    stats.misses = misses_.load(kCount);
    // Cache-owned numbers are read at reporting time rather than mirrored into
    // our counters on every operation: one source of truth, and no chance of
    // the two drifting apart.
    stats.evictions = cache_.metrics().evictions;
    stats.entries = cache_.size();
    stats.capacity = cache_.capacity();
    return stats;
}

Response Handler::handle_stats(const proto::Request& request) const {
    Response response;
    response.type = MessageType::StatsResponse;
    response.request_id = request.request_id;
    response.status = StatusCode::Ok;
    response.stats = stats();
    return response;
}

}  // namespace lrd::daemon
