# lrd — Local Recommendation Daemon

An on-device recommendation daemon written in C++20. Client processes talk to it
over a UNIX domain socket; a thread pool serves requests against a sharded
in-memory item store guarded by an explicit locking strategy.

It models the shape of an on-device ad-candidate delivery system: a background
daemon holds candidate items, ranks them against a **local user signal**, and
returns a slate. The signal is a handful of category weights — no history, no
identifiers, no item ids the user has seen. Raw activity has no path to the
daemon at all, which is the substance behind "privacy by architecture" rather
than a label on it.

```
$ lrd_cli recommend --signal tech=1.0,sport=0.3 --count 5
rank           id category         advertiser     score
1              19 tech             initech        0.882004
2              37 tech             acme           0.366338
3              20 sport            umbrella       0.277381
4              38 sport            globex         0.115887
5              79 tech             initech        0.051644
```

Built and tested on Ubuntu (WSL2), x86-64, GCC 15.2, C++20.

---

## Status

Phase 1 is a working key-value caching daemon. Phase 2 extends it into the
recommendation engine. Each step below is a self-contained, buildable
milestone.

| Step | Component | State |
|-----:|-----------|-------|
| 0 | Build system, presets, test harness | ✅ done |
| 1 | RAII fd wrapper, UNIX socket transport, short-read-safe I/O | ✅ done |
| 2 | Length-prefixed framing + binary codec | ✅ done |
| 3 | LRU cache | ✅ done |
| 4 | Thread pool | ✅ done |
| 5 | Concurrent server | ✅ done |
| 6 | Benchmark client (throughput, p50/p90/p99) | ✅ done |
| 7 | Sharded cache + contention measurements | ✅ done |

**Phase 1 complete.** Phase 2 turns it into the recommendation engine.

| Step | Component | State |
|-----:|-----------|-------|
| 8 | ProtobufCodec behind the Phase 1 seam, benchmarked | ✅ done |
| 9 | Item metadata + candidate ranking | ✅ done |
| 10 | Compliance: exposure cap, frequency limit | ⏳ next |
| 11 | Client SDK | — |
| 12 | Privacy: noise on exported metrics | — |

Phase 2 (candidate ranking, exposure/frequency policy, client SDK, privacy
noise) begins once step 7 lands.

---

## Build

```bash
cmake --preset debug
cmake --build --preset debug -j$(nproc)
ctest --preset debug
```

Four presets are defined:

| Preset | Purpose |
|--------|---------|
| `debug` | `-O0 -g`, day-to-day development |
| `release` | `-O2`, **the only build benchmark numbers are quoted from** |
| `tsan` | ThreadSanitizer — the evidence behind the "no data races" claim |
| `asan` | AddressSanitizer |

`scripts/build-all.sh` configures and builds all four and runs the suite under
the checked ones. `scripts/demo-kv.sh` runs the protocol end to end:
put/get/delete/stats, a 400 KB value, a rejected oversized value, and 2000
requests down one connection. `scripts/demo-cache.sh` demonstrates LRU
eviction through the socket, including the case that distinguishes LRU from
FIFO. `scripts/demo-concurrent.sh` runs eight
clients at once against the threaded daemon and then stops it with SIGTERM,
checking the socket file is cleaned up. `scripts/verify-tsan.sh` is the negative
control for the sanitizer: it injects a deliberate data race and confirms
ThreadSanitizer reports it, so that a clean TSan run is evidence rather than
decoration. Everything compiles with `-Wall -Wextra -Wpedantic
-Wconversion -Wshadow -Wold-style-cast -Werror`.

Requires a C++20 compiler, CMake ≥ 3.20 and pthreads. Protocol Buffers is
optional and auto-detected — without it the tree still builds and tests, minus
`ProtobufCodec`:

```bash
sudo apt install -y protobuf-compiler libprotobuf-dev   # optional
```

---

## Try it

```bash
cmake --preset debug && cmake --build --preset debug -j$(nproc)

./build/debug/bin/lrdd --socket /tmp/lrd.sock --capacity 10000 &
./build/debug/bin/lrd_cli --socket /tmp/lrd.sock --message "hello"

# 18 MB, split by the kernel across hundreds of reads and writes
./build/debug/bin/lrd_cli --socket /tmp/lrd.sock \
    --message "0123456789abcdefgh" --repeat 1000000
```

---

## Numbers

Release build, WSL2 on 8 cores, median of 3 trials. Full tables, method and
caveats in [`docs/benchmarks.md`](docs/benchmarks.md); reproduce with
`scripts/bench.sh`.

**End to end** (client → socket → daemon → locked cache), 90% reads, Zipf 0.99:

| threads | ops/sec | p50 | p99 | p99.9 |
|--------:|--------:|----:|----:|------:|
| 1 | 9,615 | 94 µs | 223 µs | 311 µs |
| 4 | 28,867 | 118 µs | 456 µs | 1.2 ms |
| 16 | 143,293 | 46 µs | 629 µs | 3.7 ms |

Single-threaded p50 is 94 µs against an **88 µs bare-IPC floor** on this host —
framing, codec, cache and lock together cost ~6 µs. The daemon is not where the
time goes; the syscalls are.

**The cache in-process**, no sockets in the way, 8 threads, by shard count:

| shards | ops/sec | p99 | hit rate | imbalance |
|-------:|--------:|----:|---------:|----------:|
| 1 | 583,516 | 184 µs | 90.8% | 1.00 |
| 8 | 2,940,006 | 28 µs | 90.8% | 1.00 |
| 64 | 6,723,146 | 5 µs | 90.6% | 1.00 |

One shard is a textbook mutex convoy — throughput *falls* as threads are added.
Sharding gives **11.5× throughput and a 35× better p99**, costing 0.2 points of
hit rate to per-shard eviction, with shard imbalance staying at 1.00.

**A prediction I got wrong, kept on the record.** Step 6 predicted sharding
would not move the end-to-end numbers, reasoning that a 300 ns cache operation
against a 90 µs request makes the lock ~0.3% of the work. Measured: **+43%
end-to-end throughput at 16 client threads**, reproducible across rounds. The
error was reasoning from *means* — under contention the single-shard p99 was
155–247 µs, the same order as an entire request. Contention is a tail
phenomenon, and comparing mean service times hides it. Full write-up in
[`docs/benchmarks.md`](docs/benchmarks.md).

---

## Design notes

Longer write-ups live in `docs/`. The short version of the decisions that
matter:

**Two wire formats behind one `Codec` interface**, selected by `--codec` at
runtime. Writing the binary one by hand first made short reads, `EINTR` and byte
order explicit rather than hiding them behind `ParseFromArray`; step 8 added
protobuf behind the same interface and measured both.

The comparison landed somewhere more interesting than "protobuf is slower":
protobuf costs **2–5× the CPU per message** and is **smaller on the wire every
time**, but end-to-end throughput is **indistinguishable** (the ordering flipped
between measurement rounds), with p50 latency consistently ~15% higher. So the
choice is not about speed — it is about failure modes. binary/v1 fails loudly on
anything unexpected; protobuf tolerates the unexpected so versions can drift
apart safely. Adding three `Stats` fields in step 3 was a *breaking* change under
binary/v1 and would have been additive under protobuf.

**And protobuf does not make the length prefix redundant.** It cannot reliably
detect truncation — a message of optional fields cut on a field boundary parses
as a valid shorter message — so the framing layer is what catches a short read.
That is asserted in the shared conformance suite, not assumed.

**A reader-writer lock is the wrong default for an LRU cache.** A cache hit
moves its entry to most-recently-used, so `get()` mutates — taking a shared
lock on the read path is a data race, not an optimisation. `LruCache::get` is
therefore deliberately non-`const`; the signature is the warning, and `peek()`
sits beside it as the operation that genuinely is const. Phase 1 starts with a
plain `std::mutex` (step 5), measures the resulting contention (step 6), then
shards the cache (step 7). The alternatives — CLOCK, sampled eviction,
sharding, lock-free — are each written up with their real costs in
`docs/concurrency.md`.

**Ranking is two-stage, because diversity cannot be sorted for.** The penalty for
repeating a category depends on what has *already been picked*, so it cannot be
precomputed. Retrieval scores every candidate independently and keeps the best
`k × 4` in a bounded min-heap (O(n log m), O(m) memory); re-ranking runs the
order-dependent diversity logic over only that shortlist. Same shape as a real
ranking pipeline, for the same reason. Full write-up in
[`docs/ranking.md`](docs/ranking.md).

**A recommendation scan must not touch recency.** Ranking visits every item; doing
that through `get()` would mark the whole cache most-recently-used on every
request, and eviction would become effectively random — the cache would keep
working and quietly stop being a cache. The scan is `const`, goes shard by shard,
and holds each lock only long enough to copy out refcounted pointers. A test pins
it: after a full scan, the item that was least recently used *before* the scan
must still be the one evicted.

**`std::variant` for messages, having rejected it in v1.** Step 2 used a tagged
struct and said so explicitly — with four types sharing two fields, `std::visit`
cost more clarity than it bought, "worth revisiting when item metadata and signal
payloads arrive". They arrived. The tagged version of v2 would carry an id, an
Item, a UserSignal, a count and a flag, of which at most two are meaningful per
message, with nothing in the type saying which. The variant makes that
unrepresentable, and adding an alternative now fails to compile at every visit
site until it is handled.

**Sharding needs the hash mixed first.** `std::hash<int>` in libstdc++ *is the
identity function*, so masking the low bits of an unmixed hash would put every
multiple of N in shard 0 — sequential integer keys would pile into a few shards
and the sharding would achieve nothing while looking correct. Keys go through a
splitmix64 finalizer before masking, and a test asserts sequential integer keys
spread evenly. Shard count is a power of two so selection is a mask rather than
a division.

**Cache values are `shared_ptr<const Value>`.** A reference into the cache would
dangle once the lock is released or the entry is evicted; returning by value
copies the whole payload *while holding the lock*. A shared_ptr copies one
refcounted pointer under the lock and keeps the value alive for as long as the
caller holds it. The `const` means an update swaps in a new pointer rather than
mutating one a reader is holding, so readers always see a coherent snapshot.

**Stream sockets have no message boundaries, and the transport layer is built
around that.** `read()` on a `SOCK_STREAM` socket returns whatever has arrived,
not what you asked for, so every read goes through `read_exact`, which loops.
`write_all` is the symmetric case for a full send buffer. Both retry `EINTR`,
and `write_all` uses `send(MSG_NOSIGNAL)` so a client hanging up mid-write
surfaces as `EPIPE` rather than killing the daemon with `SIGPIPE`.

**Framing is separate from serialisation.** The framing layer knows only about
the 4-byte length prefix and the size cap; the codec owns the header and the
payload. That split keeps the allocation-bounding check in one obvious place
and lets the same codec run over a socket, a file, or a test's `std::vector`.
A length prefix is an allocation instruction from an untrusted peer, so it is
validated before anything is resized — see `docs/protocol.md`.

**The task queue is bounded, and the thread pool says so.** An unbounded queue
turns a producer outrunning its consumers into unbounded memory growth — an OOM
kill later, far from the cause. `post()` returns `QueueFull` instead, so
overload becomes a decision the caller makes at the moment it happens. In step 5
the acceptor's answer will be to close the connection rather than pretend it can
serve it.

**`Task` is a hand-written move-only callable.** `std::function` requires a
copy-constructible target, which a lambda capturing a `UnixStream` (owning a
move-only `Fd`) is not. C++23's `std::move_only_function` solves this properly;
this build is C++20, verified, so `Task` is the ~40-line stand-in built from a
concept/model type-erasure pair. Migration to C++23 would be a single `using`.

**Blocking connection-per-task, not epoll, in Phase 1.** A worker owns a
connection until the peer disconnects. Simpler to reason about and to explain,
with one sharp edge stated rather than discovered: **more concurrent connections
than worker threads means the surplus get no service at all**, because the busy
workers are blocked in `read()` on clients that may be idle. Fine for a handful
of local clients on a device; not fine for many idle-ish connections. The fix is
to dispatch *requests* rather than *connections* via epoll — a real
restructuring, sketched in `Server`'s header rather than half-done.

**Shutdown uses the self-pipe trick.** A signal arriving while the acceptor is
blocked in `accept()` cannot be noticed by setting a flag — the thread is in the
kernel. The handler writes one byte to a pipe the acceptor also polls, which is
async-signal-safe. Workers blocked reading a quiet client are freed by
`shutdown(fd, SHUT_RDWR)` from a registry of live connections — `shutdown`, not
`close`, because closing a descriptor another thread is using is a use-after-free
with extra steps. Result: SIGTERM exits cleanly and removes the socket file,
instead of leaving the stale-socket debris that steps 1–4 relied on `bind()` to
clean up afterwards.

---

## Layout

```
include/lrd/     public headers (common, net, proto, cache, concurrency)
src/             implementation, mirroring include/
  daemon/        the lrdd binary
  client/        client library — becomes the SDK in Phase 2
bench/           load generator and latency reporter
tests/           dependency-free harness + CTest entries
docs/            protocol spec, concurrency write-up, benchmark results
scripts/         build and smoke helpers
```
