#include "lrd/privacy/noise.hpp"

#include "test_harness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace lrd::privacy;
namespace rank = lrd::rank;

namespace {

const rank::Timestamp kBase = rank::from_epoch_millis(1700000000000LL);
constexpr auto kEpoch = std::chrono::hours{1};

rank::Timestamp at(std::chrono::milliseconds offset) {
    return kBase + offset;
}

/// A base aligned to the start of an epoch.
///
/// Epochs are indexed as now_ms / epoch_ms, so an arbitrary timestamp sits at an
/// arbitrary offset inside one - kBase happens to be about 13 minutes into its
/// hour. A test walking "50 minutes from kBase" therefore crosses a boundary and
/// legitimately sees fresh noise, which looks like a memoisation bug and is not.
/// Aligning first is what makes an offset mean what it reads as.
rank::Timestamp aligned_base() {
    const auto epoch_ms =
        static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(kEpoch).count());
    const std::int64_t base_ms = rank::to_epoch_millis(kBase);
    return rank::from_epoch_millis((base_ms / epoch_ms) * epoch_ms);
}

/// A fixed seed everywhere: the mechanism is randomised, so a test that let the
/// seed vary would be a test that fails occasionally for no reason. The seed is
/// not a secret - see the note in noise.hpp - so pinning it costs nothing.
PrivacyConfig config_of(bool enabled = true, double epsilon = 1.0,
                        std::uint64_t suppression = 5, std::uint64_t rounding = 1) {
    PrivacyConfig config;
    config.enabled = enabled;
    config.epsilon = epsilon;
    config.sensitivity = 1.0;
    config.suppression_threshold = suppression;
    config.rounding = rounding;
    config.epoch = std::chrono::duration_cast<std::chrono::milliseconds>(kEpoch);
    config.seed = 0xABCDEF0123456789ULL;
    return config;
}

double mean(const std::vector<double>& values) {
    double sum = 0.0;
    for (const double value : values) {
        sum += value;
    }
    return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
}

}  // namespace

// --------------------------------------------------------------------------
// Configuration
// --------------------------------------------------------------------------

LRD_TEST("disabled is a true pass-through") {
    // The mechanism has to be switchable off without any other code changing
    // shape, and without rounding or suppression quietly still applying.
    const NoisyCounters counters(config_of(/*enabled=*/false, 1.0, 100, 1000));
    for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{7},
                                      std::uint64_t{123456}}) {
        LRD_CHECK_EQ(counters.publish("gets", value, kBase), value);
    }
}

LRD_TEST("invalid parameters are rejected when enabled") {
    for (const double bad_epsilon : {0.0, -1.0}) {
        PrivacyConfig config = config_of();
        config.epsilon = bad_epsilon;
        bool threw = false;
        try {
            const NoisyCounters counters(config);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        LRD_CHECK(threw);
    }

    PrivacyConfig zero_epoch = config_of();
    zero_epoch.epoch = std::chrono::milliseconds{0};
    bool threw = false;
    try {
        const NoisyCounters counters(zero_epoch);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    LRD_CHECK(threw);
}

LRD_TEST("the scale is sensitivity over epsilon") {
    // The one piece of arithmetic that makes the noise calibrated rather than
    // arbitrary. Getting it inverted would look plausible and be backwards.
    PrivacyConfig config = config_of();
    config.sensitivity = 2.0;
    config.epsilon = 0.5;
    const NoisyCounters counters(config);
    LRD_CHECK(std::fabs(counters.scale() - 4.0) < 1e-12);
}

// --------------------------------------------------------------------------
// The distribution
// --------------------------------------------------------------------------

LRD_TEST("noise is centred on zero") {
    // An unbiased mechanism: averaged over many metrics the noise adds nothing,
    // so aggregate reporting is not systematically inflated or deflated.
    const NoisyCounters counters(config_of());

    std::vector<double> draws;
    draws.reserve(4000);
    for (int i = 0; i < 4000; ++i) {
        draws.push_back(counters.noise_for("metric-" + std::to_string(i), kBase));
    }

    // Laplace(0, 1) has standard deviation sqrt(2), so the standard error over
    // 4000 samples is about 0.022. A bound of 0.15 is roughly seven sigma - wide
    // enough never to flake, tight enough to catch a genuinely skewed generator.
    LRD_CHECK(std::fabs(mean(draws)) < 0.15);
}

LRD_TEST("smaller epsilon produces visibly more noise") {
    // The parameter has to actually do something, in the right direction.
    const NoisyCounters loose(config_of(true, /*epsilon=*/10.0));
    const NoisyCounters tight(config_of(true, /*epsilon=*/0.1));

    double loose_spread = 0.0;
    double tight_spread = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const std::string metric = "metric-" + std::to_string(i);
        loose_spread += std::fabs(loose.noise_for(metric, kBase));
        tight_spread += std::fabs(tight.noise_for(metric, kBase));
    }

    // Mean absolute deviation of Laplace is exactly b, so the ratio should be
    // about 100. Asserting only "much larger" keeps this robust.
    LRD_CHECK(tight_spread > loose_spread * 10.0);
}

LRD_TEST("no draw is infinite or NaN") {
    // The inverse-CDF formula diverges at the endpoints of its uniform input,
    // and uniform_real_distribution's range is half-open - so one endpoint is
    // reachable. This is the guard on that.
    const NoisyCounters counters(config_of());
    for (int i = 0; i < 20000; ++i) {
        const double draw = counters.noise_for("m" + std::to_string(i), kBase);
        LRD_REQUIRE(std::isfinite(draw));
    }
}

// --------------------------------------------------------------------------
// Epoch memoisation - the averaging attack
// --------------------------------------------------------------------------

LRD_TEST("the same metric and epoch always give the same answer") {
    const NoisyCounters counters(config_of());
    const rank::Timestamp start = aligned_base();
    const std::uint64_t first = counters.publish("gets", 1000, start);

    // Sampling across the whole epoch, stopping one second short of its end.
    for (int i = 0; i < 100; ++i) {
        const auto when = start + std::chrono::seconds{i * 35};  // 0 .. 3465s of 3600
        LRD_CHECK_EQ(counters.publish("gets", 1000, when), first);
    }

    // And one second past the end is a different epoch, so a different draw is
    // expected there - which is what the next test covers.
}

LRD_TEST("repeated polling cannot average the noise away") {
    // The attack the memoisation exists to prevent, run as a test.
    //
    // If every read drew fresh noise, an observer could poll the endpoint and
    // average the results: the noise cancels, the true value emerges, and the
    // mechanism has protected nothing while looking exactly as though it had.
    //
    // Here 5000 reads within one epoch all return the identical value, so there
    // is nothing to average.
    const NoisyCounters counters(config_of());
    constexpr std::uint64_t kTruth = 137;

    std::set<std::uint64_t> observed;
    for (int i = 0; i < 5000; ++i) {
        observed.insert(counters.publish("recommends", kTruth,
                                         at(std::chrono::milliseconds{i})));
    }

    // One distinct value across 5000 reads: there is nothing to average.
    LRD_CHECK_EQ(observed.size(), std::size_t{1});
}

LRD_TEST("a new epoch draws fresh noise") {
    // The other half: the value must not be frozen forever, or a counter would
    // stop tracking reality. Different epochs are independent draws - which is
    // also where the budget goes, see docs/privacy.md.
    const NoisyCounters counters(config_of());

    std::set<std::uint64_t> observed;
    const rank::Timestamp start = aligned_base();
    for (int epoch = 0; epoch < 40; ++epoch) {
        observed.insert(counters.publish("gets", 1000, start + kEpoch * epoch));
    }

    // 40 independent Laplace draws around 1000 will not collapse to one value.
    LRD_CHECK(observed.size() > 5);
}

LRD_TEST("different metrics get independent noise") {
    // Sharing one draw across metrics would make their errors correlated, and a
    // reader who knew one counter exactly could correct the others.
    const NoisyCounters counters(config_of());

    std::set<std::uint64_t> observed;
    for (const char* metric : {"gets", "hits", "misses", "recommends", "requests"}) {
        observed.insert(counters.publish(metric, 1000, kBase));
    }
    LRD_CHECK(observed.size() > 1);
}

LRD_TEST("the same seed reproduces the same output across instances") {
    // Determinism across restarts matters: a daemon that redrew its noise on
    // every restart would hand an observer a fresh sample for free, restoring
    // the averaging attack to anyone who can cause a restart.
    const NoisyCounters first(config_of());
    const NoisyCounters second(config_of());

    for (const char* metric : {"gets", "hits", "recommends"}) {
        LRD_CHECK_EQ(first.publish(metric, 500, kBase), second.publish(metric, 500, kBase));
    }
}

LRD_TEST("different seeds produce different output") {
    PrivacyConfig a = config_of();
    PrivacyConfig b = config_of();
    b.seed = a.seed ^ 0xFFFFFFFFULL;

    const NoisyCounters first(a);
    const NoisyCounters second(b);

    int differences = 0;
    for (int i = 0; i < 50; ++i) {
        const std::string metric = "m" + std::to_string(i);
        if (first.publish(metric, 1000, kBase) != second.publish(metric, 1000, kBase)) {
            ++differences;
        }
    }
    LRD_CHECK(differences > 40);
}

// --------------------------------------------------------------------------
// Post-processing
// --------------------------------------------------------------------------

LRD_TEST("published counts are never negative") {
    // Laplace is two-sided, so a small count plus noise can go below zero. A
    // negative "number of requests" is nonsense a consumer would have to handle.
    const NoisyCounters counters(config_of(true, 0.05, /*suppression=*/0, /*rounding=*/1));
    for (int i = 0; i < 2000; ++i) {
        const std::uint64_t value =
            counters.publish("m" + std::to_string(i), 1, at(std::chrono::milliseconds{i}));
        LRD_REQUIRE(value <= 1000000);  // i.e. did not wrap around through zero
    }
}

LRD_TEST("small counts are suppressed to zero") {
    // Noise on a count of 1 does not hide the 1. Suppressing the low tail is
    // what stops the mechanism being decorative exactly where it matters most.
    const NoisyCounters counters(config_of(true, 1.0, /*suppression=*/10, /*rounding=*/1));

    int suppressed = 0;
    for (int i = 0; i < 200; ++i) {
        if (counters.publish("m" + std::to_string(i), 2, kBase) == 0) {
            ++suppressed;
        }
    }
    // With a true count of 2 and a threshold of 10, nearly everything goes.
    LRD_CHECK(suppressed > 180);
}

LRD_TEST("a large count survives suppression") {
    const NoisyCounters counters(config_of(true, 1.0, /*suppression=*/10, /*rounding=*/1));

    int survived = 0;
    for (int i = 0; i < 200; ++i) {
        if (counters.publish("m" + std::to_string(i), 10000, kBase) > 0) {
            ++survived;
        }
    }
    LRD_CHECK_EQ(survived, 200);
}

LRD_TEST("suppression tests the noisy value, not the exact one") {
    // The subtle part, and the one most likely to be got wrong.
    //
    // Suppressing on the *exact* count would leak precisely what the mechanism
    // hides: a reader seeing zero would learn the true count was below the
    // threshold - a sharp statement about the underlying data that no amount of
    // noise elsewhere undoes.
    //
    // Testing the noisy value keeps the whole pipeline post-processing. The
    // observable consequence is that a count just above the threshold is
    // *sometimes* suppressed and sometimes not, which is exactly the ambiguity
    // wanted. A mechanism suppressing on the exact value would be perfectly
    // consistent here - and that consistency is the leak.
    const NoisyCounters counters(config_of(true, 1.0, /*suppression=*/8, /*rounding=*/1));

    int suppressed = 0;
    int published = 0;
    for (int i = 0; i < 400; ++i) {
        if (counters.publish("m" + std::to_string(i), 9, kBase) == 0) {
            ++suppressed;
        } else {
            ++published;
        }
    }

    LRD_CHECK(suppressed > 0);
    LRD_CHECK(published > 0);
}

LRD_TEST("rounding lands on a multiple") {
    const NoisyCounters counters(config_of(true, 1.0, /*suppression=*/0, /*rounding=*/10));
    for (int i = 0; i < 200; ++i) {
        const std::uint64_t value = counters.publish("m" + std::to_string(i), 5000, kBase);
        LRD_REQUIRE(value % 10 == 0);
    }
}

LRD_TEST("a rounding of zero is treated as one rather than dividing by zero") {
    PrivacyConfig config = config_of();
    config.rounding = 0;
    const NoisyCounters counters(config);
    LRD_CHECK(counters.config().rounding == 1);
    // And it still works.
    (void)counters.publish("gets", 1000, kBase);
}

// --------------------------------------------------------------------------
// Utility
// --------------------------------------------------------------------------

LRD_TEST("the published value stays near the truth for large counts") {
    // The mechanism has to remain *useful*. With epsilon 1 the noise has scale 1,
    // so a counter in the tens of thousands is barely perturbed - the privacy
    // cost falls almost entirely on small counts, which is the intended shape.
    const NoisyCounters counters(config_of(true, 1.0, /*suppression=*/5, /*rounding=*/1));
    constexpr std::uint64_t kTruth = 50000;

    for (int i = 0; i < 200; ++i) {
        const std::uint64_t value = counters.publish("m" + std::to_string(i), kTruth, kBase);
        const auto error = static_cast<double>(value) - static_cast<double>(kTruth);
        LRD_REQUIRE(std::fabs(error) < 100.0);
    }
}

LRD_TEST("publish is safe to call from many threads") {
    // It is const, holds no state and takes no locks - this is the case that
    // proves it, and the one TSan is pointed at.
    const NoisyCounters counters(config_of());
    std::atomic<int> mismatches{0};
    const std::uint64_t expected = counters.publish("gets", 1234, kBase);

    {
        std::vector<std::jthread> threads;
        threads.reserve(8);
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 5000; ++i) {
                    if (counters.publish("gets", 1234, kBase) != expected) {
                        mismatches.fetch_add(1);
                    }
                }
            });
        }
    }

    LRD_CHECK_EQ(mismatches.load(), 0);
}
