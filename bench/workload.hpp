#pragma once

#include <algorithm>
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

/// Keys are precomputed rather than formatted per request. std::to_string plus
/// a concatenation on the hot path would put an allocation and an integer
/// format inside the timed region, which for a request costing a few
/// microseconds is not negligible.
[[nodiscard]] inline std::vector<std::string> make_keys(std::size_t count) {
    std::vector<std::string> keys;
    keys.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        keys.push_back("bench-key-" + std::to_string(i));
    }
    return keys;
}

[[nodiscard]] inline std::string make_value(std::size_t size) {
    std::string value(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        value[i] = static_cast<char>('a' + (i % 26));
    }
    return value;
}

}  // namespace lrd::bench
