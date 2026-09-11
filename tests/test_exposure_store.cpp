#include "lrd/policy/exposure_store.hpp"

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace lrd::policy;
namespace rank = lrd::rank;

namespace {

/// A pinned base time. Frequency windows are computed from absolute
/// milliseconds, so a test that used the wall clock would land at a different
/// offset within its window on every run and the estimates would drift.
const rank::Timestamp kBase = rank::from_epoch_millis(1700000000000LL);

rank::Timestamp at(std::chrono::milliseconds offset) {
    return kBase + offset;
}

constexpr auto kWindow = std::chrono::minutes{10};

/// A base time aligned to the start of a frequency window.
///
/// Needed by the boundary tests and worth spelling out: windows are indexed as
/// `now_ms / window_ms`, so an arbitrary timestamp sits at an arbitrary offset
/// inside its window. kBase happens to be a third of the way into one, which
/// makes "kBase + window - 1ms" land in the *middle* of the next window rather
/// than at its end. Aligning first is what makes an offset mean what it reads as.
rank::Timestamp aligned_base() {
    const auto window_ms =
        static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(kWindow).count());
    const std::int64_t base_ms = rank::to_epoch_millis(kBase);
    return rank::from_epoch_millis((base_ms / window_ms) * window_ms);
}

PolicyConfig config_of(std::uint64_t exposure_cap, std::uint32_t frequency_limit,
                       std::size_t tracked = 1024, bool fail_open = false) {
    PolicyConfig config;
    config.exposure_cap = exposure_cap;
    config.frequency_limit = frequency_limit;
    config.frequency_window = std::chrono::duration_cast<std::chrono::milliseconds>(kWindow);
    config.max_tracked_items = tracked;
    config.fail_open_when_full = fail_open;
    return config;
}

/// Reserves repeatedly and counts how many were allowed.
std::size_t reserve_n(ExposureStore& store, rank::ItemId id, std::size_t attempts,
                      rank::Timestamp when) {
    std::size_t allowed = 0;
    for (std::size_t i = 0; i < attempts; ++i) {
        if (store.reserve(id, when) == Decision::Allowed) {
            ++allowed;
        }
    }
    return allowed;
}

}  // namespace

// --------------------------------------------------------------------------
// Construction
// --------------------------------------------------------------------------

LRD_TEST("shard count must be a power of two") {
    for (const std::size_t bad : {std::size_t{0}, std::size_t{3}, std::size_t{6}}) {
        bool threw = false;
        try {
            const ExposureStore store(config_of(10, 10), bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        LRD_CHECK(threw);
    }
    const ExposureStore store(config_of(10, 10), 8);
    LRD_CHECK_EQ(store.tracked(), std::size_t{0});
}

LRD_TEST("tracking capacity below the shard count is rejected") {
    bool threw = false;
    try {
        const ExposureStore store(config_of(10, 10, /*tracked=*/4), 16);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    LRD_CHECK(threw);
}

// --------------------------------------------------------------------------
// Exposure cap
// --------------------------------------------------------------------------

LRD_TEST("a lifetime cap of zero means unlimited") {
    ExposureStore store(config_of(/*exposure_cap=*/0, /*frequency_limit=*/0), 4);
    LRD_CHECK_EQ(reserve_n(store, 1, 1000, kBase), std::size_t{1000});
}

LRD_TEST("the lifetime cap admits exactly N shows and then refuses") {
    ExposureStore store(config_of(3, 0), 4);
    LRD_CHECK(store.reserve(1, kBase) == Decision::Allowed);
    LRD_CHECK(store.reserve(1, kBase) == Decision::Allowed);
    LRD_CHECK(store.reserve(1, kBase) == Decision::Allowed);
    LRD_CHECK(store.reserve(1, kBase) == Decision::ExposureCapReached);
    // And stays refused, however long you wait - it is a lifetime cap, not a
    // windowed one.
    LRD_CHECK(store.reserve(1, at(std::chrono::hours{1000})) == Decision::ExposureCapReached);
}

LRD_TEST("the cap is per item, not global") {
    ExposureStore store(config_of(2, 0), 4);
    LRD_CHECK_EQ(reserve_n(store, 1, 5, kBase), std::size_t{2});
    LRD_CHECK_EQ(reserve_n(store, 2, 5, kBase), std::size_t{2});
    LRD_CHECK_EQ(reserve_n(store, 3, 5, kBase), std::size_t{2});
}

LRD_TEST("check does not consume a slot") {
    ExposureStore store(config_of(1, 0), 4);
    // Any number of checks, then the one reservation still succeeds.
    for (int i = 0; i < 10; ++i) {
        LRD_CHECK(store.check(1, kBase) == Decision::Allowed);
    }
    LRD_CHECK(store.reserve(1, kBase) == Decision::Allowed);
    LRD_CHECK(store.check(1, kBase) == Decision::ExposureCapReached);
}

// --------------------------------------------------------------------------
// Frequency window
// --------------------------------------------------------------------------

LRD_TEST("the frequency limit admits N shows inside one window") {
    ExposureStore store(config_of(0, 3), 4);
    LRD_CHECK_EQ(reserve_n(store, 1, 10, kBase), std::size_t{3});
}

LRD_TEST("the window recovers once it has fully passed") {
    ExposureStore store(config_of(0, 3), 4);
    LRD_REQUIRE(reserve_n(store, 1, 5, kBase) == 3);

    // Two windows later nothing is in view, so the full allowance is back.
    const auto later = at(std::chrono::duration_cast<std::chrono::milliseconds>(2 * kWindow));
    LRD_CHECK_EQ(reserve_n(store, 1, 5, later), std::size_t{3});
}

LRD_TEST("a burst across a window boundary does not get double the allowance") {
    // The failure a fixed-bucket counter has: N shows just before the boundary
    // and N more just after both pass, so 2N land inside one window's width.
    //
    // The sliding-window counter weights the previous window by how much of it is
    // still in view, so immediately after a boundary the previous window still
    // counts almost in full.
    ExposureStore store(config_of(0, 4), 4);

    const auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kWindow);
    const rank::Timestamp start = aligned_base();

    // Fill the allowance at the very end of one window.
    const rank::Timestamp end_of_window = start + window_ms - std::chrono::milliseconds{1};
    LRD_REQUIRE(reserve_n(store, 1, 10, end_of_window) == 4);

    // One millisecond later we are in the next window, but ~100% of the previous
    // one is still within the trailing view, so nothing more is admitted.
    const rank::Timestamp just_after = start + window_ms + std::chrono::milliseconds{1};
    LRD_CHECK_EQ(reserve_n(store, 1, 10, just_after), std::size_t{0});

    // Most of a window further on, the old count has largely aged out.
    const rank::Timestamp late = start + window_ms + (window_ms * 9) / 10;
    const std::size_t mid = reserve_n(store, 1, 10, late);
    LRD_CHECK(mid > 0);
    LRD_CHECK(mid <= 4);
}

LRD_TEST("the window estimate decays smoothly rather than in a step") {
    ExposureStore store(config_of(0, 10), 4);
    const auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kWindow);

    LRD_REQUIRE(reserve_n(store, 1, 10, aligned_base()) == 10);

    // Sampling across the next window, the number newly admitted should grow
    // monotonically as the previous window ages out - never jump from 0 to 10.
    std::size_t previous_allowance = 0;
    for (int step = 1; step <= 4; ++step) {
        ExposureStore fresh(config_of(0, 10), 4);
        LRD_REQUIRE(reserve_n(fresh, 1, 10, aligned_base()) == 10);

        const rank::Timestamp when = aligned_base() + window_ms + (window_ms * step) / 5;
        const std::size_t allowance = reserve_n(fresh, 1, 20, when);
        LRD_CHECK(allowance >= previous_allowance);
        previous_allowance = allowance;
    }
    // By late in the following window most of the allowance is back.
    LRD_CHECK(previous_allowance > 5);
}

LRD_TEST("a clock that moves backwards does not grant extra allowance") {
    // Defensive: system_clock is not monotonic, and an NTP correction can move it
    // backwards. Treating that as "many windows have passed" resets the counters,
    // which is the safe direction only because the lifetime cap still holds.
    ExposureStore store(config_of(5, 2), 4);
    LRD_REQUIRE(reserve_n(store, 1, 5, at(std::chrono::hours{1})) == 2);

    // Jump backwards. The frequency window resets, but the lifetime cap does not.
    LRD_CHECK_EQ(reserve_n(store, 1, 10, kBase), std::size_t{2});
    // 2 + 2 = 4 shows used; the cap of 5 leaves exactly one.
    LRD_CHECK_EQ(reserve_n(store, 1, 10, at(std::chrono::hours{2})), std::size_t{1});
}

LRD_TEST("both limits apply, and the stricter one wins") {
    ExposureStore store(config_of(/*exposure_cap=*/5, /*frequency_limit=*/2), 4);
    const auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kWindow);

    // Frequency binds first: 2 per window.
    LRD_CHECK_EQ(reserve_n(store, 1, 10, kBase), std::size_t{2});
    LRD_CHECK_EQ(reserve_n(store, 1, 10, at(2 * window_ms)), std::size_t{2});
    // Now 4 lifetime shows used; the cap leaves one, whatever the window says.
    LRD_CHECK_EQ(reserve_n(store, 1, 10, at(4 * window_ms)), std::size_t{1});
    LRD_CHECK(store.reserve(1, at(6 * window_ms)) == Decision::ExposureCapReached);
}

// --------------------------------------------------------------------------
// Capacity: fail-closed by default
// --------------------------------------------------------------------------

LRD_TEST("a full store refuses new items rather than forgetting existing caps") {
    // Fail-closed. One shard, capacity 2, so the third distinct item has nowhere
    // to go. Under-showing is recoverable; forgetting a cap and over-showing is
    // not, so when the two conflict the safe direction is to stop.
    ExposureStore store(config_of(10, 0, /*tracked=*/2, /*fail_open=*/false), 1);

    LRD_CHECK(store.reserve(1, kBase) == Decision::Allowed);
    LRD_CHECK(store.reserve(2, kBase) == Decision::Allowed);
    LRD_CHECK(store.reserve(3, kBase) == Decision::StoreFull);

    // The items already tracked keep working - they are not collateral damage.
    LRD_CHECK(store.reserve(1, kBase) == Decision::Allowed);
    LRD_CHECK(store.reserve(2, kBase) == Decision::Allowed);
    LRD_CHECK_EQ(store.metrics().store_full_blocked, std::uint64_t{1});
    LRD_CHECK_EQ(store.metrics().evicted, std::uint64_t{0});
}

LRD_TEST("fail-open evicts the least recently used counters instead") {
    ExposureStore store(config_of(10, 0, /*tracked=*/2, /*fail_open=*/true), 1);

    LRD_REQUIRE(store.reserve(1, kBase) == Decision::Allowed);
    LRD_REQUIRE(store.reserve(2, kBase) == Decision::Allowed);
    // Touch item 1 so item 2 becomes the least recently used.
    LRD_REQUIRE(store.reserve(1, kBase) == Decision::Allowed);

    LRD_CHECK(store.reserve(3, kBase) == Decision::Allowed);
    LRD_CHECK_EQ(store.metrics().evicted, std::uint64_t{1});
    LRD_CHECK_EQ(store.tracked(), std::size_t{2});
}

LRD_TEST("forget clears one item's counters") {
    // Exposed for an administrative reset. Deliberately not called when an item
    // is deleted - see the comment in exposure_store.hpp - because that would
    // make a lifetime cap resettable by delete-and-republish.
    ExposureStore store(config_of(2, 0), 4);
    LRD_REQUIRE(reserve_n(store, 1, 5, kBase) == 2);
    LRD_REQUIRE(store.reserve(1, kBase) == Decision::ExposureCapReached);

    store.forget(1);
    LRD_CHECK_EQ(reserve_n(store, 1, 5, kBase), std::size_t{2});
}

// --------------------------------------------------------------------------
// Concurrency: the race this class exists to close
// --------------------------------------------------------------------------

LRD_TEST("the cap is never exceeded under concurrent reservation") {
    // The check-then-act race, driven hard. Many threads all reserve the same
    // item; the total allowed must equal the cap *exactly*. A version that
    // checked and incremented under separate locks - or without one - would
    // overshoot here, and the overshoot would be timing-dependent enough to pass
    // a single-threaded test.
    constexpr std::uint64_t kCap = 500;
    constexpr int kThreads = 8;
    constexpr int kAttemptsPerThread = 500;

    ExposureStore store(config_of(kCap, 0), 8);
    std::atomic<std::size_t> allowed{0};

    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                std::size_t local = 0;
                for (int i = 0; i < kAttemptsPerThread; ++i) {
                    if (store.reserve(42, kBase) == Decision::Allowed) {
                        ++local;
                    }
                }
                allowed.fetch_add(local);
            });
        }
    }

    LRD_CHECK_EQ(allowed.load(), static_cast<std::size_t>(kCap));
    LRD_CHECK_EQ(store.metrics().allowed, kCap);
    LRD_CHECK_EQ(store.metrics().exposure_blocked,
                 static_cast<std::uint64_t>(kThreads * kAttemptsPerThread) - kCap);
}

LRD_TEST("the frequency limit is never exceeded under concurrent reservation") {
    constexpr std::uint32_t kLimit = 100;
    constexpr int kThreads = 8;

    ExposureStore store(config_of(0, kLimit), 8);
    std::atomic<std::size_t> allowed{0};

    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                std::size_t local = 0;
                for (int i = 0; i < 500; ++i) {
                    if (store.reserve(7, kBase) == Decision::Allowed) {
                        ++local;
                    }
                }
                allowed.fetch_add(local);
            });
        }
    }

    LRD_CHECK_EQ(allowed.load(), static_cast<std::size_t>(kLimit));
}

LRD_TEST("distinct items do not contend or interfere") {
    constexpr int kThreads = 8;
    constexpr std::uint64_t kCap = 10;

    ExposureStore store(config_of(kCap, 0, /*tracked=*/4096), 16);
    std::atomic<std::size_t> mismatches{0};

    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                // Each thread owns a disjoint id range, so every item's total
                // must come out at exactly the cap.
                for (int i = 0; i < 50; ++i) {
                    const auto id = static_cast<rank::ItemId>(t * 1000 + i + 1);
                    if (reserve_n(store, id, kCap + 5, kBase) != kCap) {
                        mismatches.fetch_add(1);
                    }
                }
            });
        }
    }

    LRD_CHECK_EQ(mismatches.load(), std::size_t{0});
}
