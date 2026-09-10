#pragma once

#include "lrd/cache/locked_cache.hpp"
#include "lrd/proto/message.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lrd::daemon {

/// Turns a decoded request into a response.
///
/// Deliberately knows nothing about sockets, frames or byte order: it takes a
/// Request and returns a Response. That is what makes it testable without a
/// socket, and it is the seam step 3 cut along - the std::unordered_map became
/// an LruCache without changing anything else, and in step 5 that became a
/// LockedCache the same way.
///
/// ## Thread safety
///
/// `handle()` is safe to call concurrently from any number of threads. One
/// Handler is shared by every worker, because the whole point is that a PUT on
/// one connection is visible to a GET on another.
///
/// Two different mechanisms, for two different reasons:
///
///   * The cache is guarded by a mutex inside LockedCache, because its
///     operations are compound - find, splice, maybe evict - and have to happen
///     as a unit.
///   * The counters are individual atomics, because each is a standalone
///     increment with nothing to keep consistent against anything else. Putting
///     them under the cache's mutex would lengthen its critical section for no
///     benefit; giving them their own mutex would add a second point of
///     contention on the hot path.
///
/// The tradeoff of that second choice is stated plainly: a STATS response is
/// not a consistent snapshot. Counters are read one at a time while other
/// threads keep working, so `hits + misses` may not exactly equal `gets` for a
/// reading taken under load. For reporting that is fine, and the alternative -
/// a lock spanning all six counters plus the cache - would cost real throughput
/// to make a diagnostic prettier.
class Handler {
public:
    /// `capacity` is the maximum number of entries the cache holds before LRU
    /// eviction begins.
    explicit Handler(std::size_t capacity) : cache_(capacity) {}

    [[nodiscard]] proto::Response handle(const proto::Request& request);

    /// A snapshot of the counters. See the note above on consistency.
    [[nodiscard]] proto::Stats stats() const;

private:
    [[nodiscard]] proto::Response handle_get(const proto::Request& request);
    [[nodiscard]] proto::Response handle_put(const proto::Request& request);
    [[nodiscard]] proto::Response handle_delete(const proto::Request& request);
    [[nodiscard]] proto::Response handle_stats(const proto::Request& request) const;

    cache::LockedCache<std::string, std::string> cache_;

    // relaxed ordering throughout: these are pure counters. Nothing else reads
    // them to decide anything, so there is no happens-before relationship to
    // establish and no reason to pay for one.
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> gets_{0};
    std::atomic<std::uint64_t> puts_{0};
    std::atomic<std::uint64_t> deletes_{0};
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
};

}  // namespace lrd::daemon
