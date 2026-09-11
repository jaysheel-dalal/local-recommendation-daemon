#pragma once

#include "lrd/cache/locked_cache.hpp"
#include "lrd/cache/lru_cache.hpp"
#include "lrd/common/hash_mix.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lrd::cache {

/// Cache line size, for keeping each shard's mutex on its own line.
///
/// Not std::hardware_destructive_interference_size: GCC warns that its value is
/// part of the ABI and may change between versions, so using it in a header
/// shared across translation units is a portability trap rather than a
/// convenience.
///
/// 64 is correct for x86-64 and for most ARM64. Worth knowing for where this
/// project is aimed: **Apple Silicon uses 128-byte cache lines**, so a build
/// targeting an M-series machine should raise this. Getting it too small
/// reintroduces the false sharing described below; too large only wastes a
/// little memory.
inline constexpr std::size_t kCacheLineSize = 64;

/// An LRU cache split into independently locked shards.
///
/// ## The idea
///
/// A key is assigned to a shard by its hash, and each shard has its own mutex.
/// Two threads touching keys in different shards never contend, so the
/// serialisation that makes a single-mutex cache lose throughput as threads are
/// added mostly goes away.
///
/// ## Shard count is a power of two, and that is deliberate
///
/// It makes shard selection `hash & (count - 1)` instead of `hash % count`.
/// Integer division is on the order of 20-40 cycles and cannot be pipelined
/// well; a mask is one cycle. On an operation whose whole critical section is a
/// few hundred nanoseconds, that is not nothing - and there is no downside,
/// because a power-of-two shard count is a perfectly good shard count.
///
/// ## The hash must be mixed first
///
/// This is the trap. `std::hash<int>` in libstdc++ **is the identity function**:
/// `std::hash<int>{}(8) == 8`. Masking the low bits of an identity hash with 8
/// shards would put every multiple of 8 in shard 0. Sequential integer keys -
/// entirely normal - would pile into a handful of shards and the sharding would
/// achieve nothing while looking like it should work.
///
/// So the hash goes through a finalizer (splitmix64's) that avalanches high
/// bits down into low ones before masking. std::hash<std::string> in libstdc++
/// is already well distributed and does not need this, but the cache is a
/// template and cannot assume its key type.
///
/// ## What sharding costs
///
/// * **Eviction becomes per-shard.** Capacity is divided evenly, so "least
///   recently used" now means least recently used *within its shard*. A shard
///   holding several hot keys evicts entries a global LRU would have kept. With
///   a skewed key distribution this is measurable - see docs/benchmarks.md,
///   where the hit rate is reported alongside throughput for exactly this
///   reason.
/// * **Global operations lose their consistency.** `size()` and `metrics()` sum
///   across shards while other threads are working, so they are approximations.
///   Making them exact would mean holding every lock at once, which is a
///   pessimisation to make a diagnostic prettier.
/// * **Memory overhead per shard**, and less locality within one.
template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class ShardedCache {
public:
    using Underlying = LockedCache<Key, Value, Hash, KeyEqual>;
    using ValuePtr = typename Underlying::ValuePtr;

    /// `capacity` is the total across all shards; `shard_count` must be a power
    /// of two.
    ShardedCache(std::size_t capacity, std::size_t shard_count)
        : capacity_(capacity), shard_count_(shard_count), mask_(shard_count - 1) {
        if (shard_count == 0 || !std::has_single_bit(shard_count)) {
            throw std::invalid_argument("ShardedCache shard count must be a power of two");
        }
        if (capacity < shard_count) {
            // Otherwise per-shard capacity rounds to zero and LruCache rejects
            // it - with an error naming the wrong thing. Fail here, where the
            // actual mistake is.
            throw std::invalid_argument("ShardedCache capacity must be at least the shard count");
        }

        const std::size_t per_shard = capacity / shard_count;
        shards_.reserve(shard_count);
        for (std::size_t i = 0; i < shard_count; ++i) {
            shards_.push_back(std::make_unique<Shard>(per_shard));
        }
    }

    ShardedCache(const ShardedCache&) = delete;
    ShardedCache& operator=(const ShardedCache&) = delete;

    [[nodiscard]] ValuePtr get(const Key& key) { return shard_for(key).get(key); }
    [[nodiscard]] ValuePtr peek(const Key& key) const { return shard_for(key).peek(key); }

    void put(const Key& key, Value value) { shard_for(key).put(key, std::move(value)); }

    bool erase(const Key& key) { return shard_for(key).erase(key); }

    void clear() {
        for (auto& shard : shards_) {
            shard->cache.clear();
        }
    }

    /// Sum across shards. Approximate under concurrency: shards are read one at
    /// a time while other threads keep working.
    [[nodiscard]] std::size_t size() const {
        std::size_t total = 0;
        for (const auto& shard : shards_) {
            total += shard->cache.size();
        }
        return total;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t shard_count() const noexcept { return shard_count_; }

    /// Summed metrics. Approximate, for the same reason as size().
    [[nodiscard]] CacheMetrics metrics() const {
        CacheMetrics total;
        for (const auto& shard : shards_) {
            const CacheMetrics m = shard->cache.metrics();
            total.hits += m.hits;
            total.misses += m.misses;
            total.insertions += m.insertions;
            total.updates += m.updates;
            total.evictions += m.evictions;
        }
        return total;
    }

    /// Copies one shard's entries into `out` (cleared first).
    ///
    /// Deliberately per-shard rather than a whole-cache snapshot. A single
    /// snapshot would hold one lock at a time anyway but would allocate a vector
    /// the size of the entire cache; iterating shard by shard bounds that by the
    /// largest shard and lets the caller process each batch before taking the
    /// next lock.
    ///
    /// The consequence, worth stating: this is **not** a consistent view. Shards
    /// are read at different instants, so an item moved between shards - which
    /// cannot happen, since the shard is a function of the key - or written
    /// during the scan may be seen or missed. For ranking that is fine: a
    /// candidate set a few milliseconds stale is not a correctness problem, and
    /// the alternative is holding every lock at once.
    void snapshot_shard(std::size_t index, std::vector<std::pair<Key, ValuePtr>>& out) const {
        shards_[index]->cache.snapshot(out);
    }

    /// Entry count per shard. Exposed for tests and diagnostics: an unbalanced
    /// distribution is the failure mode that makes sharding useless, and it is
    /// invisible in any aggregate number.
    [[nodiscard]] std::vector<std::size_t> shard_sizes() const {
        std::vector<std::size_t> sizes;
        sizes.reserve(shards_.size());
        for (const auto& shard : shards_) {
            sizes.push_back(shard->cache.size());
        }
        return sizes;
    }

    [[nodiscard]] std::size_t shard_index(const Key& key) const {
        return hash_mix(hasher_(key)) & mask_;
    }

private:
    /// alignas keeps each shard - and therefore each shard's mutex - on its own
    /// cache line.
    ///
    /// Without it, two mutexes can share a line. Locking is a read-modify-write
    /// on the mutex word, which invalidates the whole line in every other
    /// core's cache; two threads on *different* shards would then bounce that
    /// line between them on every operation. The sharding would look correct
    /// and deliver a fraction of its benefit, for reasons invisible in the
    /// source. This is false sharing, and it is the standard way a sharded
    /// structure underperforms.
    ///
    /// Honest note: each shard here is a whole LockedCache (well over a cache
    /// line already) and they are separately heap-allocated, so the alignment is
    /// insurance rather than a measured win in this particular layout. It
    /// becomes load-bearing the moment the per-shard state shrinks or the
    /// shards move into one contiguous array.
    struct alignas(kCacheLineSize) Shard {
        explicit Shard(std::size_t capacity) : cache(capacity) {}
        Underlying cache;
    };

    [[nodiscard]] Underlying& shard_for(const Key& key) {
        return shards_[shard_index(key)]->cache;
    }

    [[nodiscard]] const Underlying& shard_for(const Key& key) const {
        return shards_[shard_index(key)]->cache;
    }

    std::size_t capacity_;
    std::size_t shard_count_;
    std::size_t mask_;
    Hash hasher_;
    std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace lrd::cache
