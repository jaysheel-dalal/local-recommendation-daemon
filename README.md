# lrd — Local Recommendation Daemon

An on-device caching and ranking daemon written in C++20. Client processes talk
to it over a UNIX domain socket; a thread pool serves requests against a shared
in-memory cache guarded by an explicit locking strategy.

The project models the shape of an on-device ad-candidate delivery system:
local signals stay local, ranking and policy enforcement happen in a background
daemon, and applications reach it through a thin client SDK rather than through
raw socket calls.

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
| 7 | Sharded cache + contention measurements | ⏳ next |

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

Requires only a C++20 compiler, CMake ≥ 3.20 and pthreads. No third-party
dependencies in Phase 1 — see *Wire format* below for why.

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

**The same cache in-process**, no sockets in the way:

| threads | ops/sec | p50 | p99 |
|--------:|--------:|----:|----:|
| 1 | 2,772,177 | 200 ns | 700 ns |
| 8 | 618,791 | 1,600 ns | 160 µs |

Throughput falls to **22% of single-threaded** and the p99 rises **230×** — a
textbook mutex convoy.

Both tables matter, and that is the finding: the lock is catastrophic in
isolation and invisible end-to-end, because one IPC round trip is ~300× one
uncontended cache operation. So the honest prediction for step 7 is that
sharding will transform the second table and leave the first unchanged — and
both results get reported.

---

## Design notes

Longer write-ups live in `docs/`. The short version of the decisions that
matter:

**Wire format — hand-rolled binary in Phase 1, Protocol Buffers in Phase 2,
behind one `Codec` interface.** `SOCK_STREAM` gives you a byte stream with no
message boundaries, so framing has to exist regardless of what serialises the
payload. Writing it by hand first makes short reads, `EINTR` and byte order
explicit rather than hiding them behind `ParseFromArray`. Phase 2 adds a
protobuf codec behind the same interface, so the two are directly comparable on
the same benchmark.

**A reader-writer lock is the wrong default for an LRU cache.** A cache hit
moves its entry to most-recently-used, so `get()` mutates — taking a shared
lock on the read path is a data race, not an optimisation. `LruCache::get` is
therefore deliberately non-`const`; the signature is the warning, and `peek()`
sits beside it as the operation that genuinely is const. Phase 1 starts with a
plain `std::mutex` (step 5), measures the resulting contention (step 6), then
shards the cache (step 7). The alternatives — CLOCK, sampled eviction,
sharding, lock-free — are each written up with their real costs in
`docs/concurrency.md`.

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
