#pragma once

#include "lrd/cache/sharded_cache.hpp"
#include "lrd/proto/message.hpp"
#include "lrd/rank/item.hpp"
#include "lrd/rank/scorer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lrd::daemon {

/// Turns a decoded request into a response.
///
/// Knows nothing about sockets, frames or byte order: Request in, Response out.
/// That is what makes it testable without a socket, and it is the seam every
/// step so far has cut along - the store went from std::unordered_map to
/// LruCache to LockedCache to ShardedCache, and in step 9 from strings to item
/// metadata, without anything above it changing shape.
///
/// ## Thread safety
///
/// `handle()` is safe to call concurrently from any number of threads. One
/// Handler is shared by every worker, because the point is that a PutItem on one
/// connection is visible to a Recommend on another.
///
/// Two mechanisms, for two reasons: the cache is guarded by a mutex per shard
/// because its operations are compound, and the counters are individual atomics
/// because each is a standalone increment with nothing to stay consistent
/// against. The cost of the second choice, stated plainly: a STATS response is
/// not a consistent snapshot.
class Handler {
public:
    /// `capacity` is total entries before LRU eviction; `shard_count` must be a
    /// power of two. `scoring` is the ranking heuristic's configuration.
    ///
    /// `clock` is injected rather than hardcoded so ranking tests can pin "now"
    /// instead of depending on the wall clock - a recency term evaluated against
    /// a moving clock makes every score assertion approximate. nullptr means the
    /// system clock.
    ///
    /// Passed at construction rather than through a setter: Handler owns a
    /// ShardedCache and a set of atomics, so it is neither copyable nor movable,
    /// and a two-step "construct then configure" forces callers into contortions
    /// to return one. A function pointer rather than std::function because the
    /// clock is called once per recommendation and carries no state.
    Handler(std::size_t capacity, std::size_t shard_count, rank::ScoringConfig scoring = {},
            rank::Timestamp (*clock)() = nullptr);

    [[nodiscard]] proto::Response handle(const proto::Request& request);

    [[nodiscard]] proto::Stats stats() const;

private:
    [[nodiscard]] proto::ResponseBody on_get(const proto::GetItem& request);
    [[nodiscard]] proto::ResponseBody on_put(const proto::PutItem& request);
    [[nodiscard]] proto::ResponseBody on_delete(const proto::DeleteItem& request);
    [[nodiscard]] proto::ResponseBody on_stats() const;
    [[nodiscard]] proto::ResponseBody on_recommend(const proto::Recommend& request);

    cache::ShardedCache<rank::ItemId, rank::Item> cache_;
    rank::ScoringConfig scoring_;
    rank::Timestamp (*clock_)() = nullptr;

    // relaxed ordering: pure counters, nothing reads them to decide anything.
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> gets_{0};
    std::atomic<std::uint64_t> puts_{0};
    std::atomic<std::uint64_t> deletes_{0};
    std::atomic<std::uint64_t> recommends_{0};
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
};

}  // namespace lrd::daemon
