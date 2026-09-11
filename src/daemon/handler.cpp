#include "lrd/daemon/handler.hpp"

#include <chrono>
#include <format>
#include <utility>

namespace lrd::daemon {

using proto::ResponseBody;
using proto::StatusCode;

namespace {

constexpr std::memory_order kCount = std::memory_order_relaxed;

rank::Timestamp wall_clock_now() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
}

}  // namespace

Handler::Handler(std::size_t capacity, std::size_t shard_count, rank::ScoringConfig scoring,
                 rank::Timestamp (*clock)())
    : cache_(capacity, shard_count),
      scoring_(std::move(scoring)),
      clock_(clock != nullptr ? clock : &wall_clock_now) {}

proto::Response Handler::handle(const proto::Request& request) {
    requests_.fetch_add(1, kCount);

    proto::Response response;
    response.request_id = request.request_id;

    // std::visit over the request variant. The win over a switch on a type enum
    // is that adding an alternative to RequestBody makes this fail to compile
    // until it is handled, rather than falling into a default case at runtime.
    response.body = std::visit(
        proto::Overloaded{
            [this](const proto::GetItem& get) { return on_get(get); },
            [this](const proto::PutItem& put) { return on_put(put); },
            [this](const proto::DeleteItem& del) { return on_delete(del); },
            [this](const proto::GetStats&) { return on_stats(); },
            [this](const proto::Recommend& recommend) { return on_recommend(recommend); },
        },
        request.body);

    return response;
}

ResponseBody Handler::on_get(const proto::GetItem& request) {
    gets_.fetch_add(1, kCount);

    // get(), not peek(): a direct lookup by id *is* a use, and should keep the
    // item alive in the cache. The scan inside on_recommend is the opposite case
    // and uses a non-mutating traversal.
    const auto item = cache_.get(request.item_id);
    if (!item) {
        misses_.fetch_add(1, kCount);
        return proto::GetItemResult{StatusCode::NotFound, {}};
    }

    hits_.fetch_add(1, kCount);
    // The copy happens with no lock held - the reason the cache hands out a
    // shared_ptr rather than the value.
    return proto::GetItemResult{StatusCode::Ok, *item};
}

ResponseBody Handler::on_put(const proto::PutItem& request) {
    puts_.fetch_add(1, kCount);

    if (request.item.id == 0) {
        // Zero is reserved as "unset", so accepting it would make an unset id
        // indistinguishable from a real one in every later lookup.
        return proto::Failure{StatusCode::InvalidRequest, "item id must not be zero"};
    }
    if (request.item.category.empty()) {
        // Ranking keys affinity off the category; an empty one can never match a
        // signal and would only ever be served at the default affinity.
        return proto::Failure{StatusCode::InvalidRequest, "item category must not be empty"};
    }

    // May evict the least-recently-used entry in this item's shard. A successful
    // PutItem is therefore not a promise that a later GetItem will hit.
    cache_.put(request.item.id, request.item);
    return proto::PutItemResult{StatusCode::Ok};
}

ResponseBody Handler::on_delete(const proto::DeleteItem& request) {
    deletes_.fetch_add(1, kCount);
    return proto::DeleteItemResult{cache_.erase(request.item_id) ? StatusCode::Ok
                                                                 : StatusCode::NotFound};
}

ResponseBody Handler::on_recommend(const proto::Recommend& request) {
    recommends_.fetch_add(1, kCount);

    if (request.count == 0) {
        return proto::Failure{StatusCode::InvalidRequest, "count must be at least 1"};
    }
    if (request.count > proto::kMaxRecommendCount) {
        return proto::Failure{StatusCode::InvalidRequest,
                              std::format("count {} exceeds the limit of {}", request.count,
                                          proto::kMaxRecommendCount)};
    }

    const rank::Timestamp now = clock_();
    rank::CandidateSelector selector(scoring_, request.signal, now, request.count);

    // Shard by shard, so a lock is held only for the duration of one shard's
    // pointer copy - never while scoring. Scoring thousands of candidates under
    // a cache lock would serialise every other worker behind this request.
    //
    // The buffer is declared outside the loop and reused, so the allocation is
    // bounded by the largest shard rather than repeated per shard.
    std::vector<std::pair<rank::ItemId, decltype(cache_)::ValuePtr>> batch;
    for (std::size_t shard = 0; shard < cache_.shard_count(); ++shard) {
        cache_.snapshot_shard(shard, batch);
        for (const auto& [id, item] : batch) {
            selector.consider(*item);
        }
    }

    // dry_run is inert here: step 9 records nothing, so every recommendation is
    // effectively a dry run. Step 10 gives it meaning by having a non-dry-run
    // recommendation reserve exposure against each returned item's cap.
    (void)request.dry_run;

    return proto::RecommendResult{StatusCode::Ok, selector.select()};
}

proto::Stats Handler::stats() const {
    proto::Stats stats;
    stats.requests = requests_.load(kCount);
    stats.gets = gets_.load(kCount);
    stats.puts = puts_.load(kCount);
    stats.deletes = deletes_.load(kCount);
    stats.recommends = recommends_.load(kCount);
    stats.hits = hits_.load(kCount);
    stats.misses = misses_.load(kCount);
    // Cache-owned numbers read at reporting time rather than mirrored into our
    // counters on every operation: one source of truth, no chance of drift.
    stats.evictions = cache_.metrics().evictions;
    stats.entries = cache_.size();
    stats.capacity = cache_.capacity();
    return stats;
}

ResponseBody Handler::on_stats() const {
    return proto::StatsResult{StatusCode::Ok, stats()};
}

}  // namespace lrd::daemon
