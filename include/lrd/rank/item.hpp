#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace lrd::rank {

/// Numeric, because ad inventory ids are numeric and because it exercises
/// something real: `std::hash<std::uint64_t>` in libstdc++ is the identity
/// function, so a sharded cache keyed by these would put every multiple of N in
/// shard 0 without the splitmix64 mixing added in step 7. The item store is
/// keyed by this type, which makes that mixing load-bearing rather than
/// theoretical.
using ItemId = std::uint64_t;

/// Wall-clock instant at millisecond resolution.
///
/// system_clock, not steady_clock: these are absolute times that must survive
/// being written to the wire and compared against timestamps minted by another
/// process. steady_clock is monotonic but its epoch is arbitrary and unrelated
/// between processes, which makes it right for measuring durations (as the
/// benchmarks do) and wrong for recording when something happened.
///
/// std::chrono::sys_time rather than a raw int64: the type says what the number
/// means, so a duration cannot be passed where an instant is expected. The
/// codecs convert to milliseconds-since-epoch at the wire boundary and nowhere
/// else.
using Timestamp = std::chrono::sys_time<std::chrono::milliseconds>;

[[nodiscard]] inline std::int64_t to_epoch_millis(Timestamp t) noexcept {
    return t.time_since_epoch().count();
}

[[nodiscard]] inline Timestamp from_epoch_millis(std::int64_t millis) noexcept {
    return Timestamp{std::chrono::milliseconds{millis}};
}

/// A ranking candidate: the metadata the daemon holds about one ad-like item.
///
/// Every field here is used by the scorer. That is deliberate - a struct
/// carrying decorative fields invites a reviewer to ask what they are for, and
/// the honest answer would be "nothing".
struct Item {
    ItemId id = 0;

    /// Matched against the user signal's affinities, and one of the two axes
    /// the diversity penalty works along.
    ///
    /// A std::string rather than an interned id. Interning would make the
    /// comparison a pointer test instead of a string compare, which matters on
    /// a hot path scanning thousands of candidates - but it needs a global
    /// intern table with its own locking, and the categories here are short
    /// enough that libstdc++'s small-string optimisation keeps them inline.
    /// Worth revisiting with a profile, not before.
    std::string category;

    /// The second diversity axis: three ads from one advertiser in a row is a
    /// bad result even when all three are individually well matched.
    std::string advertiser;

    /// Quality times bid, already normalised by whatever upstream system
    /// produced it. Must be strictly positive to be eligible - a zero or
    /// negative base score is how an item is switched off without deleting it.
    double base_score = 0.0;

    /// Drives the recency term. An item created in the future is treated as
    /// having zero age rather than a negative one.
    Timestamp created_at{};

    /// Hard eligibility bound. The epoch (0 ms) means "never expires", which
    /// keeps the common case out of the wire format's way.
    Timestamp expires_at{};

    [[nodiscard]] bool never_expires() const noexcept {
        return expires_at.time_since_epoch().count() == 0;
    }
};

/// One entry in a recommendation result.
struct RankedItem {
    ItemId id = 0;
    double score = 0.0;
    std::string category;
    std::string advertiser;
};

}  // namespace lrd::rank
