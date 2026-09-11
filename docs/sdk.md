# The client SDK

`lrd::sdk::Client` is what an application links against. `lrd::client::Connection`
is what it is built on, and applications are not expected to touch it.

---

## What separates the two

`Connection` is one request/response stream over one socket. It is deliberately
**not** thread-safe: interleaving two threads' frames on one stream would
desynchronise the protocol, and no amount of care at the call site fixes that.

Everything the SDK adds exists because a caller would otherwise have to build it:

| Concern | What a caller would have to do | What the SDK does |
|---|---|---|
| Sharing across threads | Keep a connection per thread, or wrap one in a mutex and serialise everything | Pools connections; `Client` is safe to share |
| Hanging | Nothing — the transport blocks forever by default | Bounds every wait |
| Reconnecting | Detect a dead connection, discard it, open another | Drops failed connections and reopens |
| Retrying | Decide which operations are safe to repeat | Classifies each operation, and gets the compliance case right |
| Error handling | Catch exceptions from the transport | Returns a `Status` |
| Coupling | Include the protocol and transport headers | Four public headers, nothing else |

## The API surface is four headers

```
lrd/sdk/client.hpp     Client, ClientConfig, Stats
lrd/sdk/status.hpp     Status, StatusCode
lrd/rank/item.hpp      Item, ItemId, RankedItem, Timestamp
lrd/rank/signal.hpp    UserSignal, CategoryAffinity
```

No socket, no codec, no framing, no protobuf, no protocol type. `Client` holds a
`unique_ptr<Impl>` whose definition never leaves `client.cpp`, so the wire format
can change without recompiling a consumer.

**This is verified rather than asserted.** `scripts/verify-sdk-install.sh`
installs the SDK to a throwaway prefix, then builds `examples/recommender_app.cpp`
from a separate directory using `find_package(lrd)` with no access to this source
tree. If the SDK leaks an internal header, forgets to install one it needs, or
exports a target carrying a source-tree path, that script fails.

Two export bugs it caught while being written:

* The build-tree `ALIAS lrd::sdk` is **not** exported. An installed consumer got
  `lrd::lrd_sdk` — the namespace prefixed onto the raw target name. `EXPORT_NAME`
  is the separate property that fixes it, and having one without the other is
  entirely possible.
* `lrd_core`'s include directory was a bare source path, which CMake refuses to
  export. `BUILD_INTERFACE`/`INSTALL_INTERFACE` exist for exactly this.

`Stats` is copied field by field into an SDK-owned struct rather than
re-exporting `proto::Stats`. The two are identical today, which is precisely why
they must be separate types: otherwise every future protocol field becomes an API
change, and anyone wanting a counter needs the protocol header.

---

## Errors are returned, not thrown

Three reasons, in order of weight:

1. A library that throws across its boundary imposes an error-handling style on
   every caller, and transitively on theirs.
2. Exception types are ABI. Throwing `lrd::SystemError` would make that class's
   layout a compatibility surface — exactly what the pimpl exists to avoid.
3. "The daemon is not running" and "this item does not exist" are ordinary
   answers. Modelling them as throws makes the common path the one wrapped in a
   `try`.

`Status` is returned by value rather than stored as a `last_error()` member,
because `Client` is shared: a member would be mutable state written by every
call, so two threads failing at once would each read the other's message. That is
a data race that reports the *wrong* cause, which is worse than reporting none.

`Client::connect` is a factory returning a `Status`, not a constructor, because a
constructor has no way to report failure except by throwing. It is also the one
place in the SDK that catches — below that line the boundary is exception-free.

---

## The connection pool

Idle connections live in a deque used as a **stack**. Reusing the most recently
returned connection keeps the hot ones hot and leaves the cold tail idle, so a
burst that briefly needed eight connections does not keep cycling all eight
afterwards.

Checkout is RAII, and the destructor makes the decision the pool's correctness
rests on:

```cpp
if (healthy_ && connection_->connected()) {
    idle.push_front(std::move(connection_));   // reusable
} else {
    --total;                                    // dropped, and the slot freed
}
```

**A connection that failed must be dropped, never returned.** A failed call may
have left half a frame on the wire; recycling it would hand the next caller a
stream whose next read is the tail of someone else's message. Decrementing
`total` rather than just discarding means the pool reopens rather than shrinking
permanently after a blip — which is what makes the daemon-restart test pass.

`NotFound` and `InvalidArgument` leave the stream in a known state, so those
connections go straight back.

Every wait is bounded: a request timeout, an acquire timeout, and a bounded pool
size. An unbounded wait is a hang with extra steps.

---

## Timeouts

This step added `SO_RCVTIMEO`/`SO_SNDTIMEO`, a genuine gap — before it, the
transport blocked indefinitely and nothing in the client could interrupt it.

`EAGAIN`/`EWOULDBLOCK` becomes `IoStatus::TimedOut`, deliberately **not**
retried inside `read_exact`. The point of a timeout is that the caller wanted to
stop waiting; looping there would silently restore the unbounded wait it was
configured to avoid.

**A timeout is always fatal to that connection, even between frames with nothing
consumed.** The daemon may still be about to send the reply we gave up on:
reusing the connection would read that stale reply as the answer to the *next*
request, and every reply after it would be off by one. Hanging up is the only way
to be sure.

The daemon sets no timeouts, and that is correct for it: a worker blocked on the
client it is dedicated to has nothing better to do, and shutdown interrupts it
through `ConnectionRegistry` instead.

---

## Which operations are retried, and why that is a compliance question

| Operation | Retried | Reason |
|---|---|---|
| `get_item`, `stats`, `preview` | Yes | No side effects. |
| `put_item` | Yes | Idempotent — the state after one and after two is identical. |
| `delete_item` | Yes | Idempotent in *state*. See the caveat below. |
| `recommend` | **No** | Records exposure. See below. |

**`recommend` is the interesting one.** A non-dry-run recommendation reserves
exposure against every item it returns (step 10). If the connection dies after
the daemon processed the request but before the reply arrived, the work is
already done — and a retry charges those caps a second time, quietly eroding the
guarantee the caps exist to provide.

That is the classic **at-least-once versus at-most-once** choice, and without
deduplication on the daemon side an SDK cannot have both. It picks at-most-once
for the operation with a side effect: a lost recommendation is reported as
`Unavailable` and the caller decides. `retry_recording_recommendations` inverts
it for callers who would rather have availability, with the cost stated.

**What would let you have both** is request-id deduplication in the daemon: the
client reuses the same `request_id` on a retry, and the daemon caches recent
(id → response) pairs and replays rather than re-executing. `request_id` is
already on the wire and already checked for mismatch. What is missing is the
cache, and it is not free — it needs client-unique ids (a connection-local
counter restarts at 1 on reconnect, which is exactly when a retry happens), a
bounded store with its own eviction policy, and a decision about how long an id
stays deduplicable. That is a step's worth of work, and pretending a retry flag
substitutes for it would be the wrong kind of confident.

**The `delete_item` caveat**, stated because it is a real if minor wart: deleting
is idempotent in state but not in *reported status*. A retry of a delete that
already succeeded reports `NotFound`. The SDK prefers that over leaving the item
behind.

---

## Backoff

Exponential, starting at 10 ms and doubling. A fixed delay turns a daemon restart
into a synchronised stampede from every client at once; doubling spreads them
out.

Production clients add jitter for the same reason. It is omitted here because a
single-device daemon has no herd to synchronise — and adding it would make the
retry timing non-deterministic, which is a cost with no matching benefit at this
scale.
