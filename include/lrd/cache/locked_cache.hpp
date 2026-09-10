#pragma once

#include "lrd/cache/lru_cache.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace lrd::cache {

/// A thread-safe LruCache: one std::mutex, held exclusively, for every
/// operation.
///
/// ## Why a plain mutex and not a shared_mutex
///
/// Because `LruCache::get` is not a read. A cache hit splices its entry to the
/// front of the recency list, so two "readers" under a shared lock would be
/// mutating the same linked list concurrently. The full argument, and the three
/// alternatives that would make a reader-writer lock legitimate, are in
/// docs/concurrency.md. The short version: exact LRU and lock-free reads are
/// mutually exclusive, and this step picks exact LRU.
///
/// This is deliberately the simple, obviously-correct design. Step 6 measures
/// how much throughput it costs, and step 7 shards it - with numbers rather
/// than intuition deciding how many shards.
///
/// ## Keeping the critical section small
///
/// Two decisions matter more than the choice of lock:
///
///   * `put` allocates the value's shared_ptr *before* taking the lock, so the
///     copy of the payload happens off the critical path. Only the pointer move
///     and the list surgery happen under the lock.
///   * `get` returns a shared_ptr rather than the value, so the caller's copy of
///     the payload also happens after the lock is released.
///
/// Between them, the time spent holding the mutex is independent of value size.
/// A version that copied a 400 KB value under the lock would serialise every
/// other thread behind that memcpy.
template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class LockedCache {
public:
    using Underlying = LruCache<Key, Value, Hash, KeyEqual>;
    using ValuePtr = typename Underlying::ValuePtr;

    explicit LockedCache(std::size_t capacity) : capacity_(capacity), cache_(capacity) {}

    LockedCache(const LockedCache&) = delete;
    LockedCache& operator=(const LockedCache&) = delete;

    /// Looks up `key`, marking it most-recently-used.
    ///
    /// The returned pointer keeps the value alive independently of the cache,
    /// so it stays valid after the lock is released even if another thread
    /// evicts or overwrites the entry a nanosecond later.
    [[nodiscard]] ValuePtr get(const Key& key) {
        const std::lock_guard<std::mutex> lock(mutex_);
        return cache_.get(key);
    }

    /// Non-mutating lookup; does not affect recency.
    [[nodiscard]] ValuePtr peek(const Key& key) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return cache_.peek(key);
    }

    void put(const Key& key, Value value) {
        // Allocate and copy the payload outside the critical section. This is
        // the reason LruCache exposes put_shared at all: without it, the
        // make_shared below would happen inside the lock and every other thread
        // would wait through it.
        auto shared = std::make_shared<const Value>(std::move(value));

        const std::lock_guard<std::mutex> lock(mutex_);
        cache_.put_shared(key, std::move(shared));
    }

    bool erase(const Key& key) {
        const std::lock_guard<std::mutex> lock(mutex_);
        return cache_.erase(key);
    }

    void clear() {
        const std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

    /// No lock: capacity is fixed at construction and never mutated, so there
    /// is nothing to synchronise. Taking the mutex here would be cargo cult.
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// Returned by value, not by reference. Handing out a reference to state
    /// protected by a mutex lets the caller read it after the lock is gone,
    /// which is the most common way a "thread-safe wrapper" turns out not to be.
    [[nodiscard]] CacheMetrics metrics() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return cache_.metrics();
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    Underlying cache_;
};

}  // namespace lrd::cache
