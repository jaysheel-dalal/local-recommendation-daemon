#pragma once

#include "lrd/rank/item.hpp"
#include "lrd/rank/signal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace lrd::bench {

/// Key selection for a cache benchmark.
///
/// A uniform distribution over the key space is the easy choice and a poor
/// model: it produces either a ~100% hit rate (key space fits in the cache) or
/// a ~0% one (it does not), and neither resembles a real workload. Worse for
/// our purposes, uniform access spreads contention perfectly evenly, which
/// flatters any locking scheme.
///
/// Zipf is what cache and key-value benchmarks actually use (YCSB's default,
/// among others): a small number of keys take most of the traffic. That gives a
/// realistic hit rate at a cache much smaller than the key space, and - the
/// reason it matters here - it concentrates access on a few keys, which is the
/// condition under which lock contention and per-shard imbalance actually show
/// up.
///
/// theta = 0 degenerates to uniform, which is kept as a comparison point.
class KeyDistribution {
public:
    KeyDistribution(std::size_t key_count, double theta)
        : key_count_(key_count), theta_(theta), uniform_(0, key_count - 1) {
        if (theta_ <= 0.0) {
            return;  // uniform; no CDF needed
        }

        // Precompute the cumulative distribution once, then sample it with a
        // binary search. Building it costs O(n) at startup and makes every
        // draw O(log n) with no floating-point work in the hot loop.
        cdf_.resize(key_count);
        double total = 0.0;
        for (std::size_t i = 0; i < key_count; ++i) {
            total += 1.0 / std::pow(static_cast<double>(i + 1), theta_);
            cdf_[i] = total;
        }
        for (double& value : cdf_) {
            value /= total;
        }
    }

    [[nodiscard]] std::size_t next(std::mt19937_64& rng) {
        if (theta_ <= 0.0) {
            return uniform_(rng);
        }
        const double sample = real_(rng);
        const auto it = std::lower_bound(cdf_.begin(), cdf_.end(), sample);
        const auto index = static_cast<std::size_t>(std::distance(cdf_.begin(), it));
        return std::min(index, key_count_ - 1);
    }

    [[nodiscard]] std::size_t key_count() const noexcept { return key_count_; }

private:
    std::size_t key_count_;
    double theta_;
    std::uniform_int_distribution<std::size_t> uniform_;
    std::uniform_real_distribution<double> real_{0.0, 1.0};
    std::vector<double> cdf_;
};

/// Item ids are 1-based: zero is reserved as "unset" and the daemon rejects it.
[[nodiscard]] inline lrd::rank::ItemId key_at(std::size_t index) noexcept {
    return static_cast<lrd::rank::ItemId>(index + 1);
}

/// Builds the item set the benchmark stores and ranks.
///
/// Precomputed rather than generated per request: building a category and an
/// advertiser string on the hot path would put two allocations inside the timed
/// region, which for a request costing a few microseconds is not negligible.
///
/// Ages are spread deterministically over ten days so the recency term has
/// something to do, and base scores over (0, 1] so ranking has something to
/// order by. Fixed rather than random, so a run is reproducible.
[[nodiscard]] inline std::vector<lrd::rank::Item> make_items(std::size_t count,
                                                             lrd::rank::Timestamp now) {
    static constexpr const char* kCategories[] = {"tech", "sport", "travel",
                                                  "food", "music", "gaming"};
    static constexpr const char* kAdvertisers[] = {"acme", "globex", "initech", "umbrella"};
    constexpr std::size_t kCategoryCount = sizeof(kCategories) / sizeof(kCategories[0]);
    constexpr std::size_t kAdvertiserCount = sizeof(kAdvertisers) / sizeof(kAdvertisers[0]);

    std::vector<lrd::rank::Item> items;
    items.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        lrd::rank::Item item;
        item.id = key_at(i);
        item.category = kCategories[i % kCategoryCount];
        item.advertiser = kAdvertisers[i % kAdvertiserCount];
        item.base_score = 0.05 + 0.95 * static_cast<double>(i % 20) / 19.0;
        item.created_at = now - std::chrono::hours{static_cast<int>(i % 240)};
        item.expires_at = lrd::rank::from_epoch_millis(0);
        items.push_back(std::move(item));
    }
    return items;
}

/// A signal with a few category affinities, matching the shape a real client
/// would send.
[[nodiscard]] inline lrd::rank::UserSignal make_signal() {
    lrd::rank::UserSignal signal;
    signal.affinities = {{"tech", 0.9}, {"sport", 0.5}, {"travel", 0.2}};
    return signal;
}

[[nodiscard]] inline lrd::rank::Timestamp bench_now() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
}

}  // namespace lrd::bench
