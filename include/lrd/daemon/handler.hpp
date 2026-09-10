#pragma once

#include "lrd/proto/message.hpp"

#include <string>
#include <unordered_map>

namespace lrd::daemon {

/// Turns a decoded request into a response.
///
/// Deliberately knows nothing about sockets, frames or byte order: it takes a
/// Request and returns a Response. That is what makes it testable without a
/// socket, and it is the seam step 3 cuts along - the std::unordered_map below
/// becomes an LruCache and nothing else in this class changes shape.
///
/// Not thread-safe, and not yet required to be: step 2's daemon serves one
/// connection at a time. Step 5 adds the locking, and doing it then rather than
/// speculatively now means the lock can be chosen against a measured access
/// pattern instead of a guessed one.
class Handler {
public:
    [[nodiscard]] proto::Response handle(const proto::Request& request);

    [[nodiscard]] const proto::Stats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] proto::Response handle_get(const proto::Request& request);
    [[nodiscard]] proto::Response handle_put(const proto::Request& request);
    [[nodiscard]] proto::Response handle_delete(const proto::Request& request);
    [[nodiscard]] proto::Response handle_stats(const proto::Request& request) const;

    /// Step 3 replaces this with lrd::cache::LruCache. Until then it is an
    /// unbounded map: correct, and honestly labelled as the placeholder it is
    /// rather than pretending to be a cache without an eviction policy.
    std::unordered_map<std::string, std::string> store_;
    proto::Stats stats_;
};

}  // namespace lrd::daemon
