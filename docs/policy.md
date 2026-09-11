# Compliance: exposure caps and frequency limits

Two limits are enforced before any item can be returned:

* **Exposure cap** — the most times one item may *ever* be shown.
* **Frequency limit** — the most times one item may be shown within a rolling
  window.

Both default to 0, meaning unlimited, so nothing changes for a deployment that
does not opt in.

---

## Where the state lives, and why that was the whole question

### It is per-item, not per-user — and that is a feature, not a shortcut

A frequency cap is normally specified per user: "at most 3 shows per user per
hour". That needs a user identifier, and an identifier is precisely what the
signal deliberately does not carry.

The tension dissolves once you notice where this code runs. **The daemon serves
one device, so per-device and per-user are the same scope.** The identity is
implicit in the process boundary rather than carried in a field — which means the
cap works without the daemon ever learning, storing, or being able to leak who it
is capping.

A server-side ad system cannot do this. It is a property of being on-device, and
it is the clearest example in the project of privacy-by-architecture paying for
itself rather than costing something.

### Not a field on the cached Item

Cached values are `shared_ptr<const Item>` — immutable once published, which is
exactly what makes it safe for a reader to keep one after the cache lock is
released (step 3) and for a ranking scan to hold thousands of them while
unlocked (step 9). Counters must mutate. Putting a mutable counter inside that
value would either break the immutability every reader depends on, or require
per-item atomics, which cannot be copied.

### A separate structure, sharded the same way

`ExposureStore` is its own sharded map, using the same `hash_mix` and the same
shard count as the item cache — so an item and its counters land in
corresponding shards. The mixing was extracted to `include/lrd/common/hash_mix.hpp`
in this step, because two copies of a subtle correctness fix is one copy too
many. (`std::hash<uint64_t>` is the identity function in libstdc++; masking its
low bits without mixing would put every sixteenth item id in shard 0.)

### The two locks are never held at once

This is what keeps the design free of lock-ordering rules:

```
rank:     take cache shard lock -> copy pointers -> release   (step 9)
score:    no locks held
reserve:  take policy shard lock -> check and increment -> release
```

Sequential, never nested. There is no ordering to get wrong, so there is no
deadlock to reason about — which is a much better property than a documented
ordering that a future change might violate.

---

## The race this exists to close

"Check the count is below the cap, then increment it" is a textbook
check-then-act. Two threads both read `cap - 1`, both increment, and the cap is
exceeded.

`reserve()` therefore does both **under one shard lock**, and is deliberately a
single call rather than a `check()` followed by a `record()`. An API that let a
caller split them would make the race available to anyone who used it that way.

`test_exposure_store` drives it hard: 8 threads × 500 attempts against a cap of
500, asserting the total allowed is *exactly* 500. `test_handler` repeats it end
to end through the request path. Both run clean under ThreadSanitizer.

`check()` exists for the `dry_run` path and its answer is **advisory by
construction** — another thread may take the last slot a nanosecond later. That
is inherent to asking "would this be allowed" without taking it, and it is
documented on the method rather than left for a caller to discover.

---

## The frequency window

A sliding-window **counter**, not a sliding log and not a fixed bucket.

| Approach | Memory | Boundary behaviour |
|---|---|---|
| Sliding log (timestamp per show) | O(shows) per item | Exact |
| Fixed bucket | O(1) | **Allows 2× the limit** across a boundary |
| Sliding-window counter (chosen) | O(1) | Bounded, small error |

The fixed-bucket failure is worth being concrete about: with a limit of 4 per
hour, four shows at 10:59 and four more at 11:01 all pass, so eight land inside a
two-minute span.

This keeps two counters — the current window and the previous one — and estimates
the rolling count by weighting the previous window by how much of it is still in
view:

```
estimate = previous × (1 − elapsed_in_current / window) + current
```

Immediately after a boundary the previous window still counts almost in full, so
the burst above is refused. As the window advances the old count ages out
smoothly. This is the algorithm most production rate limiters use, for exactly
these reasons.

Admission asks `estimate + 1 > limit`, not `estimate >= limit`: the question is
whether admitting *this* show would exceed the limit, not whether the limit is
already met.

A test fills the allowance one millisecond before a boundary, checks that nothing
is admitted one millisecond after, and then samples across the following window
to confirm the allowance returns monotonically rather than in a step.

### Clock movement

`system_clock` is not monotonic — an NTP correction can move it backwards. A
backwards jump is treated the same as "many windows have passed", which resets
the window counters. That is only safe because the **lifetime cap is unaffected**:
a clock jump can loosen a frequency limit briefly, but it cannot let an item past
its exposure cap. A test pins that.

---

## Why the gate runs *during* selection

The policy check is a callback consulted by the re-ranker, not a filter applied
to a finished slate. Two properties depend on that, and neither survives being
done afterwards:

**The slate still fills.** Rejecting the third-best candidate lets the fourth
take its place, because selection has not finished. A filter over a finished list
of k would simply return k−1, so one capped item would quietly shrink every
response.

**Only what is returned is charged.** The callback runs for the handful of
candidates actually chosen, not for every candidate scored. An item that loses on
relevance is not charged an exposure for having been considered — which, if it
were, would burn caps at the rate of the inventory rather than the slate.

Diversity counts are also updated only for accepted items: penalising a
category for a slot it never occupied would distort the rest of the slate.

---

## Deleting an item does not clear its counters

`DeleteItem` deliberately does **not** call `forget()`.

If it did, any client could reset a lifetime exposure cap by deleting the item and
publishing it again — and the cap would be decorative. Counters therefore outlive
the items they govern, and `max_tracked_items` is what reclaims them. There is a
test for this specific exploit.

`forget()` exists for an explicit administrative reset, and for tests.

---

## Fail-closed when the store is full

The store is bounded. When it is full and a **new** item asks to be shown:

* **Fail-closed (default):** refuse the new item. `Decision::StoreFull`.
* **Fail-open (`--policy-fail-open`):** evict the least recently used counters
  and allow.

The default is asymmetric on purpose. Under-showing is a recoverable revenue
problem; forgetting a cap and over-showing is a compliance breach. When the two
conflict, the safe direction is to stop. Items already tracked keep working —
they are not collateral damage — and the refusal is counted so it can be alarmed
on.

Fail-open exists because "never serve anything again" is also a real failure, and
which one is worse is a deployment decision rather than a code one.

### The honest limitation

**A lifetime cap enforced from memory is not durable.** Restart the daemon and
every counter is gone. A bounded in-memory store makes over-exposure *unlikely*,
not impossible, and no amount of care inside this process changes that.

A production system would persist exposure counters — a small write-ahead log or
an embedded key-value store — and treat the in-memory map as a cache over it.
That is out of scope here, and saying so is better than implying the bound is
compliance-grade. What this implementation does get right is the part that is
genuinely hard: making check-and-reserve atomic under concurrency, and being
explicit about which direction it fails in.

---

## Decisions are reported separately

`Decision` has four values rather than being a bool, because the daemon reports
them as separate counters and they mean different things operationally:

| Counter | What it tells an operator |
|---|---|
| `policy_allowed` | Normal traffic. |
| `policy_exposure_blocked` | Inventory is exhausted — publish more, or raise caps. |
| `policy_frequency_blocked` | Tuning: the window or the limit is too tight for demand. |
| `policy_store_full` | **An alarm.** Capacity is being refused, not tuned. |

A slate short because everything hit its frequency limit is a configuration
question. One short because the store is full is an operational fault, and
collapsing the two into "blocked" would hide it.
