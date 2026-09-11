#include "lrd/policy/exposure_store.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <utility>

namespace lrd::policy {

namespace {
constexpr std::memory_order kCount = std::memory_order_relaxed;

std::uint64_t to_millis(rank::Timestamp now) noexcept {
    return static_cast<std::uint64_t>(rank::to_epoch_millis(now));
}
}  // namespace

const char* to_string(Decision decision) noexcept {
    switch (decision) {
        case Decision::Allowed: return "Allowed";
        case Decision::ExposureCapReached: return "ExposureCapReached";
        case Decision::FrequencyLimited: return "FrequencyLimited";
        case Decision::StoreFull: return "StoreFull";
    }
    return "Unknown";
}

ExposureStore::ExposureStore(PolicyConfig config, std::size_t shard_count)
    : config_(config), shard_count_(shard_count), mask_(shard_count - 1) {
    if (shard_count == 0 || !std::has_single_bit(shard_count)) {
        throw std::invalid_argument("ExposureStore shard count must be a power of two");
    }
    if (config_.max_tracked_items < shard_count) {
        throw std::invalid_argument(
            "ExposureStore max_tracked_items must be at least the shard count");
    }

    per_shard_capacity_ = config_.max_tracked_items / shard_count;
    shards_.reserve(shard_count);
    for (std::size_t i = 0; i < shard_count; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }
}

ExposureStore::Shard& ExposureStore::shard_for(rank::ItemId id) {
    return *shards_[hash_mix(static_cast<std::size_t>(id)) & mask_];
}

const ExposureStore::Shard& ExposureStore::shard_for(rank::ItemId id) const {
    return *shards_[hash_mix(static_cast<std::size_t>(id)) & mask_];
}

double ExposureStore::estimate_without_advancing(const Entry& entry,
                                                 std::uint64_t now_ms) const {
    const auto window_ms = static_cast<std::uint64_t>(config_.frequency_window.count());
    if (window_ms == 0) {
        return 0.0;
    }

    const std::uint64_t index = now_ms / window_ms;
    std::uint32_t current = entry.current;
    std::uint32_t previous = entry.previous;

    // The same roll-forward advance_and_estimate performs, but computed into
    // locals so this stays const. Duplicated deliberately: making the mutating
    // version call this one would need the rolled-forward values back out, and
    // two four-line functions are clearer than one with an out-parameter.
    if (index == entry.window_index + 1) {
        previous = current;
        current = 0;
    } else if (index != entry.window_index) {
        previous = 0;
        current = 0;
    }

    const double elapsed = static_cast<double>(now_ms % window_ms);
    const double weight = 1.0 - elapsed / static_cast<double>(window_ms);
    return static_cast<double>(previous) * weight + static_cast<double>(current);
}

double ExposureStore::advance_and_estimate(Entry& entry, std::uint64_t now_ms) const {
    const auto window_ms = static_cast<std::uint64_t>(config_.frequency_window.count());
    if (window_ms == 0) {
        return 0.0;
    }

    const std::uint64_t index = now_ms / window_ms;
    if (index == entry.window_index + 1) {
        // Exactly one window has passed: what was current becomes the previous
        // window, and it is that decaying tail which stops a burst at a boundary
        // from being forgotten.
        entry.previous = entry.current;
        entry.current = 0;
        entry.window_index = index;
    } else if (index != entry.window_index) {
        // Two or more windows have passed - or, defensively, the clock moved
        // backwards. Nothing within view survives either way.
        entry.previous = 0;
        entry.current = 0;
        entry.window_index = index;
    }

    const double elapsed = static_cast<double>(now_ms % window_ms);
    const double weight = 1.0 - elapsed / static_cast<double>(window_ms);
    return static_cast<double>(entry.previous) * weight + static_cast<double>(entry.current);
}

void ExposureStore::touch_recency(Shard& shard, rank::ItemId id) {
    if (!config_.fail_open_when_full) {
        return;  // nothing will ever evict, so recency is pure overhead
    }
    if (const auto found = shard.recency_index.find(id); found != shard.recency_index.end()) {
        shard.recency.splice(shard.recency.begin(), shard.recency, found->second);
        return;
    }
    shard.recency.push_front(id);
    shard.recency_index.emplace(id, shard.recency.begin());
}

bool ExposureStore::evict_one(Shard& shard) {
    if (shard.recency.empty()) {
        return false;
    }
    const rank::ItemId victim = shard.recency.back();
    shard.recency.pop_back();
    shard.recency_index.erase(victim);
    shard.entries.erase(victim);
    evicted_.fetch_add(1, kCount);
    return true;
}

Decision ExposureStore::reserve(rank::ItemId id, rank::Timestamp now) {
    const std::uint64_t now_ms = to_millis(now);
    Shard& shard = shard_for(id);

    // One lock, covering the check and the increment together. Splitting them
    // would let two threads both observe cap-1 and both increment past the cap -
    // the check-then-act race this whole class exists to close.
    const std::lock_guard<std::mutex> lock(shard.mutex);

    auto found = shard.entries.find(id);
    if (found == shard.entries.end()) {
        if (shard.entries.size() >= per_shard_capacity_) {
            if (!config_.fail_open_when_full || !evict_one(shard)) {
                // Fail-closed: refuse a new item rather than discard an existing
                // item's counters. Under-showing is recoverable; forgetting a
                // cap and over-showing is not.
                store_full_blocked_.fetch_add(1, kCount);
                return Decision::StoreFull;
            }
        }
        found = shard.entries.emplace(id, Entry{}).first;
        found->second.window_index =
            config_.frequency_window.count() > 0
                ? now_ms / static_cast<std::uint64_t>(config_.frequency_window.count())
                : 0;
    }

    Entry& entry = found->second;

    if (config_.exposure_cap > 0 && entry.lifetime_shows >= config_.exposure_cap) {
        exposure_blocked_.fetch_add(1, kCount);
        return Decision::ExposureCapReached;
    }

    if (config_.frequency_limit > 0) {
        const double estimate = advance_and_estimate(entry, now_ms);
        if (estimate + 1.0 > static_cast<double>(config_.frequency_limit)) {
            // `estimate + 1 > limit` rather than `estimate >= limit`: the
            // question is whether admitting *this* show would exceed the limit,
            // not whether the limit is already met.
            frequency_blocked_.fetch_add(1, kCount);
            return Decision::FrequencyLimited;
        }
        ++entry.current;
    }

    ++entry.lifetime_shows;
    touch_recency(shard, id);
    allowed_.fetch_add(1, kCount);
    return Decision::Allowed;
}

Decision ExposureStore::check(rank::ItemId id, rank::Timestamp now) const {
    const std::uint64_t now_ms = to_millis(now);
    const Shard& shard = shard_for(id);

    const std::lock_guard<std::mutex> lock(shard.mutex);

    const auto found = shard.entries.find(id);
    if (found == shard.entries.end()) {
        // An untracked item has been shown zero times. Whether it *could* be
        // tracked is a separate question - a dry run deliberately does not
        // reserve the slot it would need.
        return shard.entries.size() >= per_shard_capacity_ && !config_.fail_open_when_full
                   ? Decision::StoreFull
                   : Decision::Allowed;
    }

    const Entry& entry = found->second;
    if (config_.exposure_cap > 0 && entry.lifetime_shows >= config_.exposure_cap) {
        return Decision::ExposureCapReached;
    }
    if (config_.frequency_limit > 0) {
        const double estimate = estimate_without_advancing(entry, now_ms);
        if (estimate + 1.0 > static_cast<double>(config_.frequency_limit)) {
            return Decision::FrequencyLimited;
        }
    }
    return Decision::Allowed;
}

void ExposureStore::forget(rank::ItemId id) {
    Shard& shard = shard_for(id);
    const std::lock_guard<std::mutex> lock(shard.mutex);
    shard.entries.erase(id);
    if (const auto found = shard.recency_index.find(id); found != shard.recency_index.end()) {
        shard.recency.erase(found->second);
        shard.recency_index.erase(found);
    }
}

void ExposureStore::clear() {
    for (auto& shard : shards_) {
        const std::lock_guard<std::mutex> lock(shard->mutex);
        shard->entries.clear();
        shard->recency.clear();
        shard->recency_index.clear();
    }
}

std::size_t ExposureStore::tracked() const {
    std::size_t total = 0;
    for (const auto& shard : shards_) {
        const std::lock_guard<std::mutex> lock(shard->mutex);
        total += shard->entries.size();
    }
    return total;
}

PolicyMetrics ExposureStore::metrics() const {
    PolicyMetrics metrics;
    metrics.allowed = allowed_.load(kCount);
    metrics.exposure_blocked = exposure_blocked_.load(kCount);
    metrics.frequency_blocked = frequency_blocked_.load(kCount);
    metrics.store_full_blocked = store_full_blocked_.load(kCount);
    metrics.evicted = evicted_.load(kCount);
    metrics.tracked = tracked();
    return metrics;
}

}  // namespace lrd::policy
