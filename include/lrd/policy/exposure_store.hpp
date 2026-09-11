#pragma once

#include "lrd/common/hash_mix.hpp"
#include "lrd/policy/policy.hpp"
#include "lrd/rank/item.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lrd::policy {

/// Per-item exposure and frequency counters, sharded for concurrency.
///
/// ## Why this is not per-user, and why that is a feature
///
/// A frequency cap is normally "at most N shows per user per hour", which needs
/// a user identifier - and an identifier is exactly what the signal deliberately
/// does not carry. The tension resolves itself once you notice where this code
/// runs: **the daemon serves one device, so per-device and per-user are the same
/// scope.** The identity is implicit in the process boundary rather than carried
/// in a field, which means the cap works without the daemon ever learning who it
/// is capping.
///
/// ## Why a separate structure and not a field on the cached Item
///
/// Cached values are `shared_ptr<const Item>` - immutable once published, which
/// is what makes it safe for a reader to hold one after the cache lock is
/// released. Counters have to mutate. Putting a mutable counter inside that
/// value would either break the immutability every reader depends on, or need
/// per-item atomics that cannot be copied.
///
/// Keeping them apart also keeps the *locks* apart. The two are never held at
/// once: ranking reads the cache and releases its lock, then reservation takes a
/// policy lock. Sequential, never nested, so there is no lock ordering to get
/// wrong and no deadlock to reason about.
///
/// ## The race this exists to prevent
///
/// "Check the count is below the cap, then increment it" is a textbook
/// check-then-act: two threads can both read cap-1 and both increment, and the
/// cap is exceeded. `reserve()` does both under one shard lock, which is the
/// whole reason it is a single call rather than a `check()` followed by a
/// `record()`.
///
/// ## The frequency window
///
/// A sliding-window *counter*, not a sliding log and not a fixed bucket.
///
///   * A sliding log stores a timestamp per show - exact, and O(shows) memory
///     per item.
///   * A fixed bucket is O(1) but allows up to 2x the limit across a boundary:
///     N shows at 10:59 and N more at 11:01 both pass.
///   * This keeps two counters - the current window and the previous one - and
///     estimates the rolling count by weighting the previous window by how much
///     of it is still in view:
///
///         estimate = previous * (1 - elapsed/window) + current
///
///     O(1) memory, no boundary doubling, and the error is bounded and small.
///     It is the algorithm most production rate limiters use for exactly these
///     reasons.
class ExposureStore {
public:
    /// `shard_count` must be a power of two, for the same masking reason as the
    /// item cache.
    ExposureStore(PolicyConfig config, std::size_t shard_count);

    ExposureStore(const ExposureStore&) = delete;
    ExposureStore& operator=(const ExposureStore&) = delete;

    /// Atomically checks every limit and, if all pass, records one show.
    ///
    /// This is the authoritative gate. Nothing else may increment a counter, and
    /// nothing that has not been through here may be returned to a client.
    [[nodiscard]] Decision reserve(rank::ItemId id, rank::Timestamp now);

    /// Checks the limits without recording anything - the `dry_run` path.
    ///
    /// Its answer is advisory by construction: another thread may reserve the
    /// last remaining slot a nanosecond later. That is inherent to asking
    /// "would this be allowed" without taking it, and is why the live path is a
    /// single atomic call rather than this followed by a record.
    [[nodiscard]] Decision check(rank::ItemId id, rank::Timestamp now) const;

    /// Drops an item's counters.
    ///
    /// Deliberately **not** called when an item is deleted from the cache. If it
    /// were, any client could reset an item's lifetime exposure cap by deleting
    /// and re-publishing it, which makes the cap decorative. Counters therefore
    /// outlive the items they govern, and `max_tracked_items` is what bounds
    /// them. Exposed for tests and for an explicit administrative reset.
    void forget(rank::ItemId id);

    void clear();

    [[nodiscard]] PolicyMetrics metrics() const;
    [[nodiscard]] std::size_t tracked() const;
    [[nodiscard]] const PolicyConfig& config() const noexcept { return config_; }

private:
    /// Counters for one item.
    struct Entry {
        std::uint64_t lifetime_shows = 0;

        /// Which frequency window `current` belongs to, as now/window.
        std::uint64_t window_index = 0;
        std::uint32_t current = 0;
        std::uint32_t previous = 0;
    };

    /// alignas keeps each shard's mutex on its own cache line. Without it two
    /// mutexes can share a line, and threads working on different shards bounce
    /// that line between cores on every lock - false sharing, which makes the
    /// sharding look correct while delivering a fraction of its benefit.
    struct alignas(64) Shard {
        mutable std::mutex mutex;
        std::unordered_map<rank::ItemId, Entry> entries;

        /// Recency order for eviction under fail-open. Only maintained when
        /// fail-open is configured; keeping a list up to date on every reserve
        /// is pure cost when nothing will ever evict.
        std::list<rank::ItemId> recency;
        std::unordered_map<rank::ItemId, std::list<rank::ItemId>::iterator> recency_index;
    };

    [[nodiscard]] Shard& shard_for(rank::ItemId id);
    [[nodiscard]] const Shard& shard_for(rank::ItemId id) const;

    /// Rolls `entry` forward to the window containing `now`, and returns the
    /// estimated rolling count.
    [[nodiscard]] double advance_and_estimate(Entry& entry, std::uint64_t now_ms) const;

    /// Read-only variant for check(), which must not mutate.
    [[nodiscard]] double estimate_without_advancing(const Entry& entry,
                                                    std::uint64_t now_ms) const;

    void touch_recency(Shard& shard, rank::ItemId id);
    bool evict_one(Shard& shard);

    PolicyConfig config_;
    std::size_t shard_count_;
    std::size_t mask_;
    std::size_t per_shard_capacity_;
    std::vector<std::unique_ptr<Shard>> shards_;

    // relaxed: pure counters, nothing reads them to decide anything.
    std::atomic<std::uint64_t> allowed_{0};
    std::atomic<std::uint64_t> exposure_blocked_{0};
    std::atomic<std::uint64_t> frequency_blocked_{0};
    std::atomic<std::uint64_t> store_full_blocked_{0};
    std::atomic<std::uint64_t> evicted_{0};
};

}  // namespace lrd::policy
