# Benchmarks

All numbers from the `release` preset on the development host: WSL2 (Ubuntu
26.04, kernel 6.6), 8 cores, GCC 15.2, C++20. Reproduce with `scripts/bench.sh`.

**Read these as relative comparisons, not as absolutes.** WSL2's syscall path is
several times a native kernel's, and run-to-run variance on this host was
measured at roughly +/-40%. Every figure below is the median of repeated trials,
with the observed spread printed alongside by the tools themselves.

---

## The floor: what an IPC round trip costs here

Before attributing anything to our own code, measure what the machine charges
for the syscalls underneath it. These probes link none of the project.

| Probe | Round trip |
|---|---:|
| `lrd_baseline_pingpong` - one `write` + one `read` per side | ~84 us |
| `lrd_baseline_framing_shape single` - same, 40-byte messages | ~88 us |
| `lrd_baseline_framing_shape split` - two reads per message (our shape) | ~104 us |

The third row is the cost of `read_frame` issuing a separate read for the length
prefix: about **16 us per round trip, ~18%**. That is the price of validating a
length before allocating against it, and it is the first thing a buffered reader
would reclaim. Documented in `protocol.md` rather than fixed, because the check
it pays for is the one that stops a peer from asking us to allocate 4 GiB.

---

## End to end: client -> socket -> daemon -> locked cache

`lrd_bench`, 10000 keys with Zipf theta 0.99, 128-byte values, 90% reads,
5000 measured requests per thread after 1000 warmup, median of 3 trials.
The daemon ran with 16 worker threads so that even the 16-client point had a
worker each.

| threads | ops/sec | p50 us | p90 us | p99 us | p99.9 us | max us |
|--------:|--------:|-------:|-------:|-------:|---------:|-------:|
| 1 | 9,615 | 94.1 | 133.5 | 223.2 | 311.2 | 982 |
| 2 | 19,790 | 92.9 | 123.6 | 211.4 | 416.1 | 953 |
| 4 | 28,867 | 117.7 | 201.6 | 456.0 | 1,190 | 3,309 |
| 8 | 63,212 | 26.5 | 119.5 | 351.7 | 1,399 | 5,578 |
| 16 | 143,293 | 45.7 | 113.5 | 628.6 | 3,729 | 11,871 |

Two things to read off this.

**Single-threaded p50 of 94 us against an 88 us IPC floor.** Framing, codec,
hash lookup, list splice, lock and counters together cost about 6 us - within
the noise of the measurement. The daemon is not where the time goes.

**Throughput scales roughly 15x from 1 to 16 client threads.** That is expected
precisely *because* the bottleneck is per-connection syscall latency rather than
anything shared: each connection spends its time blocked, so adding connections
adds parallelism almost for free. The shared cache lock never becomes the
constraint at this scale.

The p50 dropping at 8 and 16 threads is a real, repeatable effect and not a
measurement error: with more runnable threads, a worker is more often already
running when data arrives, so the request avoids a sleep/wake round trip. The
tail moves the other way, which is the honest cost of that.

---

## In process: the same cache with no sockets in the way

`lrd_cache_bench` drives the identical `LockedCache` directly from N threads.
Same workload shape, 300000 operations per thread, median of 3 trials.

| threads | ops/sec | p50 ns | p99 ns | p99.9 ns | hit rate |
|--------:|--------:|-------:|-------:|---------:|---------:|
| 1 | 2,772,177 | 200 | 700 | 4,200 | 83.9% |
| 2 | 1,740,090 | 400 | 11,500 | 44,500 | 88.0% |
| 4 | 1,026,483 | 700 | 58,500 | 153,000 | 90.0% |
| 8 | 618,791 | 1,600 | 160,400 | 438,700 | 90.8% |
| 16 | 670,339 | 5,300 | 282,300 | 914,500 | 91.0% |

**Throughput falls to 22% of single-threaded as threads increase**, and p99
rises from 700 ns to 160 us - a factor of 230. Adding threads makes this cache
strictly worse.

This is a mutex convoy. Every thread must serialise through one lock, so beyond
one thread the extra threads add no work, only handoff cost: each acquisition
that finds the lock held becomes a futex sleep and a wake, and the cache line
holding the mutex ping-pongs between cores. The Zipf skew concentrates access on
a few hot keys, which is realistic and makes it worse.

---

## The two tables together

This is the point of measuring both, and the reason `lrd_cache_bench` exists as
a separate tool:

* The lock is **catastrophic** for the cache in isolation - a 4.5x throughput
  loss and a 230x tail.
* The lock is **invisible** end to end, because one IPC round trip (~90 us) is
  roughly 300x one uncontended cache operation (~300 ns).

At the 16-thread end-to-end peak the daemon asks the cache for ~143k ops/sec.
The cache delivers ~670k ops/sec at its *worst* measured point. There is about
4x headroom, so the lock is nowhere near the end-to-end constraint.

**The honest prediction for step 7: sharding the cache will improve
`lrd_cache_bench` substantially and will not move `lrd_bench` at all.** Both
results will be reported. Sharding a component that is at 20% utilisation is a
fix for a problem this system does not currently have - it is worth doing here
to demonstrate the technique and to establish headroom for a workload where IPC
is cheaper (a native kernel, or a batched protocol), but claiming it made the
daemon faster would be false.

---

## Coordinated omission

`lrd_bench` is closed-loop by default: it sends request i+1 only after i
completes. That systematically understates tail latency. If the server stalls
for 10 ms, a closed-loop client simply sends fewer requests during the stall, so
the stall contributes one slow sample - where a real client with a fixed arrival
rate would have accumulated hundreds of delayed requests.

`--rate` switches to open loop: requests are scheduled at fixed intervals and
latency is measured from when a request was *due*, not from when it was sent.
Eight threads against the same daemon:

| Mode | ops/sec | p50 us | p99 us | p99.9 us |
|---|---:|---:|---:|---:|
| Closed loop | 57,479 | 30.0 | 899 | 5,736 |
| Open loop, 2000 req/sec/thread | 15,999 | 259.3 | 9,237 | 22,833 |

**A caveat that has to come with those numbers.** Measuring from the due time
charges any oversleep by the scheduler to the server. `lrd_baseline_timer`
measures that directly: `sleep_until` on this host overshoots a 500 us interval
by ~110 us at p50 and ~230 us at p99.

So the open-loop **p50 of 259 us is mostly timer granularity**, not queueing.
The **p99 of 9.2 ms is not** - a 230 us scheduling error cannot account for a
10x gap against the closed-loop p99. That difference is genuine queueing delay
that closed-loop measurement hides, which is exactly what the correction exists
to expose.

---

## How these measurements avoid measuring themselves

* **Per-thread latency vectors, merged after the clock stops.** A shared
  mutex-protected histogram would contend once per operation, and the contention
  would grow with thread count - biasing the very curve being drawn.
* **Storage reserved up front**, so a vector reallocation never lands inside the
  timed region and appears as a fake tail sample.
* **Keys precomputed**, so no allocation or integer formatting happens on the
  hot path.
* **Warmup excluded**, because a fresh connection pays one-off costs for buffer
  growth, page faults and a cold cache.
* **Counters folded in once per thread at the end**, not incremented per
  operation - an atomic in the inner loop is itself a contention point.
* **Nearest-rank percentiles, not interpolated.** "p99 = 214 us" should mean a
  request really took 214 us, not that two neighbouring samples averaged to it.
* **`steady_clock`, never `system_clock`**, which can jump backwards under NTP.
