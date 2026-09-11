#include "lrd/rank/scorer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace lrd::rank {

namespace {

/// Exponential decay with a half-life, rather than a linear ramp or a cutoff.
///
/// A cutoff ("nothing older than 7 days") puts a cliff in the ranking: an item
/// 6 days 23 hours old outranks everything, and an hour later it is gone. That
/// produces visible churn at the boundary and makes the whole slate change at
/// once. Exponential decay is smooth, has one intuitive parameter, and matches
/// how interest in a piece of creative actually fades.
///
/// exp(-ln2 * age / half_life) is 1.0 at age zero, exactly 0.5 at one half-life,
/// 0.25 at two, and asymptotically approaches zero without ever reaching it - so
/// an old-but-excellent item stays reachable instead of being deleted by its age.
double recency_multiplier(std::chrono::milliseconds age, std::chrono::milliseconds half_life) {
    if (half_life.count() <= 0) {
        return 1.0;  // decay disabled
    }
    // Clamped at zero: an item with a created_at in the future (clock skew
    // between the publishing process and this one) is treated as brand new
    // rather than given a bonus for negative age.
    const double age_ms = std::max(0.0, static_cast<double>(age.count()));
    const double half_life_ms = static_cast<double>(half_life.count());
    return std::exp(-std::numbers::ln2 * age_ms / half_life_ms);
}

}  // namespace

bool is_eligible(const Item& item, const UserSignal& signal, Timestamp now) {
    // A non-positive base score is how an item is switched off without being
    // deleted, so it is a rule rather than a very low preference.
    if (item.base_score <= 0.0) {
        return false;
    }
    if (!item.never_expires() && item.expires_at <= now) {
        return false;
    }
    if (signal.excludes(item.category)) {
        return false;
    }
    return true;
}

double base_relevance(const Item& item, const UserSignal& signal, const ScoringConfig& config,
                      Timestamp now) {
    const double affinity = signal.affinity_for(item.category, config.default_affinity);
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - item.created_at);
    const double recency = recency_multiplier(age, config.recency_half_life);
    return item.base_score * affinity * recency;
}

// --------------------------------------------------------------------------
// CandidateSelector
// --------------------------------------------------------------------------

CandidateSelector::CandidateSelector(const ScoringConfig& config, const UserSignal& signal,
                                     Timestamp now, std::size_t count)
    : config_(config),
      signal_(signal),
      now_(now),
      count_(count),
      limit_(count == 0 ? 0 : count * std::max<std::size_t>(config.overfetch, 1)) {
    shortlist_.reserve(limit_);
}

bool CandidateSelector::worse(const Candidate& a, const Candidate& b) noexcept {
    if (a.relevance != b.relevance) {
        return a.relevance < b.relevance;
    }
    // Deterministic tie-break. Without it, two items with identical relevance
    // would be ordered by whichever shard happened to be scanned first, which
    // varies with the hash and makes results unreproducible.
    return a.id < b.id;
}

void CandidateSelector::consider(const Item& item) {
    ++considered_;

    if (!is_eligible(item, signal_, now_)) {
        return;
    }
    ++eligible_;

    const double relevance = base_relevance(item, signal_, config_, now_);
    if (relevance <= config_.min_score) {
        return;
    }

    Candidate candidate{relevance, item.id, item.category, item.advertiser};

    if (shortlist_.size() < limit_) {
        shortlist_.push_back(std::move(candidate));
        std::push_heap(shortlist_.begin(), shortlist_.end(),
                       [](const Candidate& a, const Candidate& b) { return worse(b, a); });
        return;
    }

    if (limit_ == 0) {
        return;
    }

    // A min-heap keeps the *weakest* retained candidate at the root, so one
    // comparison decides whether this candidate belongs at all. That is the
    // whole point of the heap: almost every candidate in a large inventory is
    // rejected by this single comparison, and only the survivors pay log m.
    if (!worse(candidate, shortlist_.front())) {
        // std::pop_heap moves the root to the back, where it is overwritten.
        std::pop_heap(shortlist_.begin(), shortlist_.end(),
                      [](const Candidate& a, const Candidate& b) { return worse(b, a); });
        shortlist_.back() = std::move(candidate);
        std::push_heap(shortlist_.begin(), shortlist_.end(),
                       [](const Candidate& a, const Candidate& b) { return worse(b, a); });
    }
}

std::vector<RankedItem> CandidateSelector::select(const AcceptFn& accept) {
    std::vector<RankedItem> chosen;
    if (count_ == 0 || shortlist_.empty()) {
        return chosen;
    }
    chosen.reserve(std::min(count_, shortlist_.size()));

    // Counts of what has been picked so far, which is what makes the penalty
    // order-dependent and this loop necessary. Small vectors rather than maps:
    // there are at most `count` entries and the scan is trivial.
    std::vector<std::pair<std::string, int>> category_counts;
    std::vector<std::pair<std::string, int>> advertiser_counts;

    const auto count_of = [](const std::vector<std::pair<std::string, int>>& counts,
                             const std::string& key) {
        for (const auto& entry : counts) {
            if (entry.first == key) {
                return entry.second;
            }
        }
        return 0;
    };
    const auto increment = [](std::vector<std::pair<std::string, int>>& counts,
                              const std::string& key) {
        for (auto& entry : counts) {
            if (entry.first == key) {
                ++entry.second;
                return;
            }
        }
        counts.emplace_back(key, 1);
    };

    // Greedy: each round picks the best remaining candidate *given what is
    // already chosen*. Re-evaluating every round is what a sort cannot do.
    std::vector<bool> taken(shortlist_.size(), false);

    while (chosen.size() < count_) {
        std::size_t best_index = shortlist_.size();
        double best_adjusted = 0.0;

        for (std::size_t i = 0; i < shortlist_.size(); ++i) {
            if (taken[i]) {
                continue;
            }
            const Candidate& candidate = shortlist_[i];

            // std::pow with an integer exponent: the penalty is geometric in the
            // number of repeats, so the second item of a category is halved, the
            // third quartered. A flat penalty would make the second and fifth
            // equally discouraged, and a hard cap would make the slate suddenly
            // unfillable when inventory is thin.
            const double category_penalty =
                std::pow(config_.category_repeat_factor,
                         static_cast<double>(count_of(category_counts, candidate.category)));
            const double advertiser_penalty =
                std::pow(config_.advertiser_repeat_factor,
                         static_cast<double>(count_of(advertiser_counts, candidate.advertiser)));

            const double adjusted = candidate.relevance * category_penalty * advertiser_penalty;

            // Strictly greater, with the id as tie-break, so the winner of a tie
            // does not depend on shortlist order.
            if (best_index == shortlist_.size() || adjusted > best_adjusted ||
                (adjusted == best_adjusted && candidate.id < shortlist_[best_index].id)) {
                best_index = i;
                best_adjusted = adjusted;
            }
        }

        if (best_index == shortlist_.size()) {
            break;  // nothing left
        }

        const Candidate& winner = shortlist_[best_index];

        // Mark it consumed before consulting `accept`, so a rejection moves on to
        // the next best rather than reconsidering the same candidate forever.
        taken[best_index] = true;

        if (accept && !accept(winner.id)) {
            // Rejected by policy. The slate is not short by one - the loop simply
            // takes the next best candidate instead.
            continue;
        }

        chosen.push_back(RankedItem{winner.id, best_adjusted, winner.category, winner.advertiser});
        // Diversity counts only what was actually returned. Counting a rejected
        // candidate would penalise its category for a slot it never occupied.
        increment(category_counts, winner.category);
        increment(advertiser_counts, winner.advertiser);
    }

    return chosen;
}

}  // namespace lrd::rank
