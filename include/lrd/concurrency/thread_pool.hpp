#pragma once

#include "lrd/concurrency/task.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace lrd::concurrency {

enum class PostStatus : std::uint8_t {
    Accepted,
    QueueFull,      ///< Backpressure: the caller must decide what to do.
    ShuttingDown,   ///< The pool is closed to new work.
};

[[nodiscard]] const char* to_string(PostStatus status) noexcept;

enum class ShutdownPolicy : std::uint8_t {
    Drain,    ///< Run everything already queued, then stop.
    Discard,  ///< Drop queued tasks; workers finish only what is in flight.
};

struct PoolMetrics {
    std::uint64_t posted = 0;
    std::uint64_t completed = 0;
    std::uint64_t rejected = 0;
    std::uint64_t exceptions = 0;
};

/// Thrown by submit() when the pool will not accept the task.
class TaskRejected : public std::runtime_error {
public:
    explicit TaskRejected(PostStatus status);
    [[nodiscard]] PostStatus status() const noexcept { return status_; }

private:
    PostStatus status_;
};

/// A fixed-size pool of worker threads consuming a bounded task queue.
///
/// ## The locking, stated once
///
/// One `std::mutex` guards exactly two things: the task deque and the
/// `stopping_` flag. One `std::condition_variable` wakes workers when either
/// changes. Every worker either holds that mutex briefly to take a task, or
/// holds nothing at all while running one. Tasks never run under the lock -
/// that would serialise the entire pool and defeat its purpose.
///
/// The counters are atomics rather than mutex-protected state, so reading
/// metrics never contends with the queue.
///
/// ## Why the queue is bounded
///
/// An unbounded queue turns a producer that outruns its consumers into
/// unbounded memory growth - the failure arrives later, as an OOM kill far from
/// the cause, instead of at the point where the system was already overloaded.
/// A bounded queue makes overload visible at the moment it happens: post()
/// returns QueueFull and the caller decides. In step 5 the acceptor's answer
/// will be to close the connection rather than pretend it can serve it.
///
/// ## Threading
///
/// post(), submit(), shutdown() and the observers are safe to call from any
/// thread. shutdown() must NOT be called from inside a task running on this
/// pool: it joins the workers, so a worker calling it would join itself.
class ThreadPool {
public:
    /// Both arguments must be at least 1.
    ThreadPool(std::size_t thread_count, std::size_t max_queued_tasks);

    /// Drains: runs everything already queued, then joins. For a pool running
    /// long-lived tasks (step 5's per-connection loops) the caller must arrange
    /// for those tasks to finish first - see shutdown().
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Queues a task. Never blocks.
    ///
    /// Returns QueueFull rather than blocking or growing without bound, so the
    /// caller keeps control of its own overload behaviour.
    [[nodiscard]] PostStatus post(Task task);

    /// Queues a callable and returns a future for its result.
    ///
    /// Throws TaskRejected if the pool will not take it - unlike post(), a
    /// caller waiting on a result has nothing sensible to do with a status
    /// code, because there will be no future to wait on either way.
    ///
    /// std::packaged_task is move-only, which is one of the reasons Task exists.
    template <typename F>
    auto submit(F&& callable) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
        using Result = std::invoke_result_t<std::decay_t<F>>;

        std::packaged_task<Result()> packaged(std::forward<F>(callable));
        std::future<Result> future = packaged.get_future();

        if (const PostStatus status = post(Task(std::move(packaged)));
            status != PostStatus::Accepted) {
            throw TaskRejected(status);
        }
        return future;
    }

    /// Stops accepting work and joins every worker. Idempotent; safe to call
    /// before the destructor does.
    void shutdown(ShutdownPolicy policy = ShutdownPolicy::Drain);

    [[nodiscard]] std::size_t thread_count() const noexcept { return workers_.size(); }
    [[nodiscard]] std::size_t queue_capacity() const noexcept { return max_queued_tasks_; }
    [[nodiscard]] std::size_t queued() const;
    [[nodiscard]] bool is_shutting_down() const;
    [[nodiscard]] PoolMetrics metrics() const noexcept;

private:
    void worker_loop();

    mutable std::mutex mutex_;
    std::condition_variable work_available_;
    std::deque<Task> queue_;
    bool stopping_ = false;

    const std::size_t max_queued_tasks_;

    std::atomic<std::uint64_t> posted_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> exceptions_{0};

    /// std::jthread, not std::thread: its destructor joins, so a throw during
    /// construction or an early return from shutdown() cannot leave a detached
    /// worker running against a half-destroyed pool. We do not use its
    /// stop_token - that would require std::condition_variable_any, which is
    /// noticeably heavier than condition_variable because it must support
    /// arbitrary lockables and allocates for the stop callback. An explicit
    /// stopping_ flag under the mutex is cheaper and makes the shutdown
    /// protocol visible in the code rather than hidden in the library.
    ///
    /// Declared last so that if the constructor throws while spawning workers,
    /// the already-spawned ones are destroyed - and therefore joined - before
    /// the mutex and queue they are using go away.
    std::vector<std::jthread> workers_;
};

}  // namespace lrd::concurrency
