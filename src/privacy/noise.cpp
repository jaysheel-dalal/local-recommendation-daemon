#include "lrd/privacy/noise.hpp"

#include "lrd/common/hash_mix.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>

namespace lrd::privacy {

namespace {

/// FNV-1a over the metric name.
///
/// Chosen over std::hash<std::string_view> for one reason that matters here:
/// libstdc++'s string hash is seeded per process in some configurations, which
/// would make the noise differ between daemon restarts within the same epoch.
/// That would quietly reintroduce the averaging attack the epoch memoisation
/// exists to prevent - restart the daemon, get a fresh draw, repeat. FNV-1a is
/// fixed and specified.
constexpr std::uint64_t fnv1a(std::string_view text) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const char c : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

/// Draws from Laplace(0, scale) by inverse transform.
///
/// The Laplace CDF inverts to:
///     x = -b * sgn(u) * ln(1 - 2|u|)   for u in (-0.5, 0.5)
///
/// which is why `u` is clamped away from the open endpoints: at exactly +/-0.5
/// the logarithm's argument is zero and the draw is infinite. A single sample
/// landing on the boundary would publish a garbage counter, and
/// uniform_real_distribution's half-open range makes one endpoint reachable.
double laplace_sample(std::mt19937_64& rng, double scale) {
    std::uniform_real_distribution<double> uniform(-0.5, 0.5);

    // Guarding by clamping rather than by resampling: a resample loop is
    // unbounded in principle, and the clamp's effect is a draw of about
    // b*36 instead of infinity - already far outside anything meaningful.
    constexpr double kGuard = 0.5 - 1e-15;
    const double u = std::clamp(uniform(rng), -kGuard, kGuard);

    const double sign = (u < 0.0) ? -1.0 : 1.0;
    return -scale * sign * std::log(1.0 - 2.0 * std::abs(u));
}

}  // namespace

NoisyCounters::NoisyCounters(PrivacyConfig config) : config_(config) {
    if (config_.enabled) {
        if (config_.epsilon <= 0.0) {
            throw std::invalid_argument("privacy epsilon must be positive");
        }
        if (config_.sensitivity <= 0.0) {
            throw std::invalid_argument("privacy sensitivity must be positive");
        }
        if (config_.epoch.count() <= 0) {
            throw std::invalid_argument("privacy epoch must be positive");
        }
    }
    if (config_.rounding == 0) {
        config_.rounding = 1;
    }
    if (config_.seed == 0) {
        // Drawn once at construction rather than per call, so the whole process
        // shares one seed and the per-epoch determinism holds.
        std::random_device device;
        config_.seed = (static_cast<std::uint64_t>(device()) << 32) ^ device();
        // 0 is the sentinel for "pick one", so a device that happened to produce
        // it would silently re-enter this branch on the next construction.
        if (config_.seed == 0) {
            config_.seed = 0x9E3779B97F4A7C15ULL;
        }
    }
}

double NoisyCounters::scale() const noexcept {
    return config_.sensitivity / config_.epsilon;
}

std::uint64_t NoisyCounters::epoch_index(rank::Timestamp now) const noexcept {
    const auto epoch_ms = static_cast<std::uint64_t>(config_.epoch.count());
    if (epoch_ms == 0) {
        return 0;
    }
    const std::int64_t millis = rank::to_epoch_millis(now);
    // Negative instants are not expected but must not produce a wild index via
    // a signed-to-unsigned conversion.
    const auto unsigned_millis = static_cast<std::uint64_t>(std::max<std::int64_t>(millis, 0));
    return unsigned_millis / epoch_ms;
}

double NoisyCounters::noise_for(std::string_view metric, rank::Timestamp now) const {
    // The whole of the per-epoch memoisation: seed a generator from the metric
    // and the epoch, so the same pair always produces the same draw. No cache,
    // no lock, no eviction, and identical across restarts.
    std::uint64_t state = hash_mix(config_.seed ^ fnv1a(metric));
    state = hash_mix(state ^ epoch_index(now));

    std::mt19937_64 rng(state);
    return laplace_sample(rng, scale());
}

std::uint64_t NoisyCounters::publish(std::string_view metric, std::uint64_t exact,
                                     rank::Timestamp now) const {
    if (!config_.enabled) {
        return exact;
    }

    const double noised = static_cast<double>(exact) + noise_for(metric, now);

    // Everything below this line is post-processing of a differentially private
    // value. That is safe by the post-processing property: no function of a DP
    // output can weaken its guarantee, because the function has no access to the
    // original data. Clamping, rounding and suppression are therefore free.
    const double clamped = std::max(0.0, noised);  // a count cannot be negative

    const auto rounded_value = static_cast<std::uint64_t>(std::llround(clamped));
    const std::uint64_t rounding = config_.rounding;
    const std::uint64_t rounded =
        rounding <= 1 ? rounded_value
                      : ((rounded_value + rounding / 2) / rounding) * rounding;

    // Suppression tests the *noisy* value, never the exact one.
    //
    // This is the subtle part. Suppressing on the exact count would leak exactly
    // what the mechanism is meant to hide: an observer seeing zero would learn
    // the true count was below the threshold, which is a precise statement about
    // the underlying data that no amount of noise elsewhere undoes. Testing the
    // noisy value keeps the whole pipeline post-processing.
    if (rounded <= config_.suppression_threshold) {
        return 0;
    }
    return rounded;
}

}  // namespace lrd::privacy
