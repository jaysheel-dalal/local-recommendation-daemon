#include "lrd/cache/lru_cache.hpp"

#include "test_harness.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using lrd::cache::LruCache;

namespace {

using Cache = LruCache<std::string, std::string>;

/// Renders recency order as "a,b,c" (most recent first) so a failed eviction
/// assertion prints the actual order rather than just "not equal".
std::string order(const Cache& cache) {
    std::string out;
    for (const std::string& key : cache.keys_mru_to_lru()) {
        if (!out.empty()) {
            out += ',';
        }
        out += key;
    }
    return out;
}

std::string value_or(const Cache::ValuePtr& ptr, const char* fallback) {
    return ptr ? *ptr : std::string(fallback);
}

}  // namespace

LRD_TEST("a zero capacity is rejected") {
    // Silently accepting it would evict every entry immediately, which looks
    // like a bug in the caller rather than in the configuration.
    bool threw = false;
    try {
        const Cache cache(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    LRD_CHECK(threw);
}

LRD_TEST("get on an empty cache misses") {
    Cache cache(4);
    LRD_CHECK(cache.get("absent") == nullptr);
    LRD_CHECK_EQ(cache.size(), std::size_t{0});
    LRD_CHECK_EQ(cache.metrics().misses, std::uint64_t{1});
    LRD_CHECK_EQ(cache.metrics().hits, std::uint64_t{0});
}

LRD_TEST("put then get returns the value") {
    Cache cache(4);
    cache.put("k", "v");

    const Cache::ValuePtr found = cache.get("k");
    LRD_REQUIRE(found != nullptr);
    LRD_CHECK_EQ(*found, std::string("v"));
    LRD_CHECK_EQ(cache.size(), std::size_t{1});
    LRD_CHECK_EQ(cache.metrics().hits, std::uint64_t{1});
    LRD_CHECK_EQ(cache.metrics().insertions, std::uint64_t{1});
}

LRD_TEST("insertion order is newest-first") {
    Cache cache(4);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");
    LRD_CHECK_EQ(order(cache), std::string("c,b,a"));
}

LRD_TEST("eviction removes the least recently used entry") {
    // The defining behaviour. Capacity 3, four insertions: "a" was inserted
    // first and never touched again, so "a" is what goes.
    Cache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");
    LRD_REQUIRE(cache.size() == 3);

    cache.put("d", "4");

    LRD_CHECK_EQ(cache.size(), std::size_t{3});
    LRD_CHECK_EQ(order(cache), std::string("d,c,b"));
    LRD_CHECK(cache.get("a") == nullptr);
    LRD_CHECK_EQ(cache.metrics().evictions, std::uint64_t{1});
}

LRD_TEST("a get rescues an entry from eviction") {
    // This is the test that actually distinguishes LRU from FIFO. Under FIFO,
    // "a" would still be evicted because it was inserted first. Under LRU the
    // get() moves it to the front and "b" - now the oldest untouched entry -
    // goes instead.
    Cache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    LRD_REQUIRE(cache.get("a") != nullptr);
    LRD_CHECK_EQ(order(cache), std::string("a,c,b"));

    cache.put("d", "4");

    LRD_CHECK_EQ(order(cache), std::string("d,a,c"));
    LRD_CHECK(cache.get("a") != nullptr);   // rescued
    LRD_CHECK(cache.get("b") == nullptr);   // evicted in its place
}

LRD_TEST("a miss does not disturb recency order") {
    Cache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    LRD_CHECK(cache.get("nonexistent") == nullptr);
    LRD_CHECK_EQ(order(cache), std::string("c,b,a"));
}

LRD_TEST("peek does not affect recency") {
    // peek() is the const, non-mutating lookup - the one that could safely run
    // under a shared lock. Its whole reason for existing is that it does NOT do
    // what get() does.
    Cache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    const Cache::ValuePtr peeked = cache.peek("a");
    LRD_REQUIRE(peeked != nullptr);
    LRD_CHECK_EQ(*peeked, std::string("1"));
    LRD_CHECK_EQ(order(cache), std::string("c,b,a"));  // unchanged

    // So "a" is still the least recently used and is still what gets evicted.
    cache.put("d", "4");
    LRD_CHECK(cache.peek("a") == nullptr);
}

LRD_TEST("put on an existing key updates without inserting or evicting") {
    Cache cache(2);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("a", "updated");

    LRD_CHECK_EQ(cache.size(), std::size_t{2});
    LRD_CHECK_EQ(cache.metrics().evictions, std::uint64_t{0});
    LRD_CHECK_EQ(cache.metrics().insertions, std::uint64_t{2});
    LRD_CHECK_EQ(cache.metrics().updates, std::uint64_t{1});
    LRD_CHECK_EQ(value_or(cache.get("a"), "?"), std::string("updated"));
    LRD_CHECK_EQ(order(cache), std::string("a,b"));
}

LRD_TEST("a value handed out survives eviction of its entry") {
    // The reason get() returns shared_ptr<const Value>. The caller's pointer
    // must stay valid even though the cache has since dropped the entry - which
    // in step 5 is the difference between a safe read after unlocking and a
    // use-after-free.
    Cache cache(1);
    cache.put("a", "original");

    const Cache::ValuePtr held = cache.get("a");
    LRD_REQUIRE(held != nullptr);

    cache.put("b", "evicts-a");
    LRD_REQUIRE(cache.get("a") == nullptr);

    // The entry is gone from the cache; the value is still ours.
    LRD_CHECK_EQ(*held, std::string("original"));
}

LRD_TEST("a value handed out is unaffected by a later update") {
    // Updates replace the pointer instead of mutating the pointed-to value, so
    // a reader holding the old pointer keeps a coherent snapshot rather than
    // watching the value change underneath it.
    Cache cache(4);
    cache.put("k", "first");

    const Cache::ValuePtr held = cache.get("k");
    LRD_REQUIRE(held != nullptr);

    cache.put("k", "second");

    LRD_CHECK_EQ(*held, std::string("first"));
    LRD_CHECK_EQ(value_or(cache.get("k"), "?"), std::string("second"));
}

LRD_TEST("erase removes an entry and reports whether it existed") {
    Cache cache(4);
    cache.put("a", "1");
    cache.put("b", "2");

    LRD_CHECK(cache.erase("a"));
    LRD_CHECK(!cache.erase("a"));
    LRD_CHECK_EQ(cache.size(), std::size_t{1});
    LRD_CHECK_EQ(order(cache), std::string("b"));
}

LRD_TEST("erase keeps the index and the list in agreement") {
    // The failure this guards against: erasing from one container but not the
    // other leaves a dangling iterator in the map, and the next access to that
    // key dereferences a freed list node. Refilling to capacity afterwards
    // exercises exactly that path.
    Cache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    LRD_REQUIRE(cache.erase("b"));
    LRD_CHECK_EQ(cache.size(), std::size_t{2});

    cache.put("d", "4");
    LRD_CHECK_EQ(cache.size(), std::size_t{3});
    LRD_CHECK_EQ(order(cache), std::string("d,c,a"));
    LRD_CHECK(cache.get("b") == nullptr);
    LRD_CHECK_EQ(value_or(cache.get("a"), "?"), std::string("1"));
    LRD_CHECK_EQ(value_or(cache.get("c"), "?"), std::string("3"));
}

LRD_TEST("clear empties both containers") {
    Cache cache(4);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.clear();

    LRD_CHECK_EQ(cache.size(), std::size_t{0});
    LRD_CHECK(cache.get("a") == nullptr);
    LRD_CHECK_EQ(order(cache), std::string(""));

    // Must still be usable afterwards.
    cache.put("c", "3");
    LRD_CHECK_EQ(value_or(cache.get("c"), "?"), std::string("3"));
}

LRD_TEST("a capacity-one cache holds exactly one entry") {
    // The degenerate case, where an off-by-one in the eviction check shows up
    // as either holding two entries or holding none.
    Cache cache(1);
    cache.put("a", "1");
    LRD_CHECK_EQ(cache.size(), std::size_t{1});

    cache.put("b", "2");
    LRD_CHECK_EQ(cache.size(), std::size_t{1});
    LRD_CHECK(cache.get("a") == nullptr);
    LRD_CHECK_EQ(value_or(cache.get("b"), "?"), std::string("2"));
}

LRD_TEST("the cache never exceeds its capacity under sustained churn") {
    Cache cache(64);
    for (int i = 0; i < 10000; ++i) {
        cache.put("key-" + std::to_string(i), "value-" + std::to_string(i));
        LRD_REQUIRE(cache.size() <= cache.capacity());
    }

    LRD_CHECK_EQ(cache.size(), std::size_t{64});
    LRD_CHECK_EQ(cache.metrics().insertions, std::uint64_t{10000});
    LRD_CHECK_EQ(cache.metrics().evictions, std::uint64_t{10000 - 64});

    // The 64 most recent keys, and nothing older.
    LRD_CHECK(cache.peek("key-9999") != nullptr);
    LRD_CHECK(cache.peek("key-9936") != nullptr);
    LRD_CHECK(cache.peek("key-9935") == nullptr);
    LRD_CHECK(cache.peek("key-0") == nullptr);
}

LRD_TEST("a repeatedly touched hot key is never evicted") {
    // The property that makes the cache worth having: a working set smaller
    // than capacity should survive an unbounded stream of one-off keys.
    Cache cache(8);
    cache.put("hot", "value");

    for (int i = 0; i < 1000; ++i) {
        cache.put("cold-" + std::to_string(i), "x");
        LRD_REQUIRE(cache.get("hot") != nullptr);
    }

    LRD_CHECK_EQ(value_or(cache.get("hot"), "?"), std::string("value"));
}

LRD_TEST("keys and values may contain arbitrary bytes") {
    Cache cache(4);
    const std::string key("a\0b", 3);
    const std::string value("x\0y", 3);
    cache.put(key, value);

    LRD_CHECK_EQ(value_or(cache.get(key), "?"), value);
    // A NUL-truncating implementation would collide these.
    LRD_CHECK(cache.get("a") == nullptr);
}

LRD_TEST("metrics count hits and misses accurately") {
    Cache cache(2);
    cache.put("a", "1");

    (void)cache.get("a");        // hit
    (void)cache.get("a");        // hit
    (void)cache.get("missing");  // miss

    LRD_CHECK_EQ(cache.metrics().hits, std::uint64_t{2});
    LRD_CHECK_EQ(cache.metrics().misses, std::uint64_t{1});
    // peek() is diagnostic and deliberately does not move the counters.
    (void)cache.peek("a");
    LRD_CHECK_EQ(cache.metrics().hits, std::uint64_t{2});
}

LRD_TEST("the cache works with non-string types") {
    // It is a template; instantiating it with something other than
    // <string,string> is the only way to know it does not accidentally depend
    // on std::string.
    LruCache<int, std::vector<int>> cache(2);
    cache.put(1, {1, 2, 3});
    cache.put(2, {4, 5});

    const auto found = cache.get(1);
    LRD_REQUIRE(found != nullptr);
    LRD_CHECK_EQ(found->size(), std::size_t{3});

    cache.put(3, {6});
    LRD_CHECK(cache.get(2) == nullptr);  // 2 was least recently used
    LRD_CHECK(cache.get(1) != nullptr);
}
