# Concurrency and locking strategy

Written as steps land. Steps 3-5 (this document's current scope) are the cache,
the thread pool, and the concurrent server that connects them. Step 6 measures
the contention and step 7 shards the cache in response.

---

## The trap: a reader-writer lock is the wrong default for an LRU cache

The obvious design, and the one most people reach for:

> Reads outnumber writes in a cache, so use a `std::shared_mutex`: many
> concurrent readers on `get()`, exclusive access on `put()`.

**This is a data race**, and the reasoning that produces it is a good example of
a correct-sounding argument built on a false premise.

The premise is that `get()` is a read. It is not. In an LRU cache, a hit
*reorders the recency list*:

```cpp
ValuePtr get(const Key& key) {
    ...
    entries_.splice(entries_.begin(), entries_, found->second);  // a WRITE
    return entries_.front().value;
}
```

`splice` rewires list pointers. Two threads calling `get()` under a shared lock
would be mutating the same intrusive linked list concurrently, with no
synchronisation between them. The results range from a lost reordering (benign)
to a corrupted list with cycles or lost nodes (a hang or a crash), and it is
timing-dependent enough to pass every test you write and fail in production.

This is why `LruCache::get` is deliberately **not** `const`. The signature is
the warning. `peek()` sits next to it as the operation that genuinely is const —
and it is the one that could safely run under a shared lock.

## The options, and what each actually costs

### 1. `std::mutex`, exclusive for every operation

Correct and trivial to reason about. Every access serialises, so throughput is
capped by one core's worth of critical section no matter how many threads are
serving.

**Chosen for step 5.** It is correct, it is explainable in one sentence, and it
gives step 6 a contention number to measure rather than a guess to defend.

### 2. `std::shared_mutex` with an eviction policy that does not mutate on read

Make reads genuinely read-only, and the reader-writer lock becomes legitimate.
That means giving up exact LRU:

* **CLOCK / second-chance** — each entry has a reference bit that a hit sets.
  Setting a bit is an atomic store to one word, not a structural change, so
  readers do not touch shared structure. Eviction sweeps a hand around the ring
  and reclaims entries whose bit is clear. Approximates LRU closely enough that
  operating system page replacement has used it for decades.
* **Timestamped entries** — a hit stores an atomic counter value into the entry.
  Eviction samples K random entries and evicts the oldest (this is roughly what
  Redis's `allkeys-lru` does). Also approximate, also read-only on the hot path.

Both are more code than step 5 needs and neither is exact LRU. Recorded here as
the road not taken, and as the honest answer to "when *is* a reader-writer lock
right?" — when reads are actually reads.

### 3. Sharding: N independent caches, each with its own mutex

Pick a shard by `hash(key) % N`. Two threads touching different shards never
contend, so throughput scales with shard count until it hits the syscall or
memory bandwidth ceiling.

**Planned for step 7**, after step 6 has measured how much contention there
actually is. The tradeoffs are worth stating up front:

* Eviction becomes per-shard, so "least recently used" is now per-shard rather
  than global. A hot shard evicts entries that a global LRU would have kept.
  With a decent hash and enough keys this is a small effect; with a skewed key
  distribution it is not.
* Global operations (`size()`, `clear()`, a consistent stats snapshot) either
  take every lock in a fixed order or accept an inconsistent view.
* More shards means more memory overhead and worse locality per shard.

### 4. Lock-free

Not attempted, and I would push back on doing so here. A correct lock-free LRU
needs hazard pointers or epoch-based reclamation to answer "when is it safe to
free an evicted node while a reader may still be traversing it". The complexity
is not justified by a workload whose real bottleneck, measured in step 2, is
syscall overhead an order of magnitude larger than any lock.

---

## Why values are `shared_ptr<const Value>`

The return type is chosen for the locked design that arrives in step 5, so it is
worth explaining before the lock exists.

Under a mutex, `get()` must decide what to hand back:

| Return type | Problem |
|---|---|
| `const Value&` | Dangles as soon as the lock is released — or sooner, if another thread evicts the entry. |
| `Value` (by value) | Copies the entire payload *while holding the lock*, lengthening the critical section by exactly the amount of work that hurts most. |
| `shared_ptr<const Value>` | Copies one refcounted pointer under the lock. The value stays alive as long as the caller holds it. |

The `const` in `shared_ptr<const Value>` is doing real work too. Values are
immutable once published, so an update *replaces the pointer* rather than
mutating the object a reader may be holding:

```cpp
found->second->value = std::move(value);  // swap the pointer, don't touch the old value
```

A reader that grabbed the previous pointer keeps a coherent older snapshot. It
never observes a half-written value. This is the same reasoning behind
copy-on-write and RCU, at a much smaller scale.

The cost is an allocation per `put` (the control block plus the value, fused
into one by `make_shared`) and an atomic increment per `get`. Both are cheap
against a critical section, and the atomic refcount is uncontended in the common
case where each value has one holder.

---

## What is not yet decided

Deliberately left open until there are measurements to decide with:

* Shard count for step 7 — to be chosen from the step 6 contention curve, not
  guessed.

Step 5 resolved the second open question: the pool queues **connections**, and
the starvation that implies is documented on `Server` rather than hidden.

---

## The thread pool's locking (step 4)

One `std::mutex` guards exactly two things: the task deque and the `stopping_`
flag. One `std::condition_variable` signals changes to either. That is the
entire synchronisation surface, and keeping it that small is deliberate — a pool
with three mutexes is a pool nobody can reason about.

Two rules do most of the work:

**Tasks never run under the lock.** A worker holds the mutex only long enough to
pop a task, then releases it and runs the task with nothing held. Running a task
under the lock would serialise every worker behind it and make the pool an
elaborate single thread.

**Wait with a predicate, always.**

```cpp
work_available_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
```

A condition variable can wake with no notify at all (spurious wakeup), and it
can also be woken legitimately only for another worker to take the only task
first. Both are ordinary, and both mean the condition must be rechecked in a
loop. The predicate overload *is* that loop — it is not sugar.

### notify_one versus notify_all

`post()` uses `notify_one`: one new task, one worker needs waking. `shutdown()`
uses `notify_all`: every worker must observe `stopping_`, and waking one would
leave the rest asleep forever. That is the general rule — `notify_one` for "one
more item available", `notify_all` for a state change every waiter must see.

`post()` notifies *after* releasing the mutex. Signalling while holding it means
the woken thread immediately blocks on the mutex we still hold — "hurry up and
wait". Modern glibc largely optimises this away with futex requeueing, so the
honest framing is that this is a small optimisation and not a correctness
requirement. Both orders are correct.

### Shutdown, and the deadlock that isn't obvious

`shutdown()` sets the flag, optionally clears the queue, notifies everyone, and
joins. The subtle case is the *constructor*: if spawning the fourth of eight
threads fails with `EAGAIN`, the three already running are blocked in `wait()`,
and the `jthread` destructors are about to join them — forever, because nothing
ever told them to stop. So the constructor catches, sets `stopping_`, notifies,
and only then rethrows.

`std::jthread` rather than `std::thread` for exactly this reason: its destructor
joins, so no path out of the pool can leave a worker running against a
half-destroyed object. We do not use its `stop_token`, because that needs
`std::condition_variable_any`, which is heavier than `condition_variable` (it
must support arbitrary lockables and allocates for the stop callback). An
explicit flag is cheaper and puts the shutdown protocol in the code rather than
in the library.

One documented restriction: `shutdown()` must not be called from a task running
on the pool, because a worker would join itself.

### Exceptions

An exception escaping a thread's entry function calls `std::terminate` — the
whole daemon dies because one request threw. The worker loop catches everything,
counts it, logs it, and goes straight back for more work. `submit()` is the
deliberate exception to that: an exception belonging to whoever is waiting on
the future is delivered through the future rather than swallowed.

### Metrics are deliberately not a consistent snapshot

The four counters are atomics read without a lock, so a concurrent task can land
between two reads. For reporting that is the right trade — a lock would make
every stats call contend with the queue. Anything needing exact agreement has to
stop the pool first, and the test that checks totals does exactly that.

---

## Validating the sanitizer, not just running it

"TSan clean" only means something if TSan would have caught a race in this code
had one existed. `scripts/verify-tsan.sh` is the negative control: it builds a
program that posts 2000 tasks incrementing a plain unsynchronised `int` across
eight workers, and asserts that ThreadSanitizer reports the race. It does:

```
WARNING: ThreadSanitizer: data race (pid=41268)
  Read of size 4 at 0x7fffffffe118 by thread T8:
    #0 operator() race.cpp:7
    #1 invoke include/lrd/concurrency/task.hpp:95
```

That is what makes the clean runs on the real suite evidence rather than
decoration.

---

## The server's shutdown path (step 5)

Three separate problems, three separate mechanisms. Each exists because the
obvious approach does not work.

### Waking an acceptor blocked in accept()

Setting a `stop` flag does nothing: the thread is inside the kernel and will
stay there until a client connects. So the acceptor does not call `accept()`
directly - it `poll()`s two descriptors, the listening socket and the read end
of a pipe, and only calls `accept()` when the listener is actually ready.

Stopping means writing one byte to that pipe. This is the **self-pipe trick**,
and its point is that a signal handler may only call async-signal-safe
functions - which excludes essentially the whole standard library: no
allocation, no locks, no `printf`, no `std::format`. `write()` is on the safe
list. So the handler does exactly one thing:

```cpp
void handle_stop_signal(int) {
    const int saved_errno = errno;
    if (Server* s = g_signal_target.load(std::memory_order_relaxed)) {
        s->request_stop();          // one write() to the pipe
    }
    errno = saved_errno;
}
```

Saving and restoring `errno` matters. The handler interrupted a thread that may
have been between a failing syscall and its check of `errno`; clobbering it
would make that thread diagnose a completely unrelated failure.

The file-scope pointer is a `std::atomic<Server*>` with a `static_assert` on
`is_always_lock_free`. A signal handler may only touch a lock-free atomic - a
lock-based one could deadlock against the very thread it interrupted.

Linux has `signalfd`, which is tidier. The pipe is used instead because it works
identically on macOS and the BSDs.

### Waking a worker blocked reading a quiet client

A worker serving a connection spends nearly all its time in `read()`, waiting
for a request that may never come. No flag reaches it either. `ConnectionRegistry`
holds the descriptor of every live connection, and shutdown calls
`shutdown(fd, SHUT_RDWR)` on each - which makes that blocked `read()` return 0,
exactly as if the client had hung up, a case the connection loop already
handles.

**`shutdown()`, not `close()`.** Closing a descriptor another thread is actively
using is a use-after-free with extra steps: the number can be reissued by the
kernel to an unrelated file the instant it is freed, and the blocked thread
would then be reading someone else's socket. `shutdown()` breaks the connection
while leaving the descriptor owned by its `Fd`, which closes it normally
afterwards.

The registry mutex is deliberately held across the `shutdown()` syscall, and
connections deregister *before* their `Fd` closes. That is what guarantees
`stop_all()` can never be looking at a number that has already been recycled.

### The connection that has not started yet

A connection can be popped off the pool's queue microseconds after `stop_all()`
has already walked the list. The registry therefore latches closed: `add()`
returns 0 once stopping has begun, and the worker abandons that connection
immediately. Combined with `ShutdownPolicy::Discard`, whose dropped tasks
destroy their captured `UnixStream`s and close those sockets, every connection
is accounted for.

### Order of operations

```cpp
listener_.close();      // 1. no new clients, socket file removed
registry_.stop_all();   // 2. break live connections, latch the registry closed
pool_.shutdown(Discard) // 3. drop queued connections, join workers
```

`Discard` rather than `Drain`: a queued task here is an unserved connection, not
useful work, and running it would only mean greeting a client we are about to
disconnect.

## A bug worth recording

The self-pipe was originally two members, `Fd stop_read_` and `Fd stop_write_`,
initialised from a helper taking the write end by reference:

```cpp
stop_read_(make_pipe(stop_write_)),   // silently broken
```

This compiles cleanly and is wrong. **Members are initialised in declaration
order, regardless of the order the constructor's init-list is written in.**
`stop_read_` is declared first, so `make_pipe` filled in `stop_write_` before
`stop_write_`'s own initialisation had run - and its default member initialiser
then overwrote the descriptor with -1.

The symptom was not a crash. Every write to the stop pipe failed silently, the
acceptor never woke, and shutdown hung forever - which is how it was found: the
server test suite timed out at 60 seconds with the first case never returning.

The fix is structural rather than a reordering: both ends now live in one struct
returned by value, so there is no second member to initialise out of order and
the hazard cannot recur.
