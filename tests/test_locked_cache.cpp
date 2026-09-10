#include "lrd/cache/locked_cache.hpp"

#include "test_harness.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using lrd::cache::LockedCache;

namespace {

using Cache = LockedCache<std::string, std::string>;

/// Values are derived from their key, so any reader can verify that what came
/// back belongs to what was asked for. A torn or mismatched value is then a
/// detectable failure rather than something that merely looks plausible.
std::string value_for(const std::string& key) {
    return "value-for-" + key + "-" + std::string(64, 'x');
}

}  // namespace

LRD_TEST("single-threaded behaviour is unchanged by the lock") {
    Cache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");
    LRD_REQUIRE(cache.get("a") != nullptr);  // rescue "a" from eviction
    cache.put("d", "4");

    LRD_CHECK(cache.get("b") == nullptr);
    LRD_CHECK(cache.get("a") != nullptr);
    LRD_CHECK_EQ(cache.size(), std::size_t{3});
    LRD_CHECK_EQ(cache.capacity(), std::size_t{3});
}

LRD_TEST("concurrent readers and writers never lose a value's integrity") {
    // The core ThreadSanitizer target. Eight threads hammer a cache far smaller
    // than their key space, so evictions, updates and lookups all race
    // constantly. Every value is derived from its key, so a value that comes
    // back attached to the wrong key - or half-written - is caught.
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 4000;
    constexpr int kKeys = 200;

    Cache cache(64);
    std::atomic<int> mismatches{0};
    std::atomic<int> hits{0};

    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);

        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kOpsPerThread; ++i) {
                    const std::string key = "key-" + std::to_string((i * 7 + t) % kKeys);

                    if (i % 3 == 0) {
                        cache.put(key, value_for(key));
                    } else {
                        if (const auto found = cache.get(key); found != nullptr) {
                            hits.fetch_add(1);
                            if (*found != value_for(key)) {
                                mismatches.fetch_add(1);
                            }
                        }
                    }
                }
            });
        }
    }

    LRD_CHECK_EQ(mismatches.load(), 0);
    LRD_CHECK(hits.load() > 0);  // the test would be vacuous if nothing ever hit
    LRD_CHECK(cache.size() <= cache.capacity());
}

LRD_TEST("capacity is never exceeded under concurrent insertion") {
    // Eviction is the operation most likely to break under concurrency: it
    // touches both the list and the map, and getting the order wrong leaves
    // them disagreeing. Sampling size() throughout catches an overshoot.
    constexpr int kThreads = 8;
    constexpr std::size_t kCapacity = 32;

    Cache cache(kCapacity);
    std::atomic<int> overshoots{0};

    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 2000; ++i) {
                    cache.put("k-" + std::to_string(t) + "-" + std::to_string(i), "v");
                    if (cache.size() > kCapacity) {
                        overshoots.fetch_add(1);
                    }
                }
            });
        }
    }

    LRD_CHECK_EQ(overshoots.load(), 0);
    LRD_CHECK_EQ(cache.size(), kCapacity);
}

LRD_TEST("a value stays valid after the entry it came from is evicted") {
    // The property the shared_ptr return type exists for, under concurrency:
    // one thread holds a value while others churn the cache hard enough to
    // evict it many times over.
    Cache cache(4);
    cache.put("pinned", "original-value");

    const auto held = cache.get("pinned");
    LRD_REQUIRE(held != nullptr);

    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 1000; ++i) {
                    cache.put("churn-" + std::to_string(t) + "-" + std::to_string(i), "x");
                }
            });
        }
    }

    LRD_CHECK(cache.get("pinned") == nullptr);          // long since evicted
    LRD_CHECK_EQ(*held, std::string("original-value"));  // still ours
}

LRD_TEST("concurrent erase and get agree") {
    constexpr int kThreads = 6;
    Cache cache(128);
    for (int i = 0; i < 100; ++i) {
        cache.put("k-" + std::to_string(i), value_for("k-" + std::to_string(i)));
    }

    std::atomic<int> mismatches{0};
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 2000; ++i) {
                    const std::string key = "k-" + std::to_string(i % 100);
                    if (t % 2 == 0) {
                        (void)cache.erase(key);
                    } else {
                        if (const auto found = cache.get(key);
                            found != nullptr && *found != value_for(key)) {
                            mismatches.fetch_add(1);
                        }
                        cache.put(key, value_for(key));
                    }
                }
            });
        }
    }
    LRD_CHECK_EQ(mismatches.load(), 0);
}

LRD_TEST("metrics remain internally plausible under load") {
    Cache cache(16);
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 1000; ++i) {
                    cache.put("k-" + std::to_string((i + t) % 64), "v");
                    (void)cache.get("k-" + std::to_string(i % 64));
                }
            });
        }
    }

    const auto metrics = cache.metrics();
    // Every insertion beyond capacity must have cost exactly one eviction.
    LRD_CHECK_EQ(metrics.insertions - metrics.evictions, cache.size());
}
