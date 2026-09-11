#pragma once

#include "lrd/rank/item.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace lrd::privacy {

/// Configuration for the noise mechanism applied to exported metrics.
struct PrivacyConfig {
    /// Off by default. An operator running a daemon for their own diagnostics
    /// wants exact numbers; the mechanism is for builds whose counters leave the
    /// device or reach a log that does.
    bool enabled = false;

    /// The privacy parameter, per metric per epoch.
    ///
    /// Smaller means more noise and a stronger guarantee. 1.0 is a conventional
    /// middle: the Laplace scale becomes sensitivity/epsilon, so with
    /// sensitivity 1 the noise has scale 1 and roughly 95% of draws land within
    /// +/-3 of the true count.
    double epsilon = 1.0;

    /// How much one *event* can change a counter.
    ///
    /// One request increments `requests` by exactly 1, so the sensitivity of
    /// these counters is 1. Stated explicitly rather than assumed, because it is
    /// the term that makes the scale meaningful - and because a counter that
    /// could move by 10 per event would need ten times the noise for the same
    /// guarantee.
    double sensitivity = 1.0;

    /// Noisy values at or below this are reported as zero.
    ///
    /// Noise on a count of 1 does not hide the 1: an observer who sees "2" knows
    /// the truth was a small number, and often that the event happened at all.
    /// Suppressing the low tail is what stops the mechanism from being
    /// decorative at exactly the counts where it matters most.
    std::uint64_t suppression_threshold = 5;

    /// Reported values are rounded to a multiple of this. 1 disables.
    ///
    /// Rounding is not itself a privacy mechanism - it is deterministic, so
    /// repeated observation defeats it - but applied *after* noise it costs
    /// nothing and removes the spurious precision of a figure like 40 237 that
    /// invites a reader to trust its last digits.
    std::uint64_t rounding = 10;

    /// How long one noise draw is reused. See the note on `publish`.
    std::chrono::milliseconds epoch =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{1});

    /// Deterministic seed. 0 means "draw one from the system at startup".
    ///
    /// A fixed seed makes a run reproducible, which tests need. It is not a
    /// secret and is not meant to be: the guarantee comes from the noise
    /// distribution, not from the attacker's ignorance of the seed. A build that
    /// wanted to defend against an attacker who can read the binary would need a
    /// secret seed *and* a cryptographic generator - see the limitations in
    /// docs/privacy.md.
    std::uint64_t seed = 0;
};

/// Adds calibrated noise to counters on their way out of the process.
///
/// ## What this protects, and what it cannot
///
/// This is **event-level** differential privacy, not user-level, and the
/// distinction is not a technicality.
///
/// Differential privacy normally bounds what an observer learns about one
/// *individual's* contribution to an aggregate. That framing assumes many
/// individuals contribute to one counter. Here the daemon runs on one person's
/// device, so every increment is that same person's: their total contribution to
/// `recommends` is the entire value of `recommends`. Bounding *that* would mean
/// reporting nothing at all.
///
/// What the mechanism does provide is meaningful and worth stating precisely: an
/// observer of an exported counter cannot confidently determine whether any
/// **particular event** occurred. Did this user ask for a recommendation in the
/// last hour? Was this item served? Those are the questions the noise answers
/// "cannot tell" to, and they are the questions that leak behaviour.
///
/// Claiming user-level DP here would be the token gesture. Event-level is what
/// the arithmetic actually supports.
///
/// ## Noise is applied at the boundary, never to the source
///
/// The daemon's own counters stay exact. Only the copy being handed out is
/// noised. Two reasons:
///
///   * Noising the source destroys the ability to debug the system - an operator
///     chasing a hit-rate problem needs the real number.
///   * Noise applied repeatedly to a running total compounds, so a counter
///     noised in place would drift further from the truth on every increment
///     rather than being perturbed once.
///
/// ## The same epoch gives the same answer
///
/// `publish` is a pure function of (seed, metric, epoch, value). Ask twice
/// within an epoch and the same noisy figure comes back.
///
/// This matters more than it might appear. If each read drew fresh noise, an
/// observer could call the endpoint a thousand times and average the results -
/// the noise cancels, the true value emerges, and the mechanism has protected
/// nothing while looking exactly as if it had. Making the draw deterministic in
/// the epoch is what closes that.
///
/// It is implemented as a hash rather than a cache: no map, no eviction policy,
/// no lock, and the answer is identical across daemon restarts within the same
/// epoch.
///
/// ## The budget still accumulates
///
/// A new epoch means a new independent draw, so an observer sampling across N
/// epochs gets N independent observations and the effective parameter is N x
/// epsilon. A one-hour epoch observed for a day is 24x the stated epsilon. That
/// is inherent to publishing a changing quantity over time and is accounted for
/// in docs/privacy.md rather than hidden.
///
/// ## Thread safety
///
/// `publish` is const, holds no state, and takes no locks. Safe to call from any
/// number of threads.
class NoisyCounters {
public:
    explicit NoisyCounters(PrivacyConfig config);

    /// Returns the value to publish for `metric`.
    ///
    /// With `enabled` false this returns `exact` unchanged, so the mechanism can
    /// be switched off without any other code changing shape.
    [[nodiscard]] std::uint64_t publish(std::string_view metric, std::uint64_t exact,
                                        rank::Timestamp now) const;

    /// The raw Laplace draw for a metric and instant, before clamping, rounding
    /// or suppression. Exposed for tests, which need to characterise the
    /// distribution rather than only its post-processed shape.
    [[nodiscard]] double noise_for(std::string_view metric, rank::Timestamp now) const;

    [[nodiscard]] std::uint64_t epoch_index(rank::Timestamp now) const noexcept;

    [[nodiscard]] const PrivacyConfig& config() const noexcept { return config_; }

    /// sensitivity / epsilon - the Laplace scale parameter b.
    [[nodiscard]] double scale() const noexcept;

private:
    PrivacyConfig config_;
};

}  // namespace lrd::privacy
