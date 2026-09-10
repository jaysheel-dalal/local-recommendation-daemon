#include "lrd/concurrency/thread_pool.hpp"

#include "lrd/common/log.hpp"

#include <exception>
#include <format>

namespace lrd::concurrency {

const char* to_string(PostStatus status) noexcept {
    switch (status) {
        case PostStatus::Accepted: return "Accepted";
        case PostStatus::QueueFull: return "QueueFull";
        case PostStatus::ShuttingDown: return "ShuttingDown";
    }
    return "Unknown";
}

TaskRejected::TaskRejected(PostStatus status)
    : std::runtime_error(std::format("task rejected: {}", to_string(status))), status_(status) {}

ThreadPool::ThreadPool(std::size_t thread_count, std::size_t max_queued_tasks)
    : max_queued_tasks_(max_queued_tasks) {
    if (thread_count == 0) {
        throw std::invalid_argument("ThreadPool requires at least one thread");
    }
    if (max_queued_tasks == 0) {
        // A zero-length queue would reject every task unless a worker happened
        // to be waiting at that instant - a race dressed up as a configuration.
        throw std::invalid_argument("ThreadPool requires a queue capacity of at least one");
    }

    workers_.reserve(thread_count);
    try {
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        // Spawning threads can fail partway through (EAGAIN under a thread
        // limit). The workers already created are blocked in wait() and their
        // jthread destructors are about to join them - which would deadlock
        // forever, because nothing has told them to stop. So release them
        // first, then let the exception continue.
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        work_available_.notify_all();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    shutdown(ShutdownPolicy::Drain);
}

PostStatus ThreadPool::post(Task task) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);

        if (stopping_) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return PostStatus::ShuttingDown;
        }
        if (queue_.size() >= max_queued_tasks_) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return PostStatus::QueueFull;
        }

        queue_.push_back(std::move(task));
        posted_.fetch_add(1, std::memory_order_relaxed);
    }

    // Notify with the mutex released.
    //
    // If we signalled while still holding it, a woken worker would immediately
    // block trying to acquire the very mutex we hold - the "hurry up and wait"
    // pattern. Modern glibc largely optimises this away with futex requeueing,
    // so the honest framing is that this is a small optimisation rather than a
    // correctness requirement: both orders are correct, this one is not slower.
    work_available_.notify_one();
    return PostStatus::Accepted;
}

void ThreadPool::shutdown(ShutdownPolicy policy) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            // Idempotent: the destructor calls this after an explicit shutdown
            // in the common case, and joining below is safe to repeat because
            // a joined jthread is no longer joinable.
            return;
        }
        stopping_ = true;
        if (policy == ShutdownPolicy::Discard) {
            // Dropped here rather than in the worker loop so the decision lives
            // in one place, and so the memory is released immediately instead of
            // waiting for workers to notice.
            queue_.clear();
        }
    }

    // notify_all, not notify_one: every worker has to observe stopping_, and
    // waking one would leave the rest asleep forever. This is the general rule -
    // notify_one for "there is one more item", notify_all for a state change
    // that every waiter must see.
    work_available_.notify_all();

    for (std::jthread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::worker_loop() {
    for (;;) {
        Task task;

        {
            std::unique_lock<std::mutex> lock(mutex_);

            // The predicate form is not a convenience. A condition variable may
            // wake spuriously - with no notify at all - and it may also be
            // notified after another worker has already taken the only task.
            // Both are ordinary, and both mean the condition must be rechecked
            // in a loop. This overload is that loop.
            work_available_.wait(lock, [this] { return stopping_ || !queue_.empty(); });

            // Drain semantics: a shutting-down pool keeps serving whatever is
            // still queued and only exits once the queue is empty. Discard is
            // implemented by having emptied the queue in shutdown(), so this
            // single condition covers both policies.
            if (queue_.empty()) {
                return;
            }

            task = std::move(queue_.front());
            queue_.pop_front();
        }

        // Outside the lock. Running a task while holding the mutex would
        // serialise every worker behind one task and make the pool a very
        // elaborate single thread.
        try {
            task();
            completed_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            // An exception escaping a thread's entry function calls
            // std::terminate - the whole process dies because one request
            // threw. Catching here is what keeps one bad task from taking the
            // daemon with it, and the worker goes straight back for more work.
            exceptions_.fetch_add(1, std::memory_order_relaxed);
            log_warn("thread pool: task threw: {}", e.what());
        } catch (...) {
            exceptions_.fetch_add(1, std::memory_order_relaxed);
            log_warn("thread pool: task threw a non-std exception");
        }
    }
}

std::size_t ThreadPool::queued() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool ThreadPool::is_shutting_down() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
}

PoolMetrics ThreadPool::metrics() const noexcept {
    // Deliberately not a consistent snapshot: the four counters are read
    // without a lock and a concurrent task may land between two of them. For
    // reporting that is the right trade - a lock here would make every stats
    // call contend with the queue. Anything needing exact agreement has to stop
    // the pool first.
    return PoolMetrics{
        posted_.load(std::memory_order_relaxed),
        completed_.load(std::memory_order_relaxed),
        rejected_.load(std::memory_order_relaxed),
        exceptions_.load(std::memory_order_relaxed),
    };
}

}  // namespace lrd::concurrency
