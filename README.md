# lrd — Local Recommendation Daemon

A background daemon that serves ranked candidate items to client processes on the
same machine over a UNIX domain socket. Clients link a small SDK, send a *local
user signal* (a handful of category affinities — no history, no identifiers, no
record of what has been seen), and get back a ranked slate. On top of that sit
compliance-style rate limits — a lifetime exposure cap and a sliding-window
frequency limit per item — and a telemetry path that adds calibrated Laplace
noise to exported counters before they leave the process. Written in **C++20**
with **UNIX domain sockets**, **Protocol Buffers** (behind a swappable codec
interface, optional at build time), and **CMake**; ~7,800 lines of library and
daemon code against ~5,800 lines of tests.

```
$ lrd_cli recommend --signal tech=1.0,sport=0.3 --count 5
rank           id category         advertiser     score
1              19 tech             initech        0.882004
2              37 tech             acme           0.366338
3              20 sport            umbrella       0.277381
4              38 sport            globex         0.115887
5              79 tech             initech        0.051644
```

Built and tested on Ubuntu (WSL2), x86-64, GCC 15.2, C++20. 243 test cases across
18 binaries, clean under ThreadSanitizer and AddressSanitizer.

---

## Architecture

```
  client process
  ┌──────────────────────────────────────────────┐
  │  lrd::sdk::Client                            │  connection pool, timeouts,
  │  put_item / get_item / recommend / stats     │  bounded retry, RAII leases
  └──────────────────────┬───────────────────────┘
                         │  UNIX domain socket (SOCK_STREAM, mode 0600)
  ═══════════════════════╪═══════════════════════════════════════════════════
  lrdd daemon            ▼
  ┌──────────────────────────────────────────────┐
  │  RAII transport    Fd / UnixListener /       │  move-only fds, short-read
  │                    UnixStream                │  safe I/O, EINTR handling
  ├──────────────────────────────────────────────┤
  │  framing           4-byte length prefix,     │  size cap validated before
  │                    codec-agnostic            │  any allocation
  ├──────────────────────────────────────────────┤
  │  codec seam        BinaryCodec | Protobuf-   │  chosen by --codec, one
  │                    Codec                     │  shared conformance suite
  ├──────────────────────────────────────────────┤
  │  concurrency       acceptor thread +         │  bounded queue, self-pipe
  │                    bounded thread pool       │  shutdown, SIGTERM-clean
  ├──────────────────────────────────────────────┤
  │  storage           ShardedCache → N ×        │  power-of-two shards,
  │                    LockedCache(LruCache)     │  splitmix64-mixed keys
  ├──────────────────────────────────────────────┤
  │  ranking           retrieval (bounded        │  two-stage; diversity is
  │                    min-heap) → re-rank       │  order-dependent
  ├──────────────────────────────────────────────┤
  │  policy            ExposureStore: lifetime   │  atomic check-and-reserve,
  │                    cap + sliding window      │  sharded like the cache
  ├──────────────────────────────────────────────┤
  │  telemetry         NoisyCounters via         │  Laplace noise at the export
  │                    published_stats()         │  boundary only
  └──────────────────────────────────────────────┘
```

The request path in one line: **socket → framing → codec → cache lookup or
ranking scan → policy gate → codec → socket**, with the policy gate running
*during* candidate selection rather than after it, so a capped item is replaced by
the next best one instead of leaving a hole in the slate.

---

## Benchmarks

> **Measured on WSL2, not native Linux.** WSL2's syscall path costs several times
> a native kernel's — the bare IPC round-trip floor on this host is **~84 µs**,
> where native Linux on comparable hardware would be single-digit microseconds.
> Every absolute latency figure below is dominated by that. Run-to-run variance
> was measured at roughly **±40%**, so each number is a median of repeated trials
> with the observed spread reported. **Read these as relative comparisons between
> configurations, not as absolute performance claims.**

Release preset, 8 cores, GCC 15.2. Reproduce with `scripts/bench.sh`.

### The floor, measured first

Before attributing any latency to the project's own code, measure what the
machine charges for the syscalls underneath it. These probes link none of the
project:

| Probe | Round trip |
|---|---:|
| one `write` + one `read` per side | ~84 µs |
| same, 40-byte messages | ~88 µs |
| two reads per message (this project's shape) | ~104 µs |

The third row is the cost of reading the length prefix separately from the body:
**~16 µs, 18% of a round trip.** That is the price of validating a length before
allocating against it, and it is documented rather than optimised away, because
the check is what stops a peer from asking the daemon to allocate 4 GiB.

### End to end

Client → socket → daemon → cache. 10,000 keys, Zipf θ=0.99, 128-byte values, 90%
reads, 5,000 measured requests per thread after warmup, median of 3 trials.

| threads | ops/sec | p50 | p90 | p99 | p99.9 |
|--------:|--------:|----:|----:|----:|------:|
| 1 | 9,615 | 94.1 µs | 133.5 µs | 223.2 µs | 311.2 µs |
| 2 | 19,790 | 92.9 µs | 123.6 µs | 211.4 µs | 416.1 µs |
| 4 | 28,867 | 117.7 µs | 201.6 µs | 456.0 µs | 1.19 ms |
| 8 | 63,212 | 26.5 µs | 119.5 µs | 351.7 µs | 1.40 ms |
| 16 | 143,293 | 45.7 µs | 113.5 µs | 628.6 µs | 3.73 ms |

**Single-threaded p50 is 94.1 µs against an 88 µs IPC floor.** Framing, codec,
hash lookup, list splice, lock and counters together cost about 6 µs — within the
noise of the measurement. The daemon is not where the time goes; the syscalls
are.

Throughput scales ~15× from 1 to 16 client threads, which is expected precisely
*because* the bottleneck is per-connection syscall latency: each connection
spends its time blocked, so adding connections adds parallelism nearly for free.

### The cache in process, by shard count

`lrd_cache_bench` drives the same cache directly, no sockets in the way. 8
threads, 300,000 operations each, median of 3 trials.

| shards | ops/sec | p50 | p99 | hit rate | shard imbalance |
|-------:|--------:|----:|----:|---------:|----------------:|
| 1 | 583,516 | 1,500 ns | 184.2 µs | 90.8% | 1.00 |
| 4 | 1,893,666 | 800 ns | 61.0 µs | 90.8% | 1.00 |
| 16 | 4,087,800 | 500 ns | 15.8 µs | 90.7% | 1.00 |
| 64 | 6,723,146 | 400 ns | 5.2 µs | 90.6% | 1.00 |

**11.5× throughput and a 35× better p99**, with returns not yet flat at 64
shards. Two columns are there to catch the ways this could have been a hollow
win: the hit rate falls only 0.2 points (eviction is per-shard now, so a global
LRU's choices are no longer available), and shard imbalance stays at 1.00,
confirming the hash spreads keys evenly. A weak hash would show up as an
imbalance well above 1 and a throughput curve that flattened early.

Holding shards fixed and varying threads shows what the single lock was actually
doing:

| threads | 1 shard | 16 shards | change |
|--------:|--------:|----------:|-------:|
| 1 | 2,646,499 | 2,606,117 | −2% |
| 4 | 951,303 | 4,434,055 | +366% |
| 8 | 642,232 | 4,025,949 | +527% |
| 16 | 717,893 | 3,546,376 | +394% |

The single-shard column *falls* as threads are added — a textbook mutex convoy.
At one thread the two are identical, as they should be: with no contention there
is nothing for sharding to fix.

### What I learned: contention is a tail phenomenon

**I predicted, on the record, that sharding would not change the end-to-end
numbers at all.** The reasoning looked sound: one IPC round trip is ~90 µs, one
uncontended cache operation ~300 ns, so the lock is ~0.3% of a request and should
be invisible. That prediction went into `docs/benchmarks.md` before measuring.

It was wrong. Measured at 16 client threads, 5 trials per arm, two independent
rounds with the arms interleaved:

| | round 1 median | round 2 median | observed range |
|---|---:|---:|---|
| 1 shard | 119,023 | 123,854 | 96,489 – 156,697 |
| 16 shards | 169,466 | 177,042 | 146,598 – 218,156 |

**About +43% end-to-end throughput**, reproducible across rounds.

The error was reasoning from **means**. Comparing a 300 ns mean cache operation
against a 90 µs mean request is valid only when the lock is uncontended. Under
contention it is not: the single-shard cache's p99 at 8–16 threads was **155–247
µs**, the same order of magnitude as an entire request. A request that waits on
the lock does not pay 300 ns — it occasionally pays more than the rest of the
request costs put together, and those waits consume a worker thread while it
waits.

The headroom argument failed the same way. "The cache does 670k ops/sec and we
only need 143k, therefore 4× headroom" treats a serialising resource as if it
were a pipe. Queueing theory says waiting time rises well before utilisation
approaches 1, driven by the *variance* of service time — which a convoy inflates
enormously.

The right comparison was the contended p99 against the request latency, and that
one predicted a real effect. Both the wrong prediction and its correction are
still in `docs/benchmarks.md`, because the correction is the useful part.

### Binary vs. Protobuf codec

Both sit behind the same `Codec` interface and pass the same conformance suite,
so the daemon runs either from a `--codec` flag with no recompilation.

Per-message CPU and wire size (24-byte key, 128-byte value, 300k iterations,
median of 3):

| message | binary enc | protobuf enc | binary dec | protobuf dec | binary bytes | protobuf bytes |
|---|---:|---:|---:|---:|---:|---:|
| GetRequest | 61 ns | 168 ns | 47 ns | 198 ns | 44 | **38** |
| PutRequest | 80 ns | 290 ns | 71 ns | 372 ns | 176 | **170** |
| StatsRequest | 48 ns | 77 ns | 27 ns | 81 ns | 16 | **4** |
| StatsResponse | **225 ns** | 137 ns | 73 ns | 218 ns | 89 | **22** |

**Protobuf costs 2–5× more CPU per message and is smaller on the wire every
time.** The interesting row is `StatsResponse`, the one message protobuf encodes
*faster* — 137 ns against 225 ns, and 4× smaller. Both come from the same cause:
the message is nine `uint64` counters, protobuf varint-encodes them so a value of
45 takes one byte, while the hand-written codec writes all nine as fixed 8-byte
big-endian fields through a `push_back` loop. That 225 ns outlier is a real
inefficiency in my own encoder, and the comparison is what exposed it.

End to end, three interleaved rounds at 8 client threads:

| round | binary ops/sec | protobuf ops/sec | binary p50 | protobuf p50 |
|---|---:|---:|---:|---:|
| 1 | 180,272 | 238,655 | 23.0 µs | 25.9 µs |
| 2 | 224,586 | 146,969 | 22.6 µs | 26.4 µs |
| 3 | 172,743 | 159,286 | 23.2 µs | 26.9 µs |

**Throughput is indistinguishable** — the ordering flips between rounds and the
ranges overlap heavily. The first run of this comparison showed protobuf 36%
slower and the second showed it 8% faster; reporting either alone would have been
reporting noise. **p50 latency is consistently ~15% higher for protobuf**, and
that one *is* stable across every round.

---

## Notable engineering decisions

### Sharding was measured wrong the first time

Covered in full above. The short version: I compared mean service times, which
systematically hides contention, and predicted no end-to-end effect where there
was a reproducible +43%. Contention lives in the tail. The corrected reasoning —
compare the *contended* p99 against request latency — is what I would use now.

The daemon defaults to 16 shards rather than the 64 the in-process numbers argue
for, and the reason is capacity granularity, not throughput: shards divide the
cache evenly, so 64 shards at the default 10,000 entries leaves 156 entries each,
and an unevenly distributed hot set would evict more than the aggregate hit rate
suggests.

### On-device execution makes per-user rate limiting possible without a user identifier

A frequency cap is normally "N shows per *user* per hour", which requires a user
identifier to key the counter on — exactly the thing this design deliberately
does not carry. The signal has no id, no history, and no record of what has been
seen.

But the daemon serves one device, so per-device and per-user are the same scope.
The counters are keyed by **item id alone**; the user's identity is implicit in
the process boundary rather than stored in a field. The cap works without the
daemon ever learning who it is capping. A server-side system cannot do this — it
has to know who you are in order to count what you have seen, and that
requirement is an artifact of where the code runs rather than of the problem
itself.

**Check-and-reserve is one atomic call, not a check followed by a record.** "Read
the count, see it is below the cap, increment" is a textbook check-then-act: two
threads both read `cap−1` and both increment. `reserve()` does both under one
shard lock, and the API offers no way to split them. Tested with 8 threads × 500
attempts against a cap of 500, asserting the total allowed is *exactly* 500,
clean under TSan.

**The frequency window is a sliding-window counter, not a fixed bucket.** A fixed
bucket allows 2× the limit across a boundary — four shows at 10:59 and four more
at 11:01 all pass. Keeping the previous window's count and weighting it by how
much is still in view costs O(1) memory and closes that.

**Stated limitation: these counters live in memory and are not durable.** Restart
the daemon and every exposure count is gone. A bounded in-memory store makes
over-exposure unlikely, not impossible. Making it durable means persisting the
counters — a write-ahead log or an embedded key-value store — and treating the
in-memory map as a cache over it. That is written up in
[`docs/policy.md`](docs/policy.md) rather than glossed over.

### The codec seam, and what benchmarking both taught

Two wire formats live behind one `Codec` interface, selected by `--codec` at
runtime. Writing the binary one by hand first made short reads, `EINTR` and byte
order explicit rather than hiding them behind `ParseFromArray`; Protobuf was
added later behind the same interface, and both run the same conformance suite.

The benchmark's answer was more interesting than "protobuf is slower": **the
codec choice does not change what this daemon can deliver.** So the decision is
not about speed — it is about failure modes:

| | binary/v1 | protobuf/v1 |
|---|---|---|
| Adding a field | **Breaking.** The decoder rejects trailing bytes — this actually happened mid-project. | **Additive.** Unknown fields are preserved; an old peer ignores them. |
| Desynchronised stream | Caught immediately by per-frame magic. | No magic; usually caught because a binary body's first byte is an invalid tag. |
| Truncation | Rejected at any offset. | **Not reliably detectable.** |
| Schema as documentation | Prose. | `proto/lrd.proto`, machine-checked, and generates other languages. |
| Dependency | None. | protoc plus libprotobuf at build and run time. |

binary/v1 fails loudly on anything unexpected; protobuf tolerates the unexpected
so that versions can drift apart safely.

**The truncation row changed a design decision.** Protobuf cannot reliably tell a
short message from a complete one — a message of optional fields cut on a field
boundary parses as a valid shorter message — so adopting protobuf does *not* make
the length prefix redundant. The prefix is what makes truncation detectable at
all. That is now asserted in the shared conformance suite rather than assumed.

### Telemetry noise, and what it does and does not guarantee

With `--privacy`, exported counters leave the process as
`suppress(round(clamp(exact + Laplace(0, sensitivity/ε))))`.

**The guarantee is event-level, not user-level**, and saying so precisely is most
of what makes it a mechanism rather than a gesture. On a single-user device this
person's contribution to a counter *is* the whole counter, so a user-level bound
would mean publishing nothing at all. What the noise buys is that no *individual
event* can be confirmed.

**The draw is memoised per (metric, epoch).** Fresh noise on every read would
average away to the truth across a thousand polls — the mechanism would look
identical and protect nothing. It is implemented as a hash of (seed, metric,
epoch) rather than a cache, so it needs no lock and no eviction, and it survives a
daemon restart within the epoch. A cache would not, handing an observer a fresh
sample for the price of a restart.

**Suppression tests the noisy value, never the exact one.** Suppressing on the
true count would leak precisely what the noise hides: a reader seeing zero would
learn the true count was below the threshold.

Which counters get noise is decided by what they derive from — `requests`,
`hits`, `recommends` and the policy outcomes are behavioural; `puts`, `evictions`
and `capacity` describe the catalogue and the configuration, so noising them
would cost accuracy and buy no privacy. A consequence stated rather than papered
over: **the noised counters no longer sum consistently**, and forcing them to
would itself be a leak.

**Stated limitations.** `std::mt19937_64` is not cryptographically secure, and the
published counters *are* its output. Naive floating-point Laplace sampling leaks
through its low-order bits (Mironov 2012), so the guarantee here is the intended
one rather than a proven one — a discrete mechanism over integers is the fix. And
there is no budget accounting across epochs: N epochs of observation is N×ε. None
of these is closed, and [`docs/privacy.md`](docs/privacy.md) says so.

### Three bugs worth describing

**The full-duplex deadlock.** An early echo demo hung on an 18 MB message and
completed fine on small ones. The client wrote its entire payload before reading
anything back. The daemon read some, echoed it, and filled its own send buffer;
the client's receive buffer filled too, so the daemon's `write` blocked — and the
client was still in `write`, not reading. Both sides blocked in `write` waiting
for the other to read. It only appears once the payload exceeds the sum of the
kernel buffers, which is exactly why small messages passed. Fixed by giving the
client a sender thread so send and receive overlap. It turned out to be the most
useful bug in the project: it is the structural reason real protocols frame
messages and bound their size, and that reasoning is now in
[`docs/protocol.md`](docs/protocol.md).

**Member initialisation order.** Shutdown hung forever — `test_server` hit its
60-second timeout with no error and no obvious cause. The self-pipe used for
shutdown was built like this:

```cpp
Fd stop_read_;
Fd stop_write_;
// ...
Server::Server(...) : stop_read_(make_pipe(stop_write_)) { }
```

C++ initialises members in **declaration order**, not in the order they appear in
the initialiser list. `stop_read_` is declared first, so `make_pipe` ran and wrote
the write end into `stop_write_` — and then `stop_write_`'s own default
initialiser ran afterwards and overwrote it with −1. The signal handler wrote to
fd −1, the acceptor never woke, and nothing ever reported an error. Fixed
structurally rather than by reordering the members: `make_pipe` now returns a
`StopPipe { Fd read; Fd write; }` by value, which makes the ordering irrelevant
and the bug unrepresentable.

**A framing constant that only broke once a second codec existed.** The framing
layer rejected frames smaller than `kHeaderSize` — a constant belonging to the
binary codec's 16-byte header. That was invisible and harmless for as long as
there was one codec, because every frame it produced was at least that large.
Adding Protobuf made small messages possible (a `StatsRequest` is 4 bytes), and
the framing layer rejected them as `Oversized`. The bug was not the bound itself;
it was a codec-specific constant living in the codec-agnostic layer, and a single
implementation behind an interface is not enough to prove the interface is
actually clean. Fixed with a genuinely codec-agnostic `kMinFrameSize = 1`.

### Smaller decisions with reasons

- **A reader-writer lock is the wrong default for an LRU cache.** A cache hit
  moves its entry to most-recently-used, so `get()` *mutates* — taking a shared
  lock on the read path is a data race, not an optimisation. `LruCache::get` is
  deliberately non-`const`; the signature is the warning, and `peek()` sits beside
  it as the operation that genuinely is const.
- **Cache values are `shared_ptr<const Value>`.** A reference into the cache
  dangles once the lock is released; returning by value copies the whole payload
  *while holding the lock*. A `shared_ptr` copies one refcounted pointer under the
  lock and keeps the value alive as long as the caller holds it. The `const` means
  an update swaps in a new pointer rather than mutating one a reader is holding.
- **Sharding needs the hash mixed first.** `std::hash<int>` in libstdc++ *is the
  identity function*, so masking the low bits of an unmixed hash would put every
  multiple of N in shard 0 — sequential keys would pile into a few shards and the
  sharding would achieve nothing while looking correct. Keys go through a
  splitmix64 finalizer before masking, and a test asserts sequential integer keys
  spread evenly.
- **Ranking is two-stage because diversity cannot be sorted for.** The penalty for
  repeating a category depends on what has *already been picked*, so it cannot be
  precomputed. Retrieval scores every candidate independently and keeps the best
  `k × 4` in a bounded min-heap (O(n log m) time, O(m) memory); re-ranking runs
  the order-dependent logic over only that shortlist.
- **A recommendation scan must not touch recency.** Ranking visits every item;
  doing that through `get()` would mark the whole cache most-recently-used on
  every request, and eviction would become effectively random — the cache would
  keep working and quietly stop being a cache. The scan is `const` and holds each
  shard lock only long enough to copy out refcounted pointers. A test pins it:
  after a full scan, the item least recently used *before* the scan must still be
  the one evicted.
- **The task queue is bounded, and the thread pool says so.** An unbounded queue
  turns a producer outrunning its consumers into unbounded memory growth — an OOM
  kill later, far from the cause. `post()` returns `QueueFull` instead, so
  overload becomes a decision the caller makes at the moment it happens.
- **Shutdown uses the self-pipe trick.** A signal arriving while the acceptor is
  blocked in `accept()` cannot be noticed by setting a flag — the thread is in the
  kernel. The handler writes one byte to a pipe the acceptor also polls, which is
  async-signal-safe. Workers blocked reading a quiet client are freed by
  `shutdown(fd, SHUT_RDWR)`, not `close()`, because closing a descriptor another
  thread is using is a use-after-free with extra steps.
- **Which SDK operations retry is a compliance question, not a networking one.** A
  recording `recommend()` reserves exposure against every item it returns. If the
  connection dies after the daemon processed the request but before the reply
  arrived, the work is done — and a retry charges those caps twice. Without
  daemon-side deduplication you cannot have both at-least-once and at-most-once,
  so the SDK picks at-most-once for the operation with a side effect and reports
  the loss. Reads, `put_item` and `preview()` retry freely.
- **A failed connection is dropped, never returned to the pool.** A failed call may
  have left half a frame on the wire; recycling that connection would hand the
  next caller a stream whose next read is the tail of someone else's message.
  Tested by killing the daemon under a live client and restarting it.
- **The SDK is verified to stand alone, not just claimed to.**
  `scripts/verify-sdk-install.sh` installs it to a throwaway prefix and builds
  `examples/recommender_app.cpp` from a separate directory via `find_package(lrd)`,
  with no access to this source tree. A consumer sees exactly four headers and no
  socket, codec, framing or protobuf. Writing that script caught two real export
  bugs: a build-tree `ALIAS` is not exported (`EXPORT_NAME` is the separate
  property that fixes it), and CMake refuses to export a target carrying a bare
  source-tree include path.

### Known limitations

Collected in one place rather than scattered through the prose:

- **Blocking connection-per-task, not `epoll`.** A worker owns a connection until
  the peer disconnects. Simpler to reason about, with one sharp edge stated rather
  than discovered: **more concurrent connections than worker threads means the
  surplus get no service at all**, because the busy workers are blocked in
  `read()` on clients that may be idle. Fine for a handful of local clients on a
  device; not fine for many idle-ish connections. The fix is to dispatch
  *requests* rather than *connections* — a real restructuring, sketched in
  `Server`'s header rather than half-done.
- **Exposure and frequency counters are not durable across a restart.**
- **The privacy RNG is not cryptographically secure**, the Laplace sampler is the
  naive floating-point form, and there is no cross-epoch budget accounting.
- **Benchmarks are WSL2 numbers.** Useful for comparing configurations against each
  other; not a claim about absolute achievable throughput.

---

## Build and run

Requires a C++20 compiler, CMake ≥ 3.20 and pthreads. Protocol Buffers is optional
and auto-detected — without it the tree still builds and every test passes, minus
`ProtobufCodec`.

```bash
sudo apt install -y build-essential cmake                 # required
sudo apt install -y protobuf-compiler libprotobuf-dev     # optional
```

### Build

```bash
git clone https://github.com/jaysheel-dalal/local-recommendation-daemon.git
cd local-recommendation-daemon

cmake --preset debug
cmake --build --preset debug -j$(nproc)
```

Four presets are defined:

| Preset | Purpose |
|--------|---------|
| `debug` | `-O0 -g`, day-to-day development |
| `release` | `-O2`, **the only build benchmark numbers are quoted from** |
| `tsan` | ThreadSanitizer — the evidence behind the "no data races" claim |
| `asan` | AddressSanitizer |

Everything compiles with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
-Wshadow -Wold-style-cast -Werror`.

### Run the daemon

```bash
./build/debug/bin/lrdd --socket /tmp/lrd.sock --capacity 10000 --shards 16 &

# store some items and ask for a slate
./build/debug/bin/lrd_cli --socket /tmp/lrd.sock seed 100
./build/debug/bin/lrd_cli --socket /tmp/lrd.sock \
    recommend --signal tech=1.0,sport=0.3 --count 5

# with compliance limits and noised telemetry
./build/debug/bin/lrdd --socket /tmp/lrd.sock \
    --exposure-cap 3 --frequency-limit 2 --frequency-window 3600 \
    --privacy --privacy-epsilon 1.0 --privacy-epoch 3600 &

# protobuf on the wire instead of the hand-written codec
./build/debug/bin/lrdd --socket /tmp/lrd.sock --codec protobuf &
./build/debug/bin/lrd_cli --socket /tmp/lrd.sock --codec protobuf stats
```

`lrdd --help` and `lrd_cli --help` list every flag. SIGTERM shuts down cleanly and
removes the socket file.

### Test

```bash
ctest --preset debug           # 243 cases across 18 binaries
ctest --preset tsan            # the same suite under ThreadSanitizer
ctest --preset asan            # the same suite under AddressSanitizer

./scripts/build-all.sh         # configure, build and test all four presets
./scripts/count-cases.sh       # per-binary case counts
```

Two of the scripts are negative controls for the tooling itself:
`scripts/verify-tsan.sh` injects a deliberate data race and confirms
ThreadSanitizer reports it, so that a clean TSan run is evidence rather than
decoration; `scripts/verify-harness.sh` does the same for the test harness.

### Benchmark

```bash
cmake --preset release && cmake --build --preset release -j$(nproc)

./scripts/latency-probe.sh     # the bare IPC floor on this machine
./scripts/bench.sh             # end-to-end and in-process, all thread counts
./scripts/bench-shards.sh      # shard-count sweep
./scripts/bench-codec-e2e.sh   # binary vs protobuf, interleaved rounds
```

Run `latency-probe.sh` first on any new host — the end-to-end numbers are
meaningless without knowing what the syscalls cost there.

### Demos

```bash
./scripts/demo-kv.sh           # protocol end to end, including a rejected oversized value
./scripts/demo-cache.sh        # LRU eviction, including the case that distinguishes LRU from FIFO
./scripts/demo-concurrent.sh   # eight clients at once, then SIGTERM and socket cleanup
./scripts/demo-recommend.sh    # ranking and diversity re-ranking
./scripts/demo-policy.sh       # exposure caps and the sliding window
./scripts/demo-sdk.sh          # the SDK, pooling and retry behaviour
./scripts/demo-privacy.sh      # noised counters, and the averaging attack failing
```

---

## Project structure

```
include/lrd/          public headers
  common/             Fd (move-only RAII descriptor), errors, logging, hash mixing
  net/                UnixListener / UnixStream, short-read-safe I/O
  proto/              wire types, framing, Codec interface, binary + protobuf codecs
  cache/              LruCache, LockedCache, ShardedCache
  concurrency/        Task (move-only callable), ThreadPool
  rank/               Item, UserSignal, scorer
  policy/             exposure cap and sliding-window frequency limit
  privacy/            Laplace noise for exported counters
  daemon/             Server, Handler
  sdk/                the four headers a consumer sees

src/                  implementation, mirroring include/
  daemon/             the lrdd binary
  client/             lrd_cli and the SDK implementation

proto/lrd.proto       the protobuf schema
bench/                load generator, latency percentiles, cache and codec
                      microbenchmarks, and three bare-syscall baseline probes
tests/                dependency-free harness, 18 binaries, 243 cases, plus a
                      codec conformance suite both codecs must pass
examples/             an app consuming the installed SDK and nothing else
docs/                 the design write-ups listed below
scripts/              build, test, benchmark, demo and verification helpers
cmake/                package config for find_package(lrd)
```

### Further reading

Each design document is written to be read on its own.

| Document | Covers |
|---|---|
| [`docs/protocol.md`](docs/protocol.md) | Framing, the length-prefix rationale, binary/v1 on the wire |
| [`docs/concurrency.md`](docs/concurrency.md) | Locking strategy, the rejected alternatives, shutdown |
| [`docs/benchmarks.md`](docs/benchmarks.md) | Every number above, the method, and the wrong prediction |
| [`docs/ranking.md`](docs/ranking.md) | Two-stage retrieval and diversity re-ranking |
| [`docs/policy.md`](docs/policy.md) | Exposure caps, sliding windows, check-and-reserve |
| [`docs/sdk.md`](docs/sdk.md) | API surface, pooling, and what may and may not be retried |
| [`docs/privacy.md`](docs/privacy.md) | The noise mechanism, what it guarantees, and what it does not |
