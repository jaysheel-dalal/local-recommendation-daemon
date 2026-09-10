#include "lrd/cache/sharded_cache.hpp"

#include "test_harness.hpp"

#include <algorithm>
#include <atomic>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using lrd::cache::ShardedCache;

namespace {

using Cache = ShardedCache<std::string, std::string>;
using IntCache = ShardedCache<int, int>;

std::string value_for(const std::string& key) {
    return "value-for-" + key;
}

}  // namespace

// --------------------------------------------------------------------------
// Construction
// --------------------------------------------------------------------------

LRD_TEST("shard count must be a power of two") {
    // The mask trick (hash & (n-1)) is only equivalent to hash % n when n is a
    // power of two. Accepting 6 shards would silently route keys to shards 0-3
    // and leave two permanently empty.
    for (const std::size_t bad : {std::size_t{0}, std::size_t{3}, std::size_t{6},
                                  std::size_t{100}}) {
        bool threw = false;
        try {
            const Cache cache(1024, bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        LRD_CHECK(threw);
    }

    for (const std::size_t good : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                                   std::size_t{64}}) {
        const Cache cache(1024, good);
        LRD_CHECK_EQ(cache.shard_count(), good);
    }
}

LRD_TEST("capacity below the shard count is rejected") {
    bool threw = false;
    try {
        const Cache cache(4, 16);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    LRD_CHECK(threw);
}

// --------------------------------------------------------------------------
// Behaviour matches the unsharded cache
// --------------------------------------------------------------------------

LRD_TEST("a single shard behaves exactly like the locked cache") {
    // The property that makes the benchmark comparison fair: shard_count == 1
    // is not a special case, it is the baseline.
    Cache cache(3, 1);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");
    LRD_REQUIRE(cache.get("a") != nullptr);  // rescue from eviction
    cache.put("d", "4");

    LRD_CHECK(cache.get("b") == nullptr);
    LRD_CHECK(cache.get("a") != nullptr);
    LRD_CHECK_EQ(cache.size(), std::size_t{3});
}

LRD_TEST("put and get round-trip across many shards") {
    Cache cache(1024, 16);
    for (int i = 0; i < 500; ++i) {
        const std::string key = "key-" + std::to_string(i);
        cache.put(key, value_for(key));
    }
    for (int i = 0; i < 500; ++i) {
        const std::string key = "key-" + std::to_string(i);
        const auto found = cache.get(key);
        LRD_REQUIRE(found != nullptr);
        LRD_CHECK_EQ(*found, value_for(key));
    }
    LRD_CHECK_EQ(cache.size(), std::size_t{500});
}

LRD_TEST("erase removes from the right shard") {
    Cache cache(1024, 8);
    cache.put("a", "1");
    cache.put("b", "2");

    LRD_CHECK(cache.erase("a"));
    LRD_CHECK(!cache.erase("a"));
    LRD_CHECK(cache.get("a") == nullptr);
    LRD_CHECK(cache.get("b") != nullptr);
}

LRD_TEST("clear empties every shard") {
    Cache cache(1024, 8);
    for (int i = 0; i < 200; ++i) {
        cache.put("k" + std::to_string(i), "v");
    }
    LRD_REQUIRE(cache.size() == 200);

    cache.clear();
    LRD_CHECK_EQ(cache.size(), std::size_t{0});
    for (const std::size_t shard_size : cache.shard_sizes()) {
        LRD_CHECK_EQ(shard_size, std::size_t{0});
    }
}

// --------------------------------------------------------------------------
// Distribution - the part that makes sharding work or quietly not work
// --------------------------------------------------------------------------

LRD_TEST("a key always maps to the same shard") {
    const Cache cache(1024, 16);
    for (int i = 0; i < 1000; ++i) {
        const std::string key = "key-" + std::to_string(i);
        LRD_REQUIRE(cache.shard_index(key) == cache.shard_index(key));
    }
}

LRD_TEST("string keys spread evenly across shards") {
    constexpr std::size_t kShards = 16;
    constexpr int kKeys = 16000;
    const Cache cache(32768, kShards);

    std::vector<std::size_t> counts(kShards, 0);
    for (int i = 0; i < kKeys; ++i) {
        ++counts[cache.shard_index("key-" + std::to_string(i))];
    }

    // Perfect balance would be 1000 per shard. Allow a generous band - this is
    // checking that the distribution is not pathological, not that the hash is
    // cryptographic.
    const auto [lowest, highest] = std::minmax_element(counts.begin(), counts.end());
    LRD_CHECK(*lowest > 700);
    LRD_CHECK(*highest < 1300);
}

LRD_TEST("sequential integer keys spread evenly despite an identity hash") {
    // The trap this guards against. std::hash<int> in libstdc++ is the identity
    // function, so masking the low bits of an unmixed hash would put every
    // multiple of 16 into shard 0 - and with sequential keys, every shard would
    // receive exactly the keys congruent to its index, which for strided access
    // patterns collapses onto a few shards.
    //
    // Without the splitmix64 finalizer this test fails: shard_index(i) would
    // simply be i % 16.
    constexpr std::size_t kShards = 16;
    const IntCache cache(32768, kShards);

    std::vector<std::size_t> counts(kShards, 0);
    for (int i = 0; i < 16000; ++i) {
        ++counts[cache.shard_index(i)];
    }

    const auto [lowest, highest] = std::minmax_element(counts.begin(), counts.end());
    LRD_CHECK(*lowest > 700);
    LRD_CHECK(*highest < 1300);

    // And the specific pathology: keys 0, 16, 32, ... must not all land
    // together, which is exactly what an unmixed identity hash would do.
    std::set<std::size_t> strided_shards;
    for (int i = 0; i < 64; ++i) {
        strided_shards.insert(cache.shard_index(i * static_cast<int>(kShards)));
    }
    LRD_CHECK(strided_shards.size() > 1);
}

// --------------------------------------------------------------------------
// Capacity and eviction
// --------------------------------------------------------------------------

LRD_TEST("total capacity is respected under churn") {
    constexpr std::size_t kCapacity = 256;
    constexpr std::size_t kShards = 8;
    Cache cache(kCapacity, kShards);

    for (int i = 0; i < 20000; ++i) {
        cache.put("key-" + std::to_string(i), "v");
        LRD_REQUIRE(cache.size() <= kCapacity);
    }
    LRD_CHECK_EQ(cache.size(), kCapacity);
}

LRD_TEST("eviction is per-shard, and that is visible") {
    // The documented cost of sharding. Every key here hashes into one shard's
    // worth of capacity, so a shard fills and evicts while the cache as a whole
    // is nowhere near full. A global LRU would have kept these entries.
    constexpr std::size_t kCapacity = 64;
    constexpr std::size_t kShards = 8;  // 8 entries per shard
    Cache cache(kCapacity, kShards);

    // Find enough keys that share one shard to overflow it.
    const std::size_t target_shard = cache.shard_index("probe");
    std::vector<std::string> same_shard;
    for (int i = 0; same_shard.size() < 12 && i < 100000; ++i) {
        const std::string key = "k" + std::to_string(i);
        if (cache.shard_index(key) == target_shard) {
            same_shard.push_back(key);
        }
    }
    LRD_REQUIRE(same_shard.size() == 12);

    for (const std::string& key : same_shard) {
        cache.put(key, "v");
    }

    // 12 keys into 8 slots: 4 evicted, even though the cache holds 8 of 64.
    LRD_CHECK_EQ(cache.size(), std::size_t{8});
    LRD_CHECK(cache.get(same_shard[0]) == nullptr);
    LRD_CHECK(cache.get(same_shard[11]) != nullptr);
    LRD_CHECK_EQ(cache.metrics().evictions, std::uint64_t{4});
}

// --------------------------------------------------------------------------
// Concurrency
// --------------------------------------------------------------------------

LRD_TEST("concurrent access keeps every value matched to its key") {
    constexpr int kThreads = 8;
    constexpr int kOps = 4000;
    constexpr int kKeys = 500;

    Cache cache(256, 16);
    std::atomic<int> mismatches{0};
    std::atomic<int> hits{0};

    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kOps; ++i) {
                    const std::string key = "key-" + std::to_string((i * 7 + t) % kKeys);
                    if (i % 3 == 0) {
                        cache.put(key, value_for(key));
                    } else if (const auto found = cache.get(key); found != nullptr) {
                        hits.fetch_add(1);
                        if (*found != value_for(key)) {
                            mismatches.fetch_add(1);
                        }
                    }
                }
            });
        }
    }

    LRD_CHECK_EQ(mismatches.load(), 0);
    LRD_CHECK(hits.load() > 0);
    LRD_CHECK(cache.size() <= cache.capacity());
}

LRD_TEST("a value handed out survives eviction of its entry") {
    Cache cache(8, 8);  // one entry per shard
    cache.put("pinned", "original");
    const auto held = cache.get("pinned");
    LRD_REQUIRE(held != nullptr);

    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 500; ++i) {
                    cache.put("churn-" + std::to_string(t) + "-" + std::to_string(i), "x");
                }
            });
        }
    }

    LRD_CHECK_EQ(*held, std::string("original"));
}

LRD_TEST("metrics sum consistently across shards") {
    Cache cache(128, 8);
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 2000; ++i) {
                    cache.put("k-" + std::to_string((i + t * 500) % 1000), "v");
                }
            });
        }
    }

    const auto metrics = cache.metrics();
    LRD_CHECK_EQ(metrics.insertions - metrics.evictions, cache.size());
}
