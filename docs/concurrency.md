# Concurrency and locking strategy

Written as steps land. Step 3 (this document's current scope) is the cache
itself, single-threaded. Steps 4–5 add the thread pool and the locking; step 7
shards the cache and measures the result.

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
* Whether the thread pool queues connections or individual requests. Queueing
  connections is simpler; it starves when concurrent connections outnumber
  worker threads. Step 5 documents whichever it picks along with the failure
  mode it accepts.
