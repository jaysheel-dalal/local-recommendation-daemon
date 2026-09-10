#include "lrd/daemon/handler.hpp"

#include "test_harness.hpp"

#include <cstddef>
#include <string>

using lrd::daemon::Handler;
using namespace lrd::proto;

namespace {

Request get(const std::string& key, std::uint64_t id = 1) {
    Request r;
    r.type = MessageType::GetRequest;
    r.request_id = id;
    r.key = key;
    return r;
}

Request put(const std::string& key, const std::string& value, std::uint64_t id = 1) {
    Request r;
    r.type = MessageType::PutRequest;
    r.request_id = id;
    r.key = key;
    r.value = value;
    return r;
}

Request del(const std::string& key, std::uint64_t id = 1) {
    Request r;
    r.type = MessageType::DeleteRequest;
    r.request_id = id;
    r.key = key;
    return r;
}

Request stats_request(std::uint64_t id = 1) {
    Request r;
    r.type = MessageType::StatsRequest;
    r.request_id = id;
    return r;
}

}  // namespace

// The handler takes a Request and returns a Response with no socket in sight,
// which is what makes these tests this short.

/// Capacity comfortably above what any case here stores, so eviction never
/// interferes with the semantics under test. Eviction has its own coverage in
/// test_lru_cache, plus the two cases at the end of this file.
constexpr std::size_t kAmpleCapacity = 128;

LRD_TEST("get on an empty store misses") {
    Handler handler(kAmpleCapacity);
    const Response response = handler.handle(get("absent"));
    LRD_CHECK(response.type == MessageType::GetResponse);
    LRD_CHECK(response.status == StatusCode::NotFound);
    LRD_CHECK(response.value.empty());
}

LRD_TEST("put then get returns the value") {
    Handler handler(kAmpleCapacity);
    LRD_REQUIRE(handler.handle(put("k", "v")).status == StatusCode::Ok);

    const Response response = handler.handle(get("k"));
    LRD_CHECK(response.status == StatusCode::Ok);
    LRD_CHECK_EQ(response.value, std::string("v"));
}

LRD_TEST("put overwrites an existing key") {
    Handler handler(kAmpleCapacity);
    (void)handler.handle(put("k", "first"));
    (void)handler.handle(put("k", "second"));

    LRD_CHECK_EQ(handler.handle(get("k")).value, std::string("second"));
}

LRD_TEST("an empty value is stored and returned, distinct from a miss") {
    // "" and absent are different answers, and a client needs to tell them
    // apart - which is why GetResponse carries a status alongside the value
    // rather than signalling absence with an empty string.
    Handler handler(kAmpleCapacity);
    LRD_REQUIRE(handler.handle(put("k", "")).status == StatusCode::Ok);

    const Response response = handler.handle(get("k"));
    LRD_CHECK(response.status == StatusCode::Ok);
    LRD_CHECK(response.value.empty());
}

LRD_TEST("an empty key is rejected") {
    Handler handler(kAmpleCapacity);
    const Response response = handler.handle(put("", "v"));
    LRD_CHECK(response.type == MessageType::ErrorResponse);
    LRD_CHECK(response.status == StatusCode::InvalidRequest);
}

LRD_TEST("delete removes a key and reports whether it was there") {
    Handler handler(kAmpleCapacity);
    (void)handler.handle(put("k", "v"));

    LRD_CHECK(handler.handle(del("k")).status == StatusCode::Ok);
    LRD_CHECK(handler.handle(get("k")).status == StatusCode::NotFound);
    // Deleting again reports NotFound rather than succeeding silently.
    LRD_CHECK(handler.handle(del("k")).status == StatusCode::NotFound);
}

LRD_TEST("responses echo the request id") {
    // With one request in flight this is only a consistency check; it becomes
    // load bearing in step 5 when responses may complete out of order.
    Handler handler(kAmpleCapacity);
    LRD_CHECK_EQ(handler.handle(get("k", 12345)).request_id, std::uint64_t{12345});
    LRD_CHECK_EQ(handler.handle(put("k", "v", 999)).request_id, std::uint64_t{999});
    LRD_CHECK_EQ(handler.handle(del("k", 7)).request_id, std::uint64_t{7});
    LRD_CHECK_EQ(handler.handle(stats_request(88)).request_id, std::uint64_t{88});
}

LRD_TEST("stats count hits and misses separately") {
    Handler handler(kAmpleCapacity);
    (void)handler.handle(put("k", "v"));
    (void)handler.handle(get("k"));       // hit
    (void)handler.handle(get("absent"));  // miss
    (void)handler.handle(get("absent"));  // miss
    (void)handler.handle(del("k"));

    const Response response = handler.handle(stats_request());
    LRD_REQUIRE(response.type == MessageType::StatsResponse);
    LRD_CHECK_EQ(response.stats.puts, std::uint64_t{1});
    LRD_CHECK_EQ(response.stats.gets, std::uint64_t{3});
    LRD_CHECK_EQ(response.stats.deletes, std::uint64_t{1});
    LRD_CHECK_EQ(response.stats.hits, std::uint64_t{1});
    LRD_CHECK_EQ(response.stats.misses, std::uint64_t{2});
    // 5 prior requests plus this one.
    LRD_CHECK_EQ(response.stats.requests, std::uint64_t{6});
}

LRD_TEST("keys with binary content are handled verbatim") {
    Handler handler(kAmpleCapacity);
    const std::string key("a\0b", 3);
    const std::string value("x\0y\0z", 5);

    LRD_REQUIRE(handler.handle(put(key, value)).status == StatusCode::Ok);
    LRD_CHECK_EQ(handler.handle(get(key)).value, value);
    // A NUL-truncating implementation would collide these two keys.
    LRD_CHECK(handler.handle(get("a")).status == StatusCode::NotFound);
}

// --------------------------------------------------------------------------
// Cache behaviour visible through the handler. The eviction *policy* is tested
// directly in test_lru_cache; these two cases check that the handler is wired
// to it at all, and that a client sees eviction as an ordinary miss.
// --------------------------------------------------------------------------

LRD_TEST("a put that overflows capacity evicts, and the eviction reads as a miss") {
    Handler handler(2);
    (void)handler.handle(put("a", "1"));
    (void)handler.handle(put("b", "2"));
    (void)handler.handle(put("c", "3"));  // evicts "a"

    // The client-visible consequence: a successful PUT is not a promise that a
    // later GET will hit. That is the difference between a cache and a store.
    LRD_CHECK(handler.handle(get("a")).status == StatusCode::NotFound);
    LRD_CHECK(handler.handle(get("b")).status == StatusCode::Ok);
    LRD_CHECK(handler.handle(get("c")).status == StatusCode::Ok);
}

LRD_TEST("stats report eviction counts and occupancy") {
    Handler handler(2);
    (void)handler.handle(put("a", "1"));
    (void)handler.handle(put("b", "2"));
    (void)handler.handle(put("c", "3"));

    const Response response = handler.handle(stats_request());
    LRD_REQUIRE(response.type == MessageType::StatsResponse);
    LRD_CHECK_EQ(response.stats.evictions, std::uint64_t{1});
    LRD_CHECK_EQ(response.stats.entries, std::uint64_t{2});
    LRD_CHECK_EQ(response.stats.capacity, std::uint64_t{2});
}
