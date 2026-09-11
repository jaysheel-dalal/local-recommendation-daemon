# Candidate ranking

The daemon holds ad-like items and answers `Recommend` with a ranked, filtered
slate produced from a local user signal.

---

## What leaves the calling app

Only a `UserSignal`: a handful of category weights and a list of excluded
categories. No item ids the user has seen, no timestamps, no identifiers, no
history. The app derives those weights on-device from whatever it knows and
sends the summary.

That is the shape the phrase "privacy by architecture" is meant to describe here.
The sensitive data has no path to the daemon at all, rather than being sent and
then protected. Step 12 adds noise to what the daemon *exports*; this step is
about what it never receives.

## The item model

Every field is used by the scorer. A struct carrying decorative fields invites the
question "what is this for", and the honest answer would be "nothing".

| Field | Role |
|---|---|
| `id` | Cache key. `uint64`, which makes step 7's hash mixing load-bearing — `std::hash<uint64_t>` is the identity function in libstdc++, so masking its low bits would pile sequential ids into a few shards. |
| `category` | Matched against the signal's affinities; one of the two diversity axes. |
| `advertiser` | The second diversity axis. Three ads from one advertiser is a bad slate even when each is individually well matched. |
| `base_score` | Quality × bid, already normalised upstream. Non-positive means "switched off" without deleting. |
| `created_at` | Drives recency decay. |
| `expires_at` | Hard eligibility bound. The epoch means "never expires" — the common case, which costs nothing on the wire under protobuf. |

## The score

```
relevance = base_score  ×  affinity(category)  ×  recency(age)
```

then, during selection:

```
adjusted = relevance × category_repeat_factor^(times that category already picked)
                     × advertiser_repeat_factor^(times that advertiser already picked)
```

### Why the terms multiply rather than add

1. **A single zero eliminates the candidate.** That is the right behaviour for
   eligibility-shaped terms. With addition, a high base score papers over zero
   affinity — exactly the failure that shows people ads for things they have no
   interest in.
2. **Scale invariance.** Doubling every base score changes no ordering. Under
   addition, each term's influence depends on the units the others happen to use,
   so retuning one silently retunes the rest.
3. Loosely, the terms are independent probabilities of the same event — "is this
   worth showing" — and independent probabilities multiply.

The cost is that scores get small: three factors below 1 compound quickly. With
four or five terms this would want accumulating in log space (a sum of logs) to
stay clear of denormals. At three terms in double precision it is nowhere near an
issue, and log space would cost readability for nothing.

### Why unknown categories get 0.1 and not 0

A zero default would make the affinity term annihilate every category the signal
has never mentioned. A user with one recorded interest could then only ever be
shown that one category — no discovery for them, and inventory that can never be
reached for everyone else. A small positive floor keeps unfamiliar items eligible
but firmly behind anything the signal endorses.

### Why exponential decay and not a cutoff

A cutoff ("nothing older than 7 days") puts a cliff in the ranking: an item at 6
days 23 hours outranks everything, and an hour later it is gone. That produces
visible churn at the boundary and changes the whole slate at once.

`exp(-ln2 × age / half_life)` is 1.0 at age zero, exactly 0.5 at one half-life,
and approaches zero without reaching it — so an old-but-excellent item stays
reachable instead of being deleted by its age. One intuitive parameter.

An item created in the *future* (clock skew between the publishing process and
this one is normal) is clamped to age zero rather than given a bonus.

### Why the diversity penalty is geometric

A flat penalty would discourage the second and the fifth item of a category
equally. A hard cap would make the slate suddenly unfillable when inventory is
thin. Geometric decay — halve, then quarter — lets a category still fill every
slot if nothing else is close, while always preferring variety when it is.

The advertiser factor (0.3) is stricter than the category factor (0.5) because
several ads from one advertiser looks broken in a way that several from one
category does not.

## Eligibility is separate from scoring

Excluded categories, expiry, and a non-positive base score are checked *before*
any arithmetic. They are rules, not preferences, and mixing them into a numeric
score would make a guarantee depend on tuning. A test pushes the category penalty
to 0.01 — brutal pressure to diversify — and asserts that an excluded category
still never appears.

## Two-stage selection

The diversity penalty depends on **what has already been picked**, so it cannot be
precomputed and the answer cannot be produced by sorting.

| Stage | What it does | Cost |
|---|---|---|
| Retrieval | Scores each candidate independently, keeps the best `count × overfetch` in a bounded min-heap | O(n log m), **O(m) memory** |
| Re-rank | Greedy, order-dependent diversity selection over those m | O(m·k), m = 4k |

This is how real ranking pipelines are shaped, for the same reason: a cheap pass
over everything, then an expensive pass over a shortlist.

The min-heap holds the **weakest** retained candidate at its root, so one
comparison decides whether a new candidate belongs at all. In a large inventory
almost every candidate is rejected by that single comparison, and only the
survivors pay log m.

Ties break on item id, deterministically. Without it, two items with equal
relevance would be ordered by whichever shard happened to be scanned first —
which varies with the hash, making results unreproducible and untestable. A test
offers candidates in ascending and descending order and asserts the answers match.

## Scanning the cache without destroying it

Ranking has to visit every item. Doing that through `get()` would mark the whole
cache most-recently-used on every recommendation, and eviction would become
effectively random — the cache would still work and quietly stop being a cache.

So the scan uses a `const` traversal that does not touch recency, and it goes
**shard by shard**: each shard's lock is held only long enough to copy out keys
and refcounted pointers, then released before any scoring happens. Scoring
thousands of candidates under a cache lock would serialise every other worker
behind one request.

Copying pointers is cheap because values are `shared_ptr<const Item>` — the return
type chosen back in step 3 so a value outlives the lock. It pays off here a second
time: an item evicted mid-scan stays alive for as long as the scan holds its
pointer.

The consequence, stated rather than hidden: this is **not a consistent snapshot**.
Shards are read at different instants, so a write during the scan may be seen or
missed. For ranking that is fine — a candidate set a few milliseconds stale is not
a correctness problem — and the alternative is holding every lock at once.

`test_handler` pins the recency property directly: a capacity-4 cache, a
recommendation that scans all four items, then an insert. The item that was least
recently used *before* the scan must still be the one evicted.

## What is deliberately not here

* **No ML.** A scoring heuristic with explainable terms is worth more in this
  context than a model nobody can interrogate, and every parameter above can be
  justified in a sentence.
* **No exploration term.** Randomised exploration is what a real system would add
  next, and it would make results non-deterministic — so it would need a seeded
  generator and a way to pin it in tests before it could go in.
* **No per-user state.** The daemon holds items, not users. Step 10 changes that
  for frequency capping, and the question of where that state lives is the main
  design decision of that step.
