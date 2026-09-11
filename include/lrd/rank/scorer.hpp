#pragma once

#include "lrd/rank/item.hpp"
#include "lrd/rank/signal.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace lrd::rank {

/// Tunables for the scoring heuristic. Every default here is a judgement call,
/// and each is explained where it is used.
struct ScoringConfig {
    /// Affinity applied to a category the signal says nothing about.
    ///
    /// Not zero, and that is the important part. Zero would make the affinity
    /// term annihilate every unfamiliar category, so a user with one recorded
    /// interest could only ever be shown that one category - bad for the user
    /// (no discovery) and bad for the marketplace (inventory that can never be
    /// reached). A small positive floor keeps unfamiliar items eligible but
    /// firmly behind anything the signal actually endorses.
    double default_affinity = 0.1;

    /// Time for the recency term to halve. A week is a reasonable default for
    /// ad creative; news would want hours and evergreen inventory months.
    std::chrono::milliseconds recency_half_life =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{24 * 7});

    /// Multiplied in once per already-selected item sharing this item's
    /// category. 0.5 means the second item of a category scores half, the third
    /// a quarter - a geometric penalty rather than a hard cap, so a category can
    /// still fill the slate if nothing else is close.
    double category_repeat_factor = 0.5;

    /// The same, per already-selected item from the same advertiser. Stricter
    /// than the category penalty: several ads from one advertiser looks broken
    /// in a way that several ads from one category does not.
    double advertiser_repeat_factor = 0.3;

    /// Retrieval keeps this multiple of the requested count before re-ranking.
    ///
    /// The two stages want different things: retrieval is cheap and runs over
    /// everything, re-ranking is order-dependent and must not. Over-fetching
    /// gives the re-ranker enough room to trade a little relevance for
    /// diversity. Too small and diversity has nothing to work with; too large
    /// and stage two stops being cheap.
    std::size_t overfetch = 4;

    /// Candidates scoring at or below this before diversity are dropped.
    double min_score = 0.0;
};

/// The per-candidate score, before any diversity adjustment.
///
/// ## Why the terms are multiplied rather than added
///
///     base_score x affinity x recency
///
/// Three reasons, and they are worth being able to give:
///
///   1. **Any single zero eliminates the candidate.** That is what you want from
///      eligibility-shaped terms: an expired item or a zeroed base score should
///      not be rescuable by a strong showing elsewhere. Addition lets a high
///      base score paper over zero affinity, which is exactly the failure that
///      shows users ads for things they have no interest in.
///   2. **Scale invariance.** Doubling every base score doubles every final
///      score and changes no ordering. With addition, the relative influence of
///      each term depends on the units the others happen to use, so retuning one
///      silently retunes the rest.
///   3. **The terms are independent probabilities of the same event**, roughly -
///      "is this worth showing" - and independent probabilities multiply.
///
/// The cost of multiplication is that scores get small: three factors below 1
/// compound quickly. With four or five terms, or a deeper pipeline, this would
/// want accumulating in log space (sum of logs) to stay away from denormals.
/// At three terms and double precision it is nowhere near an issue, and log
/// space would make the code harder to read for no benefit today.
[[nodiscard]] double base_relevance(const Item& item, const UserSignal& signal,
                                    const ScoringConfig& config, Timestamp now);

/// Hard eligibility, evaluated before any scoring.
///
/// Separate from the score for a reason: these are rules, not preferences. An
/// excluded category must never appear whatever the arithmetic says, and mixing
/// that into a numeric score makes the guarantee depend on tuning.
[[nodiscard]] bool is_eligible(const Item& item, const UserSignal& signal, Timestamp now);

/// Two-stage candidate selection: cheap retrieval over everything, then an
/// order-dependent re-rank over a shortlist.
///
/// ## Why two stages
///
/// The diversity penalty depends on what has *already been picked*, so it cannot
/// be precomputed and the result cannot be produced by sorting. A naive greedy
/// selection over the whole inventory is O(n*k) with a non-trivial inner loop.
///
/// Splitting it is how real ranking pipelines are shaped, and for the same
/// reason:
///
///   * **Retrieval** scores each candidate independently and keeps the best
///     `count * overfetch` using a bounded min-heap - O(n log m) with m small,
///     and crucially O(m) memory rather than O(n). The heap holds the *worst*
///     retained candidate at its root, so deciding whether a new candidate
///     belongs is one comparison.
///   * **Re-ranking** runs the order-dependent diversity logic over only those
///     m candidates - O(m*k), which for m = 4k is negligible.
///
/// ## Usage
///
/// One selector per request, on the stack. consider() is called for each
/// candidate; select() finishes. Deliberately not thread-safe and deliberately
/// not reusable across requests - it holds the request's signal by reference.
class CandidateSelector {
public:
    CandidateSelector(const ScoringConfig& config, const UserSignal& signal, Timestamp now,
                      std::size_t count);

    /// Offers one candidate to retrieval. Ineligible or low-scoring candidates
    /// are dropped here and never reach the re-ranker.
    void consider(const Item& item);

    /// Decides whether a candidate may actually be taken.
    ///
    /// Called at most once per candidate, in descending order of adjusted score,
    /// and only for candidates the re-ranker is about to select. That ordering
    /// matters: it is what lets the policy layer *reserve* a slot as a
    /// side effect and have the reservation be authoritative.
    using AcceptFn = std::function<bool(ItemId)>;

    /// Runs the diversity-aware re-rank and returns up to `count` items, best
    /// first.
    ///
    /// `accept` is consulted before a candidate is taken; returning false skips
    /// it and the loop moves to the next best. Passing an empty function accepts
    /// everything.
    ///
    /// ## Why the hook is here and not a filter applied afterwards
    ///
    /// Step 10 needs a compliance check that both *decides* and *records* - an
    /// item at its exposure cap must not be returned, and returning it must
    /// count against that cap. Two properties follow, and neither survives being
    /// done as a post-filter:
    ///
    ///   * **The slate still fills.** Rejecting the third-best candidate lets the
    ///     fourth take its place, because selection has not finished. A filter
    ///     applied to a finished list of k would simply return k-1.
    ///   * **Only what is returned is counted.** The hook runs for the handful of
    ///     candidates actually chosen, not for every candidate scored, so an item
    ///     that loses on relevance is not charged an exposure for having been
    ///     considered.
    ///
    /// The cost is one std::function call per selected item - a few per request,
    /// against a request already costing microseconds.
    [[nodiscard]] std::vector<RankedItem> select(const AcceptFn& accept = {});

    [[nodiscard]] std::size_t considered() const noexcept { return considered_; }
    [[nodiscard]] std::size_t eligible() const noexcept { return eligible_; }
    [[nodiscard]] std::size_t retained() const noexcept { return shortlist_.size(); }

private:
    /// A retained candidate. The item's fields needed later are copied rather
    /// than referenced: the cache's lock is released between shards, and a
    /// reference into a cache entry could be evicted before select() runs.
    struct Candidate {
        double relevance = 0.0;
        ItemId id = 0;
        std::string category;
        std::string advertiser;
    };

    /// Orders the min-heap. Strictly weaker relevance first, with the item id as
    /// a deterministic tie-break so that equal scores resolve the same way on
    /// every run - without it, results would depend on the order shards happened
    /// to be visited in, and tests could not assert on them.
    static bool worse(const Candidate& a, const Candidate& b) noexcept;

    const ScoringConfig& config_;
    const UserSignal& signal_;
    Timestamp now_;
    std::size_t count_;
    std::size_t limit_;

    std::vector<Candidate> shortlist_;
    std::size_t considered_ = 0;
    std::size_t eligible_ = 0;
};

}  // namespace lrd::rank
