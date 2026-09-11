#pragma once

#include "lrd/rank/item.hpp"
#include "lrd/rank/signal.hpp"
#include "lrd/sdk/status.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lrd::sdk {

/// Counters describing the daemon's state, as returned by `Client::stats`.
///
/// A plain struct owned by the SDK rather than the protocol's `proto::Stats`.
/// The two happen to be identical today, and that is precisely why they must be
/// separate types: re-exporting the wire struct would make every future protocol
/// field an API change, and would put `<lrd/proto/message.hpp>` on the include
/// path of anyone who wanted a counter.
struct Stats {
    std::uint64_t requests = 0;
    std::uint64_t gets = 0;
    std::uint64_t puts = 0;
    std::uint64_t deletes = 0;
    std::uint64_t recommends = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t entries = 0;
    std::uint64_t capacity = 0;

    std::uint64_t policy_allowed = 0;
    std::uint64_t policy_exposure_blocked = 0;
    std::uint64_t policy_frequency_blocked = 0;
    std::uint64_t policy_store_full = 0;
    std::uint64_t policy_tracked = 0;
};

struct ClientConfig {
    std::string socket_path = "/tmp/lrd.sock";

    /// Must match the daemon's. There is no negotiation.
    std::string codec = "binary";

    /// Upper bound on how long any single call may block.
    ///
    /// Not optional in practice: the transport blocks indefinitely by default,
    /// and a library that can park a caller's thread forever is not shippable.
    /// A timeout mid-call always closes that connection - see the note on
    /// FrameStatus::TimedOut for why a half-read reply cannot be recovered from.
    std::chrono::milliseconds request_timeout{2000};

    /// Maximum connections held open. Each is one request/response stream, so
    /// this is also the maximum number of calls that can be in flight.
    std::size_t max_connections = 8;

    /// How long a caller waits for a connection when all are busy, before
    /// giving up with Unavailable. Bounded for the same reason as the request
    /// timeout: an unbounded wait is a hang with extra steps.
    std::chrono::milliseconds acquire_timeout{1000};

    /// Attempts after the first for operations the SDK considers safe to retry.
    /// 0 disables retrying entirely.
    std::size_t max_retries = 2;

    /// Delay before the first retry; doubled for each subsequent one.
    std::chrono::milliseconds retry_backoff{10};

    /// Retry a *recording* recommendation after a lost connection.
    ///
    /// **Off by default, and this is a correctness decision rather than a
    /// preference.** A non-dry-run `recommend` reserves exposure against every
    /// item it returns (step 10). If the connection dies after the daemon
    /// processed the request but before the reply arrived, the work is already
    /// done and a retry charges those caps a second time - quietly eroding the
    /// compliance guarantee the caps exist to provide.
    ///
    /// That is the classic at-least-once versus at-most-once choice, and with no
    /// deduplication on the daemon side the SDK cannot have both. It therefore
    /// picks at-most-once for the operation with a side effect: a lost
    /// recommendation is reported as Unavailable and the caller decides.
    ///
    /// Turning this on buys availability at the cost of occasional
    /// over-exposure. What would let you have both is request-id deduplication
    /// in the daemon - see docs/sdk.md.
    bool retry_recording_recommendations = false;
};

/// A thread-safe client for the daemon.
///
/// ## What makes this an SDK rather than a wrapper
///
///   * **It is safe to share.** One Client can be used from any number of
///     threads. The underlying Connection deliberately cannot - it is a single
///     request/response stream, and interleaving two threads' frames would
///     desynchronise the protocol - so the Client owns a pool and hands out one
///     connection at a time. Getting that right is work a caller should not have
///     to repeat.
///   * **It bounds every wait.** Request timeouts, an acquire timeout, and a
///     bounded pool, so no call can hang and no overload can grow without limit.
///   * **It knows which operations are safe to retry**, which turns out to be a
///     question about compliance rather than about networking. See
///     ClientConfig::retry_recording_recommendations.
///   * **It exposes no implementation.** This header names no socket, no codec,
///     no protobuf and no protocol type. Everything is behind a pimpl, so the
///     wire format can change without recompiling a consumer.
///
/// ## Lifetime and thread safety
///
/// Methods are safe to call concurrently. Destroying a Client while a call is in
/// flight on another thread is not - the usual rule that an object must outlive
/// its users applies here as it does everywhere.
class Client {
public:
    /// Connects and returns a ready client.
    ///
    /// A factory rather than a throwing constructor: connecting can fail for
    /// ordinary reasons (the daemon is not running), and a constructor has no
    /// way to report that except by throwing. `out` is left untouched on
    /// failure.
    [[nodiscard]] static Status connect(ClientConfig config, std::unique_ptr<Client>& out);

    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// Stores or replaces an item. Idempotent, so it is retried on failure.
    [[nodiscard]] Status put_item(const rank::Item& item);

    /// Fetches an item. Returns NotFound with `item` untouched if absent.
    [[nodiscard]] Status get_item(rank::ItemId id, rank::Item& item);

    /// Deletes an item. Returns NotFound if it was not there.
    ///
    /// Retried on failure: the *state* after one delete and after two is the
    /// same. The caveat worth knowing is that the reported *status* is not - a
    /// retry of a delete that already succeeded reports NotFound. The SDK
    /// prefers that over leaving the item behind.
    [[nodiscard]] Status delete_item(rank::ItemId id);

    /// Asks for up to `count` ranked items for `signal`.
    ///
    /// A recording call - each returned item is charged against its exposure and
    /// frequency caps - so by default it is **not** retried. See
    /// ClientConfig::retry_recording_recommendations.
    [[nodiscard]] Status recommend(const rank::UserSignal& signal, std::uint32_t count,
                                   std::vector<rank::RankedItem>& items);

    /// Ranks without recording anything. Free of side effects, so always
    /// retried.
    ///
    /// Useful for previewing a slate, and for asking "what would be served"
    /// without spending the caps to find out.
    [[nodiscard]] Status preview(const rank::UserSignal& signal, std::uint32_t count,
                                 std::vector<rank::RankedItem>& items);

    [[nodiscard]] Status stats(Stats& stats);

    /// Connections currently open. Diagnostic; the pool grows lazily, so this
    /// reflects peak concurrency rather than configuration.
    [[nodiscard]] std::size_t open_connections() const;

    /// Opaque implementation. Declared public only so the connection-pool
    /// helpers in the .cpp can name the type; the definition never leaves that
    /// translation unit, so nothing about it is part of the API.
    struct Impl;

private:
    Client();

    std::unique_ptr<Impl> impl_;
};

}  // namespace lrd::sdk
