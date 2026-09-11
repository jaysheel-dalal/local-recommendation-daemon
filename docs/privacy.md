# Privacy: noise on exported metrics

The daemon's counters are exact internally. When `--privacy` is enabled, the
copy that leaves the process is perturbed with calibrated noise.

---

## What the guarantee actually is

**Event-level differential privacy, not user-level.** The distinction is not a
technicality, and getting it right is most of what makes this a mechanism rather
than a gesture.

Differential privacy normally bounds what an observer learns about one
*individual's* contribution to an aggregate. That framing assumes many
individuals contribute to one counter. Here the daemon runs on one person's
device, so every increment is that same person's: their total contribution to
`recommends` **is** the entire value of `recommends`. Bounding that would mean
publishing nothing at all.

What the mechanism does provide, stated precisely:

> An observer of an exported counter cannot confidently determine whether any
> **particular event** occurred.

Did this person ask for a recommendation in the last hour? Was this item served?
Those are the questions the noise answers "cannot tell" to, and they are the
questions that leak behaviour. Claiming user-level DP on a single-user device
would be false; event-level is what the arithmetic supports.

## The mechanism

```
published = suppress(round(clamp(exact + Laplace(0, sensitivity/epsilon))))
```

| Parameter | Default | Meaning |
|---|---|---|
| `epsilon` | 1.0 | Smaller means more noise. Per metric, per epoch. |
| `sensitivity` | 1.0 | How much one event can move the counter. One request increments by exactly 1. |
| `suppression_threshold` | 5 | Noisy values at or below this publish as 0. |
| `rounding` | 10 | Published values are a multiple of this. |
| `epoch` | 1 hour | How long one draw is reused. |

**Sensitivity is stated rather than assumed**, because it is the term that makes
the scale meaningful. A counter that moved by 10 per event would need ten times
the noise for the same guarantee, and a mechanism that ignored that would be
calibrated to nothing.

Laplace is drawn by inverse transform. `u` is clamped away from the open
endpoints of its uniform range, because the formula diverges there and
`uniform_real_distribution`'s range is half-open — so one endpoint is reachable
and a single unlucky draw would publish an infinite counter.

## Noise goes on the boundary, never the source

The daemon's counters stay exact; only the copy going out is noised. Two
reasons:

* **Debuggability.** An operator chasing a hit-rate problem needs the real
  number. Noising in place destroys that permanently.
* **Compounding.** Noise applied repeatedly to a running total accumulates, so a
  counter noised on every increment drifts further from the truth over time
  rather than being perturbed once.

`Handler::stats()` returns the exact figures for in-process use;
`Handler::published_stats()` is what goes on the wire.

## The averaging attack, and why the epoch exists

If every read drew fresh noise, an observer could poll the endpoint a thousand
times and average. The noise cancels, the true value emerges, and the mechanism
has protected nothing while looking exactly as though it had.

So `publish` is a **pure function of (seed, metric, epoch, value)**. Ask twice
within an epoch, get the same number:

```
### the averaging attack: 12 reads of the same counter, same epoch
40 40 40 40 40 40 40 40 40 40 40 40
```

It is implemented as a hash rather than a cache — no map, no eviction, no lock,
and identical across daemon restarts within the same epoch. That last property
matters: a daemon that redrew on restart would hand a fresh sample to anyone who
could cause one.

For the same reason the metric name is hashed with FNV-1a rather than
`std::hash<std::string_view>`, which libstdc++ seeds per process in some
configurations — that would reintroduce exactly the restart-and-resample hole.

### The budget still accumulates

A new epoch is a new independent draw, so an observer sampling across N epochs
gets N independent observations and the effective parameter is **N × epsilon**. A
one-hour epoch watched for a day is 24× the stated epsilon.

That is inherent to publishing a changing quantity over time. A real deployment
would budget it explicitly — a fixed total epsilon per day, after which the
endpoint stops answering. This implementation does not, and that is a stated gap
rather than an oversight.

## Which counters are noised

Split by **what the number derives from**, not by how sensitive it feels:

| Noised (behavioural) | Exact (catalogue, config or operational) |
|---|---|
| `requests`, `gets`, `recommends` | `puts`, `deletes`, `evictions` |
| `hits`, `misses` | `entries`, `capacity`, `policy_tracked` |
| `policy_allowed` | `policy_store_full` |
| `policy_exposure_blocked`, `policy_frequency_blocked` | |

A counter that moves because the person using this device did something is
behavioural. `puts` and `deletes` are what an advertiser or content pipeline did,
and `evictions` follows from them — noise there would cost accuracy and buy no
privacy.

`policy_store_full` is exact for a different reason: it is an **operational
alarm**. An alarm that reads zero because noise suppressed it is worse than the
small leak about capacity pressure, and what it leaks is about the daemon rather
than a person.

**The noised counters no longer add up**, and that is deliberate. `requests` is
noised independently of its components, so it will not equal their sum. Forcing
consistency would mean deriving one from the others, and that correlation is
itself a channel — two noisy values constrained to sum to a third leak more than
three independent ones.

## Post-processing is free

Clamping to non-negative, rounding, and suppression all happen *after* the noise.
That is safe by the **post-processing property** of differential privacy: no
function of a DP output can weaken its guarantee, because the function never
touches the original data.

**Suppression tests the noisy value, never the exact one.** This is the part most
likely to be got wrong. Suppressing on the exact count would leak precisely what
the mechanism hides — a reader seeing zero would learn the true count was below
the threshold, a sharp statement about the data that no amount of noise elsewhere
undoes. There is a test whose whole point is that a count just above the
threshold is *sometimes* suppressed and sometimes not; that inconsistency is the
ambiguity we want, and a mechanism suppressing on the exact value would be
perfectly consistent, which is the leak.

Rounding is not itself a privacy mechanism — it is deterministic, so repeated
observation defeats it. Applied after noise it costs nothing and removes the
spurious precision of a figure like 40 237 that invites a reader to trust its
last digits.

Worth knowing when reading the output: at small counts the default rounding of
10 swallows noise of ±1–3 entirely. The demo switches rounding off to show the
raw draw.

```
exact recommends = 40; published: 44 42 39 38 45 41 38 41 43 38
```

---

## Limitations, stated rather than buried

These are the things that would come up in any serious review, and none of them
is fixed here.

**1. `std::mt19937_64` is not cryptographically secure.** Given enough output an
observer can recover its state and predict every subsequent draw — and here the
"output" is the published counters themselves. A deployment defending against an
adversary who can observe many metrics wants a CSPRNG and a secret seed. The seed
in this implementation is explicitly not a secret.

**2. Floating-point Laplace leaks through its bit representation.** Mironov
(2012), *On Significance of the Least Significant Bits for Differential Privacy*,
showed that naive inverse-transform sampling in floating point produces outputs
whose low-order bits reveal information about the input, breaking the formal
guarantee regardless of epsilon. The fix is a discrete mechanism — the discrete
Laplace (geometric) or snapping — sampled over integers. This implementation uses
the naive form, so its guarantee is the *intended* one rather than a proven one.

**3. No budget accounting across epochs.** See above: N epochs is N × epsilon,
and nothing here stops an observer from collecting them.

**4. Counters are noised, but the recommendation results themselves are not.**
An observer who can call `recommend` directly learns far more than the metrics
would ever reveal. The mechanism protects the *exported aggregates*, which is the
scope it claims — but the socket is mode 0600 for a reason, and that permission
is doing more privacy work here than the noise is.

The point of listing these is that a mechanism whose limits are known is worth
more than one whose limits are unexamined. Any of them could be closed; none of
them is closed by pretending otherwise.
