#include "lrd/daemon/handler.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstddef>
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
Handler make_handler(std::size_t capacity = kCapacity, std::size_t shards = kShards) {
    return Handler(capacity, shards, lrd::rank::ScoringConfig{}, &fixed_now);
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
