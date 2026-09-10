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
| 4 | Thread pool | ⏳ next |
| 5 | Concurrent server | — |
| 6 | Benchmark client (throughput, p50/p90/p99) | — |
| 7 | Sharded cache + contention measurements | — |

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
FIFO. Everything compiles with `-Wall -Wextra -Wpedantic
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

**Blocking connection-per-task, not epoll, in Phase 1.** Simpler to reason
about and to explain. Its real limitation — more concurrent connections than
pool threads causes starvation — is a documented tradeoff, with the epoll
migration sketched rather than pretended away.

---

## Layout

```
include/lrd/     public headers (common, net, proto, cache, concurrency)
src/             implementation, mirroring include/
  daemon/        the lrdd binary
  client/        client library — becomes the SDK in Phase 2
bench/           load generator and latency reporter
tests/           dependency-free harness + CTest entries
docs/            protocol spec and concurrency write-up
scripts/         build and smoke helpers
```
