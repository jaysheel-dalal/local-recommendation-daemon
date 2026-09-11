#pragma once

#include "lrd/rank/item.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lrd::policy {

/// Compliance limits applied to every item before it can be shown.
struct PolicyConfig {
    /// Lifetime cap: the most times one item may ever be returned. 0 disables.
    std::uint64_t exposure_cap = 0;

    /// Rolling-window cap: the most times one item may be returned within
    /// `frequency_window`. 0 disables.
    std::uint32_t frequency_limit = 0;

    std::chrono::milliseconds frequency_window =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{1});

    /// Maximum number of items the store tracks counters for.
    ///
    /// Bounded because the store is in memory and its keys outlive the items
    /// themselves (see `forget` in exposure_store.hpp for why deleting an item
    /// deliberately does *not* clear its counters).
    std::size_t max_tracked_items = 100000;

    /// What to do when the store is full and a *new* item asks to be shown.
    ///
    /// false (the default) is fail-closed: refuse the new item rather than evict
    /// an existing item's counters. The reasoning is asymmetric - under-showing
    /// is a recoverable revenue problem, over-showing past a cap is a compliance
    /// breach - so when the two conflict, the safe direction is to stop.
    ///
    /// true is fail-open: evict the least recently used counters and allow. It
    /// exists because "never serve anything again" is also a real failure, and
    /// which one is worse is a deployment decision rather than a code one.
    bool fail_open_when_full = false;
};

/// Why an item was or was not allowed.
///
/// Distinct values rather than a bool because the daemon reports them
/// separately: a slate short because everything hit its frequency limit is a
/// tuning problem, whereas one short because the store is full is an operational
/// alarm.
enum class Decision : std::uint8_t {
    Allowed,
    ExposureCapReached,
    FrequencyLimited,
    StoreFull,
};

[[nodiscard]] const char* to_string(Decision decision) noexcept;

struct PolicyMetrics {
    std::uint64_t allowed = 0;
    std::uint64_t exposure_blocked = 0;
    std::uint64_t frequency_blocked = 0;
    std::uint64_t store_full_blocked = 0;
    std::uint64_t evicted = 0;  ///< Counters discarded under fail-open.
    std::uint64_t tracked = 0;  ///< Items currently tracked.
};

}  // namespace lrd::policy
