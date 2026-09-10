#pragma once

#include "lrd/cache/lru_cache.hpp"
#include "lrd/proto/message.hpp"

#include <cstddef>
#include <string>

namespace lrd::daemon {

/// Turns a decoded request into a response.
///
/// Deliberately knows nothing about sockets, frames or byte order: it takes a
/// Request and returns a Response. That is what makes it testable without a
/// socket, and it is the seam step 3 cuts along - the std::unordered_map below
/// becomes an LruCache and nothing else in this class changes shape.
///
/// Not thread-safe, and not yet required to be: the daemon serves one
/// connection at a time. Step 5 adds the locking, and doing it then rather than
/// speculatively now means the lock can be chosen against a measured access
/// pattern instead of a guessed one.
class Handler {
public:
    /// `capacity` is the maximum number of entries the cache holds before LRU
    /// eviction begins.
    explicit Handler(std::size_t capacity) : cache_(capacity) {}

    [[nodiscard]] proto::Response handle(const proto::Request& request);

    [[nodiscard]] const proto::Stats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] proto::Response handle_get(const proto::Request& request);
    [[nodiscard]] proto::Response handle_put(const proto::Request& request);
    [[nodiscard]] proto::Response handle_delete(const proto::Request& request);
    [[nodiscard]] proto::Response handle_stats(const proto::Request& request) const;

    /// The step 2 placeholder was an unbounded std::unordered_map. Swapping it
    /// for the cache touched only this line and the four handlers below, which
    /// is what the Request-in/Response-out shape was for.
    cache::LruCache<std::string, std::string> cache_;
    proto::Stats stats_;
};

}  // namespace lrd::daemon
