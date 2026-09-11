#include "lrd/rank/scorer.hpp"

#include "test_harness.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace lrd::rank;

namespace {

/// A fixed "now" so nothing here depends on the wall clock. Tests that vary
/// with the time of day are tests that fail in CI at 3am.
const Timestamp kNow = from_epoch_millis(1'700'000'000'000);

Timestamp ago(std::chrono::milliseconds age) {
    return kNow - age;
}

Item make_item(ItemId id, std::string category, double base_score,
               std::chrono::milliseconds age = std::chrono::milliseconds{0},
               std::string advertiser = "adv-default") {
    Item item;
    item.id = id;
    item.category = std::move(category);
    item.advertiser = std::move(advertiser);
    item.base_score = base_score;
    item.created_at = ago(age);
    item.expires_at = from_epoch_millis(0);  // never expires
    return item;
}

UserSignal signal_for(std::vector<CategoryAffinity> affinities,
                      std::vector<std::string> excluded = {}) {
    UserSignal signal;
    signal.affinities = std::move(affinities);
    signal.excluded_categories = std::move(excluded);
    return signal;
}

std::vector<ItemId> ids_of(const std::vector<RankedItem>& ranked) {
    std::vector<ItemId> ids;
    ids.reserve(ranked.size());
    for (const RankedItem& item : ranked) {
        ids.push_back(item.id);
    }
    return ids;
}

bool nearly(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) <= tolerance;
}

constexpr auto kDay = std::chrono::hours{24};

}  // namespace

// --------------------------------------------------------------------------
// Eligibility: rules, not preferences
// --------------------------------------------------------------------------

LRD_TEST("a non-positive base score makes an item ineligible") {
    // How an item is switched off without deleting it.
    const UserSignal signal = signal_for({{"tech", 1.0}});
    LRD_CHECK(!is_eligible(make_item(1, "tech", 0.0), signal, kNow));
    LRD_CHECK(!is_eligible(make_item(2, "tech", -1.0), signal, kNow));
    LRD_CHECK(is_eligible(make_item(3, "tech", 0.0001), signal, kNow));
}

LRD_TEST("an expired item is ineligible, and the epoch means never expires") {
    const UserSignal signal = signal_for({{"tech", 1.0}});

    Item expired = make_item(1, "tech", 1.0);
    expired.expires_at = kNow - std::chrono::milliseconds{1};
    LRD_CHECK(!is_eligible(expired, signal, kNow));

    Item expiring_now = make_item(2, "tech", 1.0);
    expiring_now.expires_at = kNow;
    // Boundary: expiry is exclusive, so an item expiring exactly now is out.
    LRD_CHECK(!is_eligible(expiring_now, signal, kNow));

    Item future = make_item(3, "tech", 1.0);
    future.expires_at = kNow + std::chrono::milliseconds{1};
    LRD_CHECK(is_eligible(future, signal, kNow));

    LRD_CHECK(is_eligible(make_item(4, "tech", 1.0), signal, kNow));  // epoch = never
}

LRD_TEST("an excluded category is never eligible, whatever it scores") {
    // The distinction that matters: a zero affinity is a preference, an
    // exclusion is a guarantee. A very high base score must not defeat it.
    const UserSignal signal = signal_for({{"gambling", 1.0}}, {"gambling"});
    LRD_CHECK(!is_eligible(make_item(1, "gambling", 1000.0), signal, kNow));
}

// --------------------------------------------------------------------------
// The score terms
// --------------------------------------------------------------------------

LRD_TEST("relevance is the product of base score, affinity and recency") {
    const ScoringConfig config;
    const UserSignal signal = signal_for({{"tech", 0.5}});

    // Age zero, so recency is exactly 1.0 and the product is base x affinity.
    const Item fresh = make_item(1, "tech", 0.8);
    LRD_CHECK(nearly(base_relevance(fresh, signal, config, kNow), 0.8 * 0.5));
}

LRD_TEST("an unknown category gets the default affinity, not zero") {
    // Zero would mean a user with one recorded interest could only ever be shown
    // that one category - no discovery for them, unreachable inventory for
    // everyone else.
    ScoringConfig config;
    config.default_affinity = 0.1;
    const UserSignal signal = signal_for({{"tech", 1.0}});

    const Item unknown = make_item(1, "gardening", 1.0);
    const double relevance = base_relevance(unknown, signal, config, kNow);
    LRD_CHECK(relevance > 0.0);
    LRD_CHECK(nearly(relevance, 0.1));
}

LRD_TEST("recency halves at exactly one half-life") {
    ScoringConfig config;
    config.recency_half_life = std::chrono::duration_cast<std::chrono::milliseconds>(kDay);
    const UserSignal signal = signal_for({{"tech", 1.0}});

    const Item fresh = make_item(1, "tech", 1.0, std::chrono::milliseconds{0});
    const Item one_half_life =
        make_item(2, "tech", 1.0, std::chrono::duration_cast<std::chrono::milliseconds>(kDay));
    const Item two_half_lives =
        make_item(3, "tech", 1.0, std::chrono::duration_cast<std::chrono::milliseconds>(2 * kDay));

    LRD_CHECK(nearly(base_relevance(fresh, signal, config, kNow), 1.0));
    LRD_CHECK(nearly(base_relevance(one_half_life, signal, config, kNow), 0.5, 1e-6));
    LRD_CHECK(nearly(base_relevance(two_half_lives, signal, config, kNow), 0.25, 1e-6));
}

LRD_TEST("decay is smooth and never reaches zero") {
    // No cliff: an item one millisecond either side of any age scores almost the
    // same, so the slate does not churn at a boundary. And a very old but
    // excellent item stays reachable rather than being deleted by its age.
    ScoringConfig config;
    config.recency_half_life = std::chrono::duration_cast<std::chrono::milliseconds>(kDay);
    const UserSignal signal = signal_for({{"tech", 1.0}});

    const Item ancient =
        make_item(1, "tech", 1.0, std::chrono::duration_cast<std::chrono::milliseconds>(100 * kDay));
    const double relevance = base_relevance(ancient, signal, config, kNow);
    LRD_CHECK(relevance > 0.0);
    LRD_CHECK(relevance < 1e-20);
}

LRD_TEST("an item created in the future is treated as new, not given a bonus") {
    // Clock skew between the publishing process and this one is normal; a
    // negative age must not become a multiplier above 1.
    ScoringConfig config;
    const UserSignal signal = signal_for({{"tech", 1.0}});

    Item future = make_item(1, "tech", 1.0);
    future.created_at = kNow + std::chrono::hours{48};
    LRD_CHECK(nearly(base_relevance(future, signal, config, kNow), 1.0));
}

LRD_TEST("a zero half-life disables decay rather than dividing by zero") {
    ScoringConfig config;
    config.recency_half_life = std::chrono::milliseconds{0};
    const UserSignal signal = signal_for({{"tech", 1.0}});

    const Item old =
        make_item(1, "tech", 1.0, std::chrono::duration_cast<std::chrono::milliseconds>(365 * kDay));
    LRD_CHECK(nearly(base_relevance(old, signal, config, kNow), 1.0));
}

// --------------------------------------------------------------------------
// Retrieval
// --------------------------------------------------------------------------

LRD_TEST("selection returns the best candidates in order") {
    ScoringConfig config;
    config.category_repeat_factor = 1.0;   // diversity off, so this is pure relevance
    config.advertiser_repeat_factor = 1.0;
    const UserSignal signal = signal_for({{"tech", 1.0}});

    CandidateSelector selector(config, signal, kNow, 3);
    for (int i = 1; i <= 10; ++i) {
        // base_score 0.1 .. 1.0, each with its own advertiser
        selector.consider(make_item(static_cast<ItemId>(i), "tech", 0.1 * static_cast<double>(i),
                                    std::chrono::milliseconds{0},
                                    "adv-" + std::to_string(i)));
    }

    const std::vector<RankedItem> ranked = selector.select();
    LRD_REQUIRE(ranked.size() == 3);
    LRD_CHECK_EQ(ranked[0].id, ItemId{10});
    LRD_CHECK_EQ(ranked[1].id, ItemId{9});
    LRD_CHECK_EQ(ranked[2].id, ItemId{8});
    // Scores must be non-increasing.
    LRD_CHECK(ranked[0].score >= ranked[1].score);
    LRD_CHECK(ranked[1].score >= ranked[2].score);
}

LRD_TEST("ineligible candidates never reach the shortlist") {
    ScoringConfig config;
    const UserSignal signal = signal_for({{"tech", 1.0}}, {"blocked"});

    CandidateSelector selector(config, signal, kNow, 5);
    selector.consider(make_item(1, "tech", 1.0));
    selector.consider(make_item(2, "blocked", 1.0));   // excluded
    selector.consider(make_item(3, "tech", 0.0));      // switched off
    Item expired = make_item(4, "tech", 1.0);
    expired.expires_at = kNow - std::chrono::milliseconds{1};
    selector.consider(expired);

    LRD_CHECK_EQ(selector.considered(), std::size_t{4});
    LRD_CHECK_EQ(selector.eligible(), std::size_t{1});
    LRD_CHECK_EQ(ids_of(selector.select()), std::vector<ItemId>{1});
}

LRD_TEST("the shortlist is bounded by count times overfetch") {
    // The memory property of the bounded heap: 10000 candidates offered, but
    // only count*overfetch ever retained.
    ScoringConfig config;
    config.overfetch = 4;

    const UserSignal signal = signal_for({{"tech", 1.0}});
    CandidateSelector selector(config, signal, kNow, 5);

    for (int i = 1; i <= 10000; ++i) {
        selector.consider(make_item(static_cast<ItemId>(i), "tech",
                                    0.0001 * static_cast<double>(i)));
    }

    LRD_CHECK_EQ(selector.considered(), std::size_t{10000});
    LRD_CHECK_EQ(selector.retained(), std::size_t{20});  // 5 * 4
}

LRD_TEST("the bounded heap keeps the right candidates, not just some") {
    // A heap that retains the wrong 20 would still report retained() == 20.
    // Offering candidates in ascending order is the adversarial case: every one
    // displaces the previous root.
    ScoringConfig config;
    config.overfetch = 2;
    config.category_repeat_factor = 1.0;
    config.advertiser_repeat_factor = 1.0;

    const UserSignal signal = signal_for({{"tech", 1.0}});
    CandidateSelector selector(config, signal, kNow, 3);

    for (int i = 1; i <= 100; ++i) {
        selector.consider(make_item(static_cast<ItemId>(i), "tech",
                                    0.01 * static_cast<double>(i), std::chrono::milliseconds{0},
                                    "adv-" + std::to_string(i)));
    }

    LRD_CHECK_EQ(ids_of(selector.select()), (std::vector<ItemId>{100, 99, 98}));
}

LRD_TEST("descending offer order gives the same answer as ascending") {
    // Retrieval must not depend on the order candidates arrive in - which in the
    // daemon is the order shards happen to be scanned.
    ScoringConfig config;
    config.category_repeat_factor = 1.0;
    config.advertiser_repeat_factor = 1.0;
    const UserSignal signal = signal_for({{"tech", 1.0}});

    const auto run = [&](bool ascending) {
        CandidateSelector selector(config, signal, kNow, 4);
        for (int step = 0; step < 200; ++step) {
            const int i = ascending ? step + 1 : 200 - step;
            selector.consider(make_item(static_cast<ItemId>(i), "tech",
                                        0.005 * static_cast<double>(i),
                                        std::chrono::milliseconds{0},
                                        "adv-" + std::to_string(i)));
        }
        return ids_of(selector.select());
    };

    LRD_CHECK_EQ(run(true), run(false));
}

LRD_TEST("ties are broken deterministically by item id") {
    // Without a tie-break, identical scores would be ordered by scan order,
    // which varies with the shard hash and makes results unreproducible.
    ScoringConfig config;
    config.category_repeat_factor = 1.0;
    config.advertiser_repeat_factor = 1.0;
    const UserSignal signal = signal_for({{"tech", 1.0}});

    CandidateSelector selector(config, signal, kNow, 3);
    // Offered highest-id first; the lowest ids must still win.
    for (ItemId id : {ItemId{50}, ItemId{40}, ItemId{30}, ItemId{20}, ItemId{10}}) {
        selector.consider(make_item(id, "tech", 1.0, std::chrono::milliseconds{0},
                                    "adv-" + std::to_string(id)));
    }

    LRD_CHECK_EQ(ids_of(selector.select()), (std::vector<ItemId>{10, 20, 30}));
}

// --------------------------------------------------------------------------
// Diversity re-ranking
// --------------------------------------------------------------------------

LRD_TEST("the category penalty breaks up a single-category slate") {
    // Five equally strong tech items and one slightly weaker sport item. Without
    // diversity the answer is five tech; with it, the sport item beats the
    // second tech item because tech has been halved.
    ScoringConfig config;
    config.category_repeat_factor = 0.5;
    config.advertiser_repeat_factor = 1.0;  // isolate the category effect
    const UserSignal signal = signal_for({{"tech", 1.0}, {"sport", 1.0}});

    CandidateSelector selector(config, signal, kNow, 2);
    for (int i = 1; i <= 5; ++i) {
        selector.consider(make_item(static_cast<ItemId>(i), "tech", 1.0,
                                    std::chrono::milliseconds{0},
                                    "adv-" + std::to_string(i)));
    }
    selector.consider(make_item(99, "sport", 0.7, std::chrono::milliseconds{0}, "adv-sport"));

    const std::vector<RankedItem> ranked = selector.select();
    LRD_REQUIRE(ranked.size() == 2);
    LRD_CHECK_EQ(ranked[0].category, std::string("tech"));    // 1.0
    // Second slot: tech would be 1.0 * 0.5 = 0.5, sport is 0.7 * 1.0 = 0.7.
    LRD_CHECK_EQ(ranked[1].category, std::string("sport"));
}

LRD_TEST("the advertiser penalty is stricter than the category penalty") {
    // Several ads from one advertiser looks broken in a way that several from one
    // category does not, so the default factor is lower.
    const ScoringConfig config;
    LRD_CHECK(config.advertiser_repeat_factor < config.category_repeat_factor);

    const UserSignal signal = signal_for({{"tech", 1.0}, {"sport", 1.0}});
    CandidateSelector selector(config, signal, kNow, 2);

    // Two strong items from one advertiser, one weaker from another.
    selector.consider(make_item(1, "tech", 1.0, std::chrono::milliseconds{0}, "acme"));
    selector.consider(make_item(2, "sport", 0.9, std::chrono::milliseconds{0}, "acme"));
    selector.consider(make_item(3, "sport", 0.5, std::chrono::milliseconds{0}, "other"));

    const std::vector<RankedItem> ranked = selector.select();
    LRD_REQUIRE(ranked.size() == 2);
    LRD_CHECK_EQ(ranked[0].id, ItemId{1});
    // Item 2 would be 0.9 * 0.3 (advertiser repeat) = 0.27; item 3 is 0.5.
    LRD_CHECK_EQ(ranked[1].id, ItemId{3});
}

LRD_TEST("the penalty is geometric, so a category can still fill a thin slate") {
    // A hard cap would make the slate unfillable when there is nothing else.
    // With only tech items available, all three slots go to tech.
    ScoringConfig config;
    config.category_repeat_factor = 0.5;
    const UserSignal signal = signal_for({{"tech", 1.0}});

    CandidateSelector selector(config, signal, kNow, 3);
    for (int i = 1; i <= 5; ++i) {
        selector.consider(make_item(static_cast<ItemId>(i), "tech", 1.0,
                                    std::chrono::milliseconds{0},
                                    "adv-" + std::to_string(i)));
    }

    const std::vector<RankedItem> ranked = selector.select();
    LRD_CHECK_EQ(ranked.size(), std::size_t{3});
    // And the reported scores show the penalty compounding: 1, 0.5, 0.25.
    LRD_CHECK(nearly(ranked[0].score, 1.0));
    LRD_CHECK(nearly(ranked[1].score, 0.5));
    LRD_CHECK(nearly(ranked[2].score, 0.25));
}

LRD_TEST("diversity cannot resurrect an ineligible item") {
    // The guarantee: hard filters are applied before scoring, so no amount of
    // diversity pressure can surface an excluded category.
    ScoringConfig config;
    config.category_repeat_factor = 0.01;  // brutal pressure to diversify
    const UserSignal signal = signal_for({{"tech", 1.0}}, {"blocked"});

    CandidateSelector selector(config, signal, kNow, 3);
    selector.consider(make_item(1, "tech", 1.0, std::chrono::milliseconds{0}, "a"));
    selector.consider(make_item(2, "tech", 1.0, std::chrono::milliseconds{0}, "b"));
    selector.consider(make_item(3, "blocked", 1000.0, std::chrono::milliseconds{0}, "c"));

    for (const RankedItem& item : selector.select()) {
        LRD_CHECK(item.category != "blocked");
    }
}

// --------------------------------------------------------------------------
// Degenerate inputs
// --------------------------------------------------------------------------

LRD_TEST("asking for zero items returns nothing and retains nothing") {
    const ScoringConfig config;
    const UserSignal signal = signal_for({{"tech", 1.0}});

    CandidateSelector selector(config, signal, kNow, 0);
    for (int i = 1; i <= 10; ++i) {
        selector.consider(make_item(static_cast<ItemId>(i), "tech", 1.0));
    }
    LRD_CHECK(selector.select().empty());
    LRD_CHECK_EQ(selector.retained(), std::size_t{0});
}

LRD_TEST("asking for more items than exist returns what there is") {
    const ScoringConfig config;
    const UserSignal signal = signal_for({{"tech", 1.0}});

    CandidateSelector selector(config, signal, kNow, 10);
    selector.consider(make_item(1, "tech", 1.0, std::chrono::milliseconds{0}, "a"));
    selector.consider(make_item(2, "tech", 0.5, std::chrono::milliseconds{0}, "b"));

    LRD_CHECK_EQ(selector.select().size(), std::size_t{2});
}

LRD_TEST("an empty signal still ranks by base score and recency") {
    // A brand-new user with no recorded interests must still get a sensible
    // slate, not an empty one.
    const ScoringConfig config;
    const UserSignal empty;

    CandidateSelector selector(config, empty, kNow, 2);
    selector.consider(make_item(1, "tech", 0.2, std::chrono::milliseconds{0}, "a"));
    selector.consider(make_item(2, "sport", 0.9, std::chrono::milliseconds{0}, "b"));

    const std::vector<RankedItem> ranked = selector.select();
    LRD_REQUIRE(ranked.size() == 2);
    LRD_CHECK_EQ(ranked[0].id, ItemId{2});  // higher base score wins
}

LRD_TEST("considering nothing selects nothing") {
    const ScoringConfig config;
    const UserSignal signal;
    CandidateSelector selector(config, signal, kNow, 5);
    LRD_CHECK(selector.select().empty());
    LRD_CHECK_EQ(selector.considered(), std::size_t{0});
}
