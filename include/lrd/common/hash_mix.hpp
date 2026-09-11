#pragma once

#include <cstddef>
#include <cstdint>

namespace lrd {

/// splitmix64's finalizer: avalanches every input bit across the whole word.
///
/// Needed wherever a hash's *low* bits are used to pick a shard, because
/// `std::hash` for integral types in libstdc++ **is the identity function**:
/// `std::hash<uint64_t>{}(8) == 8`. Masking the low bits of an identity hash
/// with 16 shards puts every multiple of 16 in shard 0, so sequential ids -
/// entirely normal for ad inventory - pile into a handful of shards while the
/// code looks correct.
///
/// Extracted here in step 10 because a second sharded structure (the policy
/// store) needs exactly the same treatment, and two copies of a subtle
/// correctness fix is one copy too many.
[[nodiscard]] constexpr std::size_t hash_mix(std::size_t hash) noexcept {
    auto value = static_cast<std::uint64_t>(hash);
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return static_cast<std::size_t>(value);
}

}  // namespace lrd
