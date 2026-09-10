# lrd wire protocol v1

A request/response protocol over a UNIX domain stream socket. One connection
carries many requests; each is answered by exactly one response.

All multi-byte integers are **big-endian** (network byte order). Strings are
length-prefixed and are *not* NUL-terminated.

---

## Why framing exists at all

`SOCK_STREAM` delivers a byte stream with no message boundaries. A single
`read()` may return part of a message, exactly one message, or three messages
and a fragment of a fourth — all without any error. The receiver therefore has
to be told where each message ends, and there are only three ways to do that:

| Approach | Verdict |
|---|---|
| Length prefix | Chosen. Constant-cost, handles binary payloads, bounds allocation before reading. |
| Delimiter (`\n`) | Requires escaping any payload byte that collides, and you cannot know the size in advance. |
| Fixed-size messages | Wastes space and caps the payload forever. |

## Frame layout

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+---------------------------------------------------------------+
|                     length  (uint32, BE)                        |  4 bytes
+---------------------------------------------------------------+
|                  magic 'L' 'R' 'D' '1'  (0x4C524431)            |  4 bytes  ─┐
+-------+-------+-----------------------------------------------+            │
|version| type  |            flags  (uint16, BE)                 |  4 bytes   │ header
+-------+-------+-----------------------------------------------+            │ 16 bytes
|                                                                 |            │
|                 request_id  (uint64, BE)                        |  8 bytes  ─┘
|                                                                 |
+---------------------------------------------------------------+
|                     payload (type-specific)                     |  length-16
+---------------------------------------------------------------+
```

* `length` counts **everything after the length field** — header plus payload.
  A frame with an empty payload has `length == 16`.
* `length` must be in `[16, 1048576]`. A frame claiming more is rejected
  without reading it; see *Bounding allocation* below.
* `magic` must be `0x4C524431`. It is checked on every frame rather than once
  per connection: if framing ever desynchronises, the very next frame fails
  loudly instead of being interpreted as garbage fields.
* `version` must be `1`. Unknown versions are rejected rather than guessed at.
* `flags` is reserved and must be `0` in v1. Receivers reject non-zero values
  so that a future flag cannot be silently ignored by an old daemon.
* `request_id` is chosen by the client and echoed verbatim in the response. It
  is unused while the daemon answers one request at a time, and becomes load
  bearing in step 5 when responses may complete out of order.

### Cost note

The 16-byte header is meaningful overhead on a 30-byte request. A production
protocol would validate magic and version once in a connection handshake and
use a leaner per-frame header. Per-frame magic is the deliberate v1 choice: it
detects desynchronisation at any point in the stream, which is worth more than
the bytes while the framing code is young. Step 6's benchmark will show whether
the overhead is measurable against syscall cost.

## Message types

The high bit distinguishes a response from a request, so a receiver can reject
a wrongly-directed message before parsing the payload.

| Value | Type | Payload |
|------:|------|---------|
| `0x01` | `GetRequest` | `string key` |
| `0x02` | `PutRequest` | `string key`, `string value` |
| `0x03` | `DeleteRequest` | `string key` |
| `0x04` | `StatsRequest` | *(empty)* |
| `0x81` | `GetResponse` | `uint8 status`, `string value` |
| `0x82` | `PutResponse` | `uint8 status` |
| `0x83` | `DeleteResponse` | `uint8 status` |
| `0x84` | `StatsResponse` | `uint8 status`, then 6 × `uint64` |
| `0xFF` | `ErrorResponse` | `uint8 status`, `string message` |

### Status codes

| Value | Name | Meaning |
|------:|------|---------|
| `0` | `Ok` | Succeeded. |
| `1` | `NotFound` | Key absent (`GetResponse`, `DeleteResponse`). |
| `2` | `InvalidRequest` | Malformed, or a limit exceeded. |
| `3` | `Internal` | The daemon failed for its own reasons. |

### `StatsResponse` fields

In order: `requests`, `gets`, `puts`, `deletes`, `hits`, `misses`,
`evictions`, `entries`, `capacity` — nine `uint64` values.

The last three were appended in step 3. Appending fields is a **breaking**
change under this protocol, because the decoder rejects trailing bytes: a step 2
client talking to a step 3 daemon gets `TrailingBytes` rather than silently
misreading the reply. That is the intended behaviour — version skew becomes a
loud, immediate failure instead of plausible-looking wrong numbers. Nothing is
deployed, so v1 was edited in place; a shipped protocol would have bumped
`kVersion` instead.

## Field encodings

| Type | Encoding |
|---|---|
| `uint8/16/32/64` | Big-endian, fixed width. |
| `string` | `uint32` byte count, then exactly that many bytes. No terminator. |

Limits, enforced by both encoder and decoder:

| Limit | Value |
|---|---|
| `kMaxFrameSize` | 1 MiB |
| `kMaxKeyLength` | 1024 bytes |
| `kMaxValueLength` | 512 KiB |

## Bounding allocation

A length prefix is an instruction from a peer to allocate memory, and an
unvalidated one is a denial-of-service primitive: four bytes of `0xFF` ask the
daemon to reserve 4 GiB before a single payload byte arrives.

Both checks below happen *before* any allocation:

1. The frame length is validated against `kMaxFrameSize` immediately after the
   4-byte prefix is read, and an oversized frame closes the connection rather
   than being skipped — a peer that sends one is either broken or hostile, and
   neither deserves to keep the connection.
2. Every string length is validated against the bytes actually remaining in the
   frame before the string is read, so a field claiming 900 KB inside a 40-byte
   frame is rejected rather than resized-to.

## Byte order

Encoding is done with explicit shifts rather than `htonl`/`memcpy` of a native
integer:

```cpp
out[0] = static_cast<std::byte>((value >> 24) & 0xFF);
out[1] = static_cast<std::byte>((value >> 16) & 0xFF);
...
```

Two reasons. `htonl` only covers 32 bits — the 64-bit spelling (`htobe64`) is a
BSD/glibc extension, not standard C or C++. And casting a buffer pointer to
`uint32_t*` to read it directly is undefined behaviour when the pointer is not
suitably aligned, which for a pointer into the middle of a frame it generally
is not. Shifts are portable, alignment-agnostic, and compile to a single
`bswap` on any compiler worth using.

## Errors and connection state

| Condition | Daemon behaviour |
|---|---|
| Unknown key | `status = NotFound`, connection stays open. |
| Key or value over the limit | `ErrorResponse` with `InvalidRequest`, connection stays open. |
| Bad magic, version, flags, or type | Connection closed. Framing cannot be trusted once the header is wrong. |
| Frame length out of range | Connection closed. |
| Truncated frame (peer vanished mid-message) | Connection closed. |

The distinction is deliberate: a *semantic* error is answered and the
connection continues, because the stream is still in a known state. A *framing*
error means the daemon no longer knows where the next message starts, and the
only safe move is to hang up.

---

## Measured cost, and one known inefficiency

Measured on this development machine (WSL2, Ubuntu 26.04, GCC 15.2, Release
build). WSL2 syscall overhead is high, so these numbers are useful as
*relative* comparisons, not as absolute figures for native Linux.

Baselines, from a bare two-process ping-pong with no framing and no codec:

| Shape | Round trip |
|---|---:|
| One `write` + one `read` per side | ~80 µs |
| One `write` + **two** `read`s per side (our shape) | ~111 µs |
| `lrd` full stack: framing + codec + store | ~105 µs |

Two things follow.

**The daemon is essentially at the floor for its syscall shape.** Framing,
encoding, decoding, the hash lookup and the counters together cost less than
the measurement noise on top of an equivalent raw C program. The work is not in
our code; it is in the syscalls.

**Reading the length prefix as a separate `read()` costs ~30 µs per round trip
here — about 28% of the total.** `read_frame` issues two reads: four bytes for
the prefix, then the body. That is what makes "validate the length before
allocating" straightforward, and it is why the check is trivially correct. The
fix is a buffered reader that pulls whatever is available into a reusable
buffer and parses frames out of it, so a request usually costs one `read`
rather than two — at the price of managing partially-consumed buffer state
across calls, which is exactly the kind of code that gets framing wrong.

That trade is deliberately deferred: correctness of the framing came first, the
cost is now measured rather than assumed, and step 6's benchmark is the right
place to decide whether it is worth paying for. It is recorded here so the
decision is visible rather than accidental.

The write path *was* optimised, because that one was free: `write_message`
reserves the four prefix bytes in the scratch buffer before encoding and
patches the length in afterwards, so the body is never copied and a warm
connection allocates nothing per message.
