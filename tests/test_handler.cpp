#include "lrd/daemon/handler.hpp"
#include "lrd/policy/policy.hpp"
#include "lrd/privacy/noise.hpp"

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <string>
#include <variant>
#include <vector>

using lrd::daemon::Handler;
using namespace lrd::proto;
namespace rank = lrd::rank;

namespace {

/// One shard for the semantics tests: eviction is per-shard, and these cases are
/// about request handling rather than striping.
constexpr std::size_t kCapacity = 256;
constexpr std::size_t kShards = 1;

/// A pinned clock. Ranking uses `now`, so a handler under test must not depend on
/// the wall clock - otherwise the recency term drifts between runs and every
/// assertion about scores becomes approximate.
rank::Timestamp fixed_now() {
    return rank::from_epoch_millis(1700000000000LL);
}

/// Returns a prvalue, which C++17 guarantees is constructed directly in the
/// caller - Handler is neither copyable nor movable (it owns a ShardedCache and
/// atomics), so anything needing a move here would not compile.
Handler make_handler(std::size_t capacity = kCapacity, std::size_t shards = kShards,
                     lrd::policy::PolicyConfig policy = {},
                     lrd::privacy::PrivacyConfig privacy = {}) {
    return Handler(capacity, shards, lrd::rank::ScoringConfig{}, policy, privacy, &fixed_now);
}

rank::Item item_of(rank::ItemId id, std::string category, double score,
                   std::string advertiser = "acme") {
    rank::Item item;
    item.id = id;
    item.category = std::move(category);
    item.advertiser = std::move(advertiser);
    item.base_score = score;
    item.created_at = fixed_now();
    item.expires_at = rank::from_epoch_millis(0);
    return item;
}

Request put_of(const rank::Item& item, std::uint64_t id = 1) {
    Request request;
    request.request_id = id;
    request.body = PutItem{item};
    return request;
}

Request get_of(rank::ItemId item_id, std::uint64_t id = 1) {
    Request request;
    request.request_id = id;
    request.body = GetItem{item_id};
    return request;
}

Request del_of(rank::ItemId item_id, std::uint64_t id = 1) {
    Request request;
    request.request_id = id;
    request.body = DeleteItem{item_id};
    return request;
}

Request stats_of(std::uint64_t id = 1) {
    Request request;
    request.request_id = id;
    request.body = GetStats{};
    return request;
}

Request recommend_of(std::vector<rank::CategoryAffinity> affinities, std::uint32_t count,
                     std::vector<std::string> excluded = {}, std::uint64_t id = 1) {
    Recommend recommend;
    recommend.signal.affinities = std::move(affinities);
    recommend.signal.excluded_categories = std::move(excluded);
    recommend.count = count;
    Request request;
    request.request_id = id;
    request.body = recommend;
    return request;
}

}  // namespace

// The handler takes a Request and returns a Response with no socket in sight,
// which is what makes these tests this short.

LRD_TEST("get on an empty store misses") {
    Handler handler = make_handler();
    const Response response = handler.handle(get_of(1));
    const auto* result = std::get_if<GetItemResult>(&response.body);
    LRD_REQUIRE(result != nullptr);
    LRD_CHECK(result->status == StatusCode::NotFound);
}

LRD_TEST("put then get returns every field intact") {
    Handler handler = make_handler();
    const rank::Item original = item_of(7, "tech", 0.75, "globex");
    LRD_REQUIRE(std::holds_alternative<PutItemResult>(handler.handle(put_of(original)).body));

    const Response response = handler.handle(get_of(7));
    const auto* result = std::get_if<GetItemResult>(&response.body);
    LRD_REQUIRE(result != nullptr);
    LRD_REQUIRE(result->status == StatusCode::Ok);
    LRD_CHECK_EQ(result->item.id, rank::ItemId{7});
    LRD_CHECK_EQ(result->item.category, std::string("tech"));
    LRD_CHECK_EQ(result->item.advertiser, std::string("globex"));
    LRD_CHECK_EQ(result->item.base_score, 0.75);
}

LRD_TEST("put overwrites an existing id") {
    Handler handler = make_handler();
    (void)handler.handle(put_of(item_of(1, "tech", 0.1)));
    (void)handler.handle(put_of(item_of(1, "sport", 0.9)));

    const Response response = handler.handle(get_of(1));
    const auto* result = std::get_if<GetItemResult>(&response.body);
    LRD_REQUIRE(result != nullptr);
    LRD_CHECK_EQ(result->item.category, std::string("sport"));
}

LRD_TEST("a zero item id is rejected") {
    // Zero is reserved as "unset"; accepting it would make an unset id
    // indistinguishable from a real one in every later lookup.
    Handler handler = make_handler();
    const Response response = handler.handle(put_of(item_of(0, "tech", 1.0)));
    const auto* failure = std::get_if<Failure>(&response.body);
    LRD_REQUIRE(failure != nullptr);
    LRD_CHECK(failure->status == StatusCode::InvalidRequest);
}

LRD_TEST("an empty category is rejected") {
    // Ranking keys affinity off the category; an empty one can never match a
    // signal and would only ever be served at the default affinity.
    Handler handler = make_handler();
    const Response response = handler.handle(put_of(item_of(1, "", 1.0)));
    const auto* failure = std::get_if<Failure>(&response.body);
    LRD_REQUIRE(failure != nullptr);
    LRD_CHECK(failure->status == StatusCode::InvalidRequest);
}

LRD_TEST("delete removes an item and reports whether it was there") {
    Handler handler = make_handler();
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));

    {
        const Response response = handler.handle(del_of(1));
        const auto* result = std::get_if<DeleteItemResult>(&response.body);
        LRD_REQUIRE(result != nullptr);
        LRD_CHECK(result->status == StatusCode::Ok);
    }
    {
        const Response response = handler.handle(del_of(1));
        const auto* result = std::get_if<DeleteItemResult>(&response.body);
        LRD_REQUIRE(result != nullptr);
        LRD_CHECK(result->status == StatusCode::NotFound);
    }
}

LRD_TEST("responses echo the request id") {
    Handler handler = make_handler();
    LRD_CHECK_EQ(handler.handle(get_of(1, 12345)).request_id, std::uint64_t{12345});
    LRD_CHECK_EQ(handler.handle(put_of(item_of(1, "tech", 1.0), 999)).request_id,
                 std::uint64_t{999});
    LRD_CHECK_EQ(handler.handle(del_of(1, 7)).request_id, std::uint64_t{7});
    LRD_CHECK_EQ(handler.handle(stats_of(88)).request_id, std::uint64_t{88});
    LRD_CHECK_EQ(handler.handle(recommend_of({{"tech", 1.0}}, 3, {}, 55)).request_id,
                 std::uint64_t{55});
}

// --------------------------------------------------------------------------
// Recommendations
// --------------------------------------------------------------------------

LRD_TEST("recommend on an empty store returns an empty slate, not an error") {
    Handler handler = make_handler();
    const Response response = handler.handle(recommend_of({{"tech", 1.0}}, 5));
    const auto* result = std::get_if<RecommendResult>(&response.body);
    LRD_REQUIRE(result != nullptr);
    LRD_CHECK(result->status == StatusCode::Ok);
    LRD_CHECK(result->items.empty());
}

LRD_TEST("recommend ranks stored items by the signal") {
    Handler handler = make_handler();
    (void)handler.handle(put_of(item_of(1, "tech", 0.5, "a")));
    (void)handler.handle(put_of(item_of(2, "sport", 0.5, "b")));
    (void)handler.handle(put_of(item_of(3, "food", 0.5, "c")));

    const Response response = handler.handle(recommend_of({{"tech", 1.0}, {"sport", 0.5}}, 2));
    const auto* result = std::get_if<RecommendResult>(&response.body);
    LRD_REQUIRE(result != nullptr);
    LRD_REQUIRE(result->items.size() == 2);
    // tech has the highest affinity, so it leads.
    LRD_CHECK_EQ(result->items[0].id, rank::ItemId{1});
    LRD_CHECK_EQ(result->items[0].category, std::string("tech"));
}

LRD_TEST("recommend honours an excluded category") {
    Handler handler = make_handler();
    (void)handler.handle(put_of(item_of(1, "gambling", 1000.0, "a")));
    (void)handler.handle(put_of(item_of(2, "tech", 0.1, "b")));

    const Response response = handler.handle(recommend_of({{"gambling", 1.0}}, 5, {"gambling"}));
    const auto* result = std::get_if<RecommendResult>(&response.body);
    LRD_REQUIRE(result != nullptr);
    for (const rank::RankedItem& item : result->items) {
        LRD_CHECK(item.category != "gambling");
    }
}

LRD_TEST("recommend sees items across every shard") {
    // The scan walks shards one at a time, so an item in any shard must be
    // reachable. With 16 shards and 200 ids, every shard is populated.
    Handler handler = make_handler(1024, 16);
    for (rank::ItemId id = 1; id <= 200; ++id) {
        (void)handler.handle(put_of(item_of(id, "tech", 0.5, "adv-" + std::to_string(id))));
    }

    const Response response = handler.handle(recommend_of({{"tech", 1.0}}, 50));
    const auto* result = std::get_if<RecommendResult>(&response.body);
    LRD_REQUIRE(result != nullptr);
    LRD_CHECK_EQ(result->items.size(), std::size_t{50});
}

LRD_TEST("a recommendation scan does not disturb recency") {
    // The reason the scan uses a non-mutating traversal. If recommend touched
    // every item through get(), each recommendation would mark the whole cache
    // most-recently-used and eviction would become effectively random.
    //
    // Capacity 4, one shard. Item 1 is the least recently used. A recommend scans
    // all four; if that counted as a use, the next insert would evict something
    // other than item 1.
    Handler handler = make_handler(4, 1);
    for (rank::ItemId id = 1; id <= 4; ++id) {
        (void)handler.handle(put_of(item_of(id, "tech", 0.5)));
    }

    (void)handler.handle(recommend_of({{"tech", 1.0}}, 4));
    (void)handler.handle(put_of(item_of(5, "tech", 0.5)));

    const Response response = handler.handle(get_of(1));
    const auto* gone = std::get_if<GetItemResult>(&response.body);
    LRD_REQUIRE(gone != nullptr);
    LRD_CHECK(gone->status == StatusCode::NotFound);
}

LRD_TEST("a count of zero is rejected") {
    Handler handler = make_handler();
    const Response response = handler.handle(recommend_of({{"tech", 1.0}}, 0));
    const auto* failure = std::get_if<Failure>(&response.body);
    LRD_REQUIRE(failure != nullptr);
    LRD_CHECK(failure->status == StatusCode::InvalidRequest);
}

LRD_TEST("a count above the protocol limit is rejected") {
    Handler handler = make_handler();
    const Response response =
        handler.handle(recommend_of({{"tech", 1.0}}, kMaxRecommendCount + 1));
    const auto* failure = std::get_if<Failure>(&response.body);
    LRD_REQUIRE(failure != nullptr);
    LRD_CHECK(failure->status == StatusCode::InvalidRequest);
}

LRD_TEST("dry_run behaves identically in step 9") {
    // Nothing is recorded yet, so a dry run and a live run must agree. Pinned now
    // so that step 10, which makes them differ, has to change this deliberately.
    Handler handler = make_handler();
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));

    Request dry = recommend_of({{"tech", 1.0}}, 1);
    std::get<Recommend>(dry.body).dry_run = true;

    const Response live_response = handler.handle(recommend_of({{"tech", 1.0}}, 1));
    const Response dry_response = handler.handle(dry);
    const auto* live = std::get_if<RecommendResult>(&live_response.body);
    const auto* dried = std::get_if<RecommendResult>(&dry_response.body);
    LRD_REQUIRE(live != nullptr);
    LRD_REQUIRE(dried != nullptr);
    LRD_REQUIRE(live->items.size() == 1);
    LRD_REQUIRE(dried->items.size() == 1);
    LRD_CHECK_EQ(live->items[0].id, dried->items[0].id);
    LRD_CHECK_EQ(live->items[0].score, dried->items[0].score);
}

// --------------------------------------------------------------------------
// Stats and eviction
// --------------------------------------------------------------------------

LRD_TEST("stats count each operation separately") {
    Handler handler = make_handler();
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));
    (void)handler.handle(get_of(1));    // hit
    (void)handler.handle(get_of(999));  // miss
    (void)handler.handle(del_of(1));
    (void)handler.handle(recommend_of({{"tech", 1.0}}, 3));

    const Response response = handler.handle(stats_of());
    const auto* result = std::get_if<StatsResult>(&response.body);
    LRD_REQUIRE(result != nullptr);
    LRD_CHECK_EQ(result->stats.puts, std::uint64_t{1});
    LRD_CHECK_EQ(result->stats.gets, std::uint64_t{2});
    LRD_CHECK_EQ(result->stats.deletes, std::uint64_t{1});
    LRD_CHECK_EQ(result->stats.recommends, std::uint64_t{1});
    LRD_CHECK_EQ(result->stats.hits, std::uint64_t{1});
    LRD_CHECK_EQ(result->stats.misses, std::uint64_t{1});
    LRD_CHECK_EQ(result->stats.requests, std::uint64_t{6});
}

LRD_TEST("a put that overflows capacity evicts, and eviction reads as a miss") {
    Handler handler = make_handler(2, 1);
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));
    (void)handler.handle(put_of(item_of(2, "tech", 1.0)));
    (void)handler.handle(put_of(item_of(3, "tech", 1.0)));  // evicts item 1

    const Response gone_response = handler.handle(get_of(1));
    const auto* gone = std::get_if<GetItemResult>(&gone_response.body);
    LRD_REQUIRE(gone != nullptr);
    LRD_CHECK(gone->status == StatusCode::NotFound);

    const Response response = handler.handle(stats_of());
    const auto* stats = std::get_if<StatsResult>(&response.body);
    LRD_REQUIRE(stats != nullptr);
    LRD_CHECK_EQ(stats->stats.evictions, std::uint64_t{1});
    LRD_CHECK_EQ(stats->stats.entries, std::uint64_t{2});
    LRD_CHECK_EQ(stats->stats.capacity, std::uint64_t{2});
}

// --------------------------------------------------------------------------
// Compliance (step 10)
// --------------------------------------------------------------------------

namespace {

lrd::policy::PolicyConfig policy_of(std::uint64_t exposure_cap, std::uint32_t frequency_limit,
                                    std::size_t tracked = 1024) {
    lrd::policy::PolicyConfig config;
    config.exposure_cap = exposure_cap;
    config.frequency_limit = frequency_limit;
    config.max_tracked_items = tracked;
    return config;
}

std::size_t recommend_count(Handler& handler, std::uint32_t count) {
    const Response response = handler.handle(recommend_of({{"tech", 1.0}}, count));
    const auto* result = std::get_if<RecommendResult>(&response.body);
    return result == nullptr ? 0 : result->items.size();
}

}  // namespace

LRD_TEST("an item stops being returned once it hits its exposure cap") {
    // One item, cap of 2. The first two recommendations return it; the third
    // returns an empty slate because there is nothing else to fall back to.
    Handler handler = make_handler(kCapacity, kShards, policy_of(2, 0));
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));

    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{1});
    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{1});
    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{0});
}

LRD_TEST("a capped item is replaced by the next best, not omitted") {
    // The reason the policy gate is consulted *during* selection. Item 1 is the
    // strongest candidate but capped at one show; once it is spent, a slate of
    // one must still come back full - filled by item 2.
    Handler handler = make_handler(kCapacity, kShards, policy_of(1, 0));
    (void)handler.handle(put_of(item_of(1, "tech", 1.0, "a")));
    (void)handler.handle(put_of(item_of(2, "tech", 0.5, "b")));

    {
        const Response response = handler.handle(recommend_of({{"tech", 1.0}}, 1));
        const auto* result = std::get_if<RecommendResult>(&response.body);
        LRD_REQUIRE(result != nullptr);
        LRD_REQUIRE(result->items.size() == 1);
        LRD_CHECK_EQ(result->items[0].id, rank::ItemId{1});
    }
    {
        const Response response = handler.handle(recommend_of({{"tech", 1.0}}, 1));
        const auto* result = std::get_if<RecommendResult>(&response.body);
        LRD_REQUIRE(result != nullptr);
        // Still one item - the slate filled rather than shrank.
        LRD_REQUIRE(result->items.size() == 1);
        LRD_CHECK_EQ(result->items[0].id, rank::ItemId{2});
    }
}

LRD_TEST("only returned items are charged an exposure") {
    // Three items, a slate of one, a cap of one each. After three
    // recommendations all three should be spent - meaning each recommendation
    // charged exactly the one item it returned, not every item it scored.
    Handler handler = make_handler(kCapacity, kShards, policy_of(1, 0));
    for (rank::ItemId id = 1; id <= 3; ++id) {
        (void)handler.handle(put_of(item_of(id, "tech", 1.0 / static_cast<double>(id),
                                            "adv-" + std::to_string(id))));
    }

    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{1});
    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{1});
    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{1});
    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{0});
}

LRD_TEST("a dry run reports the same slate without spending it") {
    // The behaviour the flag has existed for since step 9, now meaningful. Three
    // dry runs return the same item; a live run then spends it.
    Handler handler = make_handler(kCapacity, kShards, policy_of(1, 0));
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));

    Request dry = recommend_of({{"tech", 1.0}}, 1);
    std::get<Recommend>(dry.body).dry_run = true;

    for (int i = 0; i < 3; ++i) {
        const Response response = handler.handle(dry);
        const auto* result = std::get_if<RecommendResult>(&response.body);
        LRD_REQUIRE(result != nullptr);
        LRD_REQUIRE(result->items.size() == 1);
        LRD_CHECK_EQ(result->items[0].id, rank::ItemId{1});
    }

    // The cap is still intact, so a live run succeeds - and then exhausts it.
    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{1});
    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{0});

    // And a dry run now honestly reports that nothing is available.
    const Response response = handler.handle(dry);
    const auto* result = std::get_if<RecommendResult>(&response.body);
    LRD_REQUIRE(result != nullptr);
    LRD_CHECK(result->items.empty());
}

LRD_TEST("the frequency limit binds within a window") {
    Handler handler = make_handler(kCapacity, kShards, policy_of(0, 2));
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));

    // The clock is pinned, so every request lands in the same window.
    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{1});
    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{1});
    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{0});
}

LRD_TEST("deleting and republishing an item does not reset its exposure cap") {
    // The exploit the handler deliberately does not permit. If DeleteItem cleared
    // the counters, any client could reset a lifetime cap at will and the cap
    // would be decorative.
    Handler handler = make_handler(kCapacity, kShards, policy_of(1, 0));
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));
    LRD_REQUIRE(recommend_count(handler, 1) == 1);

    (void)handler.handle(del_of(1));
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));

    LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{0});
}

LRD_TEST("stats report why items were blocked") {
    Handler handler = make_handler(kCapacity, kShards, policy_of(1, 0));
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));

    (void)recommend_count(handler, 1);  // allowed
    (void)recommend_count(handler, 1);  // blocked on the cap

    const Response response = handler.handle(stats_of());
    const auto* stats = std::get_if<StatsResult>(&response.body);
    LRD_REQUIRE(stats != nullptr);
    LRD_CHECK_EQ(stats->stats.policy_allowed, std::uint64_t{1});
    LRD_CHECK_EQ(stats->stats.policy_exposure_blocked, std::uint64_t{1});
    LRD_CHECK_EQ(stats->stats.policy_tracked, std::uint64_t{1});
}

LRD_TEST("with no limits configured nothing is blocked") {
    // The default: caps of zero mean unlimited, so step 9 behaviour is unchanged
    // for anyone who does not opt in.
    Handler handler = make_handler();
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));

    for (int i = 0; i < 50; ++i) {
        LRD_CHECK_EQ(recommend_count(handler, 1), std::size_t{1});
    }

    const Response response = handler.handle(stats_of());
    const auto* stats = std::get_if<StatsResult>(&response.body);
    LRD_REQUIRE(stats != nullptr);
    LRD_CHECK_EQ(stats->stats.policy_exposure_blocked, std::uint64_t{0});
    LRD_CHECK_EQ(stats->stats.policy_frequency_blocked, std::uint64_t{0});
}

LRD_TEST("concurrent recommendations never exceed an item's cap") {
    // End to end through the handler, which is where the cap has to hold: eight
    // threads asking for recommendations at once, one item, a cap of 50. The
    // total number of times it is returned must be exactly 50.
    constexpr std::uint64_t kCap = 50;
    Handler handler = make_handler(kCapacity, kShards, policy_of(kCap, 0));
    (void)handler.handle(put_of(item_of(1, "tech", 1.0)));

    std::atomic<std::size_t> returned{0};
    {
        std::vector<std::jthread> threads;
        threads.reserve(8);
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                std::size_t local = 0;
                for (int i = 0; i < 100; ++i) {
                    local += recommend_count(handler, 1);
                }
                returned.fetch_add(local);
            });
        }
    }

    LRD_CHECK_EQ(returned.load(), static_cast<std::size_t>(kCap));
}
