#include "lrd/concurrency/thread_pool.hpp"

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace lrd::concurrency;
using namespace std::chrono_literals;

namespace {

/// Blocks until `counter` reaches `target`, or the deadline passes.
///
/// Returns false on timeout rather than looping forever: a test that fails
/// should fail, not hang until CTest's 60-second timeout kills it and tells you
/// nothing about which assertion was involved.
bool wait_for_count(const std::atomic<int>& counter, int target,
                    std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (counter.load() < target) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

}  // namespace

// --------------------------------------------------------------------------
// Task: the move-only, type-erased callable the queue holds.
// --------------------------------------------------------------------------

LRD_TEST("Task wraps a plain lambda") {
    int called = 0;
    Task task([&called] { ++called; });
    LRD_REQUIRE(static_cast<bool>(task));
    task();
    LRD_CHECK_EQ(called, 1);
}

LRD_TEST("Task carries a move-only capture") {
    // The reason Task exists. std::function would not compile against this,
    // because a lambda capturing a unique_ptr is not copy-constructible - and
    // in step 5 the capture is a UnixStream, which owns a move-only Fd.
    auto owned = std::make_unique<int>(42);
    int observed = 0;

    Task task([captured = std::move(owned), &observed]() mutable { observed = *captured; });
    task();

    LRD_CHECK_EQ(observed, 42);
}

LRD_TEST("a default-constructed Task is empty") {
    const Task task;
    LRD_CHECK(!static_cast<bool>(task));
}

LRD_TEST("moving a Task transfers the callable and empties the source") {
    int called = 0;
    Task source([&called] { ++called; });
    Task sink(std::move(source));

    LRD_CHECK(static_cast<bool>(sink));
    LRD_CHECK(!static_cast<bool>(source));  // NOLINT(bugprone-use-after-move)
    sink();
    LRD_CHECK_EQ(called, 1);
}

// --------------------------------------------------------------------------
// Construction
// --------------------------------------------------------------------------

LRD_TEST("a pool with no threads or no queue is rejected") {
    bool threw_threads = false;
    bool threw_queue = false;
    try {
        const ThreadPool pool(0, 16);
    } catch (const std::invalid_argument&) {
        threw_threads = true;
    }
    try {
        const ThreadPool pool(2, 0);
    } catch (const std::invalid_argument&) {
        threw_queue = true;
    }
    LRD_CHECK(threw_threads);
    LRD_CHECK(threw_queue);
}

// --------------------------------------------------------------------------
// Execution
// --------------------------------------------------------------------------

LRD_TEST("every posted task runs exactly once") {
    constexpr int kTasks = 2000;
    std::atomic<int> counter{0};

    {
        ThreadPool pool(8, kTasks);
        for (int i = 0; i < kTasks; ++i) {
            LRD_REQUIRE(pool.post([&counter] { counter.fetch_add(1); }) == PostStatus::Accepted);
        }
        // The destructor drains, so by the time this scope ends every task has
        // run. That is the property being asserted below.
    }

    LRD_CHECK_EQ(counter.load(), kTasks);
}

LRD_TEST("work actually runs on several threads at once") {
    // A pool that quietly ran everything on one thread would pass every other
    // test in this file. This one makes genuine parallelism a requirement: each
    // task blocks until all four have started, so it can only complete if four
    // are running concurrently.
    constexpr int kWorkers = 4;
    ThreadPool pool(kWorkers, 64);

    std::atomic<int> started{0};
    std::mutex mutex;
    std::set<std::thread::id> thread_ids;
    std::atomic<bool> all_started{false};

    for (int i = 0; i < kWorkers; ++i) {
        LRD_REQUIRE(pool.post([&] {
            {
                const std::lock_guard<std::mutex> lock(mutex);
                thread_ids.insert(std::this_thread::get_id());
            }
            started.fetch_add(1);
            // Spin until every worker has arrived - a serial pool deadlocks
            // here and the wait below times out.
            while (!all_started.load() && started.load() < kWorkers) {
                std::this_thread::sleep_for(1ms);
            }
            all_started.store(true);
        }) == PostStatus::Accepted);
    }

    LRD_REQUIRE(wait_for_count(started, kWorkers));
    pool.shutdown();

    const std::lock_guard<std::mutex> lock(mutex);
    LRD_CHECK_EQ(thread_ids.size(), std::size_t{kWorkers});
}

LRD_TEST("a single-threaded pool preserves submission order") {
    // FIFO is only guaranteed with one worker; with several, tasks start in
    // order but finish in whatever order they finish. Asserting order on a
    // multi-threaded pool would be asserting a coincidence.
    ThreadPool pool(1, 128);
    std::mutex mutex;
    std::vector<int> order;

    for (int i = 0; i < 50; ++i) {
        LRD_REQUIRE(pool.post([&, i] {
            const std::lock_guard<std::mutex> lock(mutex);
            order.push_back(i);
        }) == PostStatus::Accepted);
    }
    pool.shutdown();

    LRD_REQUIRE(order.size() == 50);
    bool sorted = true;
    for (int i = 0; i < 50; ++i) {
        if (order[static_cast<std::size_t>(i)] != i) {
            sorted = false;
        }
    }
    LRD_CHECK(sorted);
}

// --------------------------------------------------------------------------
// Backpressure
// --------------------------------------------------------------------------

LRD_TEST("a full queue is reported rather than grown") {
    // One worker, held busy, and a queue of two. The fourth post has nowhere to
    // go, and the pool says so instead of allocating - which is what turns
    // overload into a decision the caller makes rather than an OOM later.
    ThreadPool pool(1, 2);

    std::mutex gate;
    std::unique_lock<std::mutex> held(gate);
    std::atomic<int> ran{0};

    // Occupies the only worker until `gate` is released.
    LRD_REQUIRE(pool.post([&] {
        const std::lock_guard<std::mutex> lock(gate);
        ran.fetch_add(1);
    }) == PostStatus::Accepted);

    // Give the worker a moment to pick that task up so the queue is genuinely
    // empty behind it.
    std::this_thread::sleep_for(50ms);

    LRD_CHECK(pool.post([&] { ran.fetch_add(1); }) == PostStatus::Accepted);
    LRD_CHECK(pool.post([&] { ran.fetch_add(1); }) == PostStatus::Accepted);
    LRD_CHECK(pool.post([&] { ran.fetch_add(1); }) == PostStatus::QueueFull);

    LRD_CHECK_EQ(pool.metrics().rejected, std::uint64_t{1});

    held.unlock();
    pool.shutdown();
    LRD_CHECK_EQ(ran.load(), 3);
}

// --------------------------------------------------------------------------
// Shutdown
// --------------------------------------------------------------------------

LRD_TEST("shutdown drains what is already queued") {
    constexpr int kTasks = 200;
    std::atomic<int> counter{0};

    ThreadPool pool(2, kTasks);
    for (int i = 0; i < kTasks; ++i) {
        LRD_REQUIRE(pool.post([&counter] { counter.fetch_add(1); }) == PostStatus::Accepted);
    }

    pool.shutdown(ShutdownPolicy::Drain);
    LRD_CHECK_EQ(counter.load(), kTasks);
}

LRD_TEST("shutdown with Discard drops what has not started") {
    ThreadPool pool(1, 256);

    std::mutex gate;
    std::unique_lock<std::mutex> held(gate);
    std::atomic<int> counter{0};

    // Pin the single worker.
    LRD_REQUIRE(pool.post([&] {
        const std::lock_guard<std::mutex> lock(gate);
        counter.fetch_add(1);
    }) == PostStatus::Accepted);
    std::this_thread::sleep_for(50ms);

    for (int i = 0; i < 100; ++i) {
        LRD_REQUIRE(pool.post([&counter] { counter.fetch_add(1); }) == PostStatus::Accepted);
    }

    // Release the worker and discard concurrently; either way the 100 queued
    // tasks must not run.
    held.unlock();
    pool.shutdown(ShutdownPolicy::Discard);

    // Only the in-flight task is allowed to have completed.
    LRD_CHECK(counter.load() <= 1);
}

LRD_TEST("posting after shutdown is refused") {
    ThreadPool pool(2, 16);
    pool.shutdown();

    std::atomic<int> counter{0};
    LRD_CHECK(pool.post([&counter] { counter.fetch_add(1); }) == PostStatus::ShuttingDown);
    LRD_CHECK_EQ(counter.load(), 0);
    LRD_CHECK(pool.is_shutting_down());
}

LRD_TEST("shutdown is idempotent") {
    ThreadPool pool(2, 16);
    pool.shutdown();
    pool.shutdown();
    pool.shutdown(ShutdownPolicy::Discard);
    // And the destructor runs it once more on the way out.
    LRD_CHECK(pool.is_shutting_down());
}

LRD_TEST("an idle pool shuts down promptly rather than waiting for work") {
    // Guards against a worker that only rechecks stopping_ after a task
    // arrives: with no task ever arriving, shutdown would block forever.
    ThreadPool pool(4, 16);
    const auto started = std::chrono::steady_clock::now();
    pool.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    LRD_CHECK(elapsed < 2s);
}

// --------------------------------------------------------------------------
// Exceptions
// --------------------------------------------------------------------------

LRD_TEST("a task that throws does not kill its worker") {
    // An exception escaping a thread's entry function calls std::terminate -
    // the entire daemon would die because one request threw. The worker must
    // absorb it and go back for more work.
    ThreadPool pool(1, 64);

    std::atomic<int> completed{0};
    LRD_REQUIRE(pool.post([] { throw std::runtime_error("boom"); }) == PostStatus::Accepted);
    LRD_REQUIRE(pool.post([] { throw 42; }) == PostStatus::Accepted);  // non-std exception
    for (int i = 0; i < 10; ++i) {
        LRD_REQUIRE(pool.post([&completed] { completed.fetch_add(1); }) == PostStatus::Accepted);
    }

    pool.shutdown();

    LRD_CHECK_EQ(completed.load(), 10);
    LRD_CHECK_EQ(pool.metrics().exceptions, std::uint64_t{2});
    // The throwing tasks are counted as exceptions, not completions.
    LRD_CHECK_EQ(pool.metrics().completed, std::uint64_t{10});
}

// --------------------------------------------------------------------------
// submit() and futures
// --------------------------------------------------------------------------

LRD_TEST("submit returns the task's result through a future") {
    ThreadPool pool(2, 16);
    std::future<int> answer = pool.submit([] { return 6 * 7; });
    LRD_CHECK_EQ(answer.get(), 42);
}

LRD_TEST("submit propagates an exception through the future") {
    // The exception belongs to whoever is waiting on the result, so unlike a
    // posted task it must not be swallowed by the worker.
    ThreadPool pool(2, 16);
    std::future<int> answer = pool.submit([]() -> int { throw std::runtime_error("nope"); });

    bool threw = false;
    try {
        (void)answer.get();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    LRD_CHECK(threw);
    // And it was delivered to the caller rather than logged as an escape.
    LRD_CHECK_EQ(pool.metrics().exceptions, std::uint64_t{0});
}

LRD_TEST("submit on a shut-down pool throws TaskRejected") {
    ThreadPool pool(2, 16);
    pool.shutdown();

    bool threw = false;
    try {
        auto future = pool.submit([] { return 1; });
    } catch (const TaskRejected& e) {
        threw = true;
        LRD_CHECK(e.status() == PostStatus::ShuttingDown);
    }
    LRD_CHECK(threw);
}

LRD_TEST("submit handles a void-returning callable") {
    ThreadPool pool(2, 16);
    std::atomic<int> counter{0};
    std::future<void> done = pool.submit([&counter] { counter.fetch_add(1); });
    done.get();
    LRD_CHECK_EQ(counter.load(), 1);
}

// --------------------------------------------------------------------------
// Concurrency stress. This is the case ThreadSanitizer is here for.
// --------------------------------------------------------------------------

LRD_TEST("many producers and workers agree on the total") {
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 500;
    constexpr int kExpected = kProducers * kPerProducer;

    std::atomic<int> counter{0};
    std::atomic<int> rejected{0};

    {
        ThreadPool pool(8, 4096);
        std::vector<std::jthread> producers;
        producers.reserve(kProducers);

        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&] {
                for (int i = 0; i < kPerProducer; ++i) {
                    // Retry on backpressure so the total stays exact - the
                    // queue is deliberately smaller than the total work.
                    while (pool.post([&counter] { counter.fetch_add(1); }) !=
                           PostStatus::Accepted) {
                        rejected.fetch_add(1);
                        std::this_thread::sleep_for(100us);
                    }
                }
            });
        }
        // producers join here, then the pool destructor drains.
    }

    LRD_CHECK_EQ(counter.load(), kExpected);
}

LRD_TEST("metrics account for every task") {
    constexpr int kTasks = 500;
    ThreadPool pool(4, kTasks);

    for (int i = 0; i < kTasks; ++i) {
        LRD_REQUIRE(pool.post([] {}) == PostStatus::Accepted);
    }
    pool.shutdown();

    const PoolMetrics metrics = pool.metrics();
    LRD_CHECK_EQ(metrics.posted, std::uint64_t{kTasks});
    LRD_CHECK_EQ(metrics.completed, std::uint64_t{kTasks});
    LRD_CHECK_EQ(metrics.rejected, std::uint64_t{0});
    LRD_CHECK_EQ(metrics.exceptions, std::uint64_t{0});
}
