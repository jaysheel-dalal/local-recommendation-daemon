#include "lrd/sdk/client.hpp"

#include "lrd/client/connection.hpp"
#include "lrd/common/errors.hpp"
#include "lrd/net/unix_socket.hpp"

#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace lrd::sdk {

const char* to_string(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Ok: return "Ok";
        case StatusCode::NotFound: return "NotFound";
        case StatusCode::InvalidArgument: return "InvalidArgument";
        case StatusCode::Unavailable: return "Unavailable";
        case StatusCode::DeadlineExceeded: return "DeadlineExceeded";
        case StatusCode::Internal: return "Internal";
        case StatusCode::ProtocolError: return "ProtocolError";
    }
    return "Unknown";
}

namespace {

/// Maps the transport-level outcome onto the SDK's vocabulary.
///
/// The two sets differ on purpose. `client::CallStatus` describes what happened
/// to a connection; `sdk::StatusCode` describes what a caller should do about
/// it. Collapsing ProtocolError and ConnectionLost into one "failed" would lose
/// the distinction between "retry might help" and "your two binaries disagree".
StatusCode map_status(client::CallStatus status) noexcept {
    switch (status) {
        case client::CallStatus::Ok: return StatusCode::Ok;
        case client::CallStatus::NotFound: return StatusCode::NotFound;
        case client::CallStatus::InvalidRequest: return StatusCode::InvalidArgument;
        case client::CallStatus::ServerError: return StatusCode::Internal;
        case client::CallStatus::ConnectionLost: return StatusCode::Unavailable;
        case client::CallStatus::ProtocolError: return StatusCode::ProtocolError;
    }
    return StatusCode::Internal;
}

/// Whether another attempt could plausibly succeed.
///
/// Deliberately narrow. InvalidArgument means the request itself is wrong, so
/// retrying it unchanged is pure waste; ProtocolError means the two ends
/// disagree about the wire format, which no amount of retrying repairs.
bool is_retryable_failure(StatusCode code) noexcept {
    return code == StatusCode::Unavailable || code == StatusCode::DeadlineExceeded;
}

}  // namespace

// --------------------------------------------------------------------------
// Impl
// --------------------------------------------------------------------------

struct Client::Impl {
    explicit Impl(ClientConfig configuration) : config(std::move(configuration)) {}

    ClientConfig config;

    mutable std::mutex mutex;
    std::condition_variable available;

    /// Idle connections, newest first.
    ///
    /// A deque used as a stack rather than a queue: reusing the most recently
    /// returned connection keeps the hot ones hot and lets the cold tail of the
    /// pool sit idle, which is what you want if connections are ever reaped.
    /// It also means a burst that briefly needed eight connections does not keep
    /// cycling all eight afterwards.
    std::deque<std::unique_ptr<client::Connection>> idle;

    /// Connections currently checked out plus those idle. Bounded by
    /// config.max_connections.
    std::size_t total = 0;

    /// Creates a connection with the configured timeouts applied.
    ///
    /// Timeouts are set on the socket *before* it is wrapped, because once a
    /// Connection owns the stream there is no supported way to reach the
    /// descriptor - which is the point of the encapsulation.
    std::unique_ptr<client::Connection> open_connection() {
        net::UnixStream stream = net::UnixStream::connect(config.socket_path);
        stream.set_timeouts(config.request_timeout, config.request_timeout);
        return std::make_unique<client::Connection>(std::move(stream), config.codec);
    }
};

/// RAII checkout from the pool.
///
/// The destructor decides where the connection goes, and that decision is the
/// pool's whole correctness argument: a connection that failed must be *dropped*
/// rather than returned, because a failed call may have left half a frame on the
/// wire. Returning it would hand the next caller a stream whose next read is the
/// tail of someone else's message.
namespace {

class PooledConnection {
public:
    PooledConnection(Client::Impl& impl, std::unique_ptr<client::Connection> connection)
        : impl_(impl), connection_(std::move(connection)) {}

    ~PooledConnection() {
        const std::lock_guard<std::mutex> lock(impl_.mutex);
        if (healthy_ && connection_ && connection_->connected()) {
            impl_.idle.push_front(std::move(connection_));
        } else {
            // Dropped, not recycled. `total` falls so the pool can open a
            // replacement rather than shrinking permanently after a blip.
            --impl_.total;
        }
        impl_.available.notify_one();
    }

    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;

    client::Connection& operator*() const noexcept { return *connection_; }

    void mark_failed() noexcept { healthy_ = false; }

private:
    Client::Impl& impl_;
    std::unique_ptr<client::Connection> connection_;
    bool healthy_ = true;
};

}  // namespace

// --------------------------------------------------------------------------
// Client
// --------------------------------------------------------------------------

Client::Client() = default;
Client::~Client() = default;

Status Client::connect(ClientConfig config, std::unique_ptr<Client>& out) {
    if (config.max_connections == 0) {
        return Status::make(StatusCode::InvalidArgument, "max_connections must be at least 1");
    }

    auto client = std::unique_ptr<Client>(new Client());
    client->impl_ = std::make_unique<Impl>(std::move(config));

    // One connection is opened eagerly, so that "the daemon is not running" is
    // reported here rather than on some later call. A lazily-connecting client
    // that reports success and then fails on first use is a worse API.
    try {
        auto first = client->impl_->open_connection();
        const std::lock_guard<std::mutex> lock(client->impl_->mutex);
        client->impl_->idle.push_front(std::move(first));
        client->impl_->total = 1;
    } catch (const SystemError& e) {
        // The one place an exception is caught and converted: below this line
        // the SDK's boundary is exception-free.
        return Status::make(StatusCode::Unavailable,
                            std::format("cannot reach daemon at {}: {}",
                                        client->impl_->config.socket_path, e.what()));
    } catch (const std::exception& e) {
        return Status::make(StatusCode::InvalidArgument, e.what());
    }

    out = std::move(client);
    return Status::success();
}

std::size_t Client::open_connections() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->total;
}

namespace {

/// Runs `call` against a pooled connection, retrying if the operation allows it.
///
/// Templated on the callable rather than taking a std::function so the compiler
/// can inline the body; this is the hot path for every SDK method.
template <typename Call>
Status invoke(Client::Impl& impl, bool retryable, Call&& call) {
    const std::size_t attempts = retryable ? impl.config.max_retries + 1 : 1;
    std::chrono::milliseconds backoff = impl.config.retry_backoff;
    Status last = Status::make(StatusCode::Unavailable, "no attempt was made");

    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0) {
            // Exponential backoff. A fixed delay turns a daemon restart into a
            // synchronised stampede from every client at once; doubling spreads
            // them out. Real production clients also add jitter for the same
            // reason - omitted here because a single-device daemon has no herd
            // to synchronise.
            std::this_thread::sleep_for(backoff);
            backoff *= 2;
        }

        std::unique_ptr<client::Connection> connection;
        {
            std::unique_lock<std::mutex> lock(impl.mutex);

            // Wait for an idle connection, or for room to open a new one.
            const bool ready = impl.available.wait_for(
                lock, impl.config.acquire_timeout, [&impl] {
                    return !impl.idle.empty() || impl.total < impl.config.max_connections;
                });

            if (!ready) {
                last = Status::make(StatusCode::Unavailable,
                                    "timed out waiting for a free connection");
                continue;
            }

            if (!impl.idle.empty()) {
                connection = std::move(impl.idle.front());
                impl.idle.pop_front();
            } else {
                ++impl.total;  // reserve the slot before releasing the lock
            }
        }

        if (!connection) {
            try {
                connection = impl.open_connection();
            } catch (const std::exception& e) {
                const std::lock_guard<std::mutex> lock(impl.mutex);
                --impl.total;  // give the reserved slot back
                impl.available.notify_one();
                last = Status::make(StatusCode::Unavailable, e.what());
                continue;
            }
        }

        PooledConnection pooled(impl, std::move(connection));
        const client::CallStatus result = call(*pooled);
        const StatusCode code = map_status(result);

        if (code == StatusCode::Ok) {
            return Status::success();
        }

        // A lost or desynchronised connection must not go back in the pool.
        // NotFound and InvalidArgument leave the stream in a known state, so
        // that connection is still perfectly good.
        if (code == StatusCode::Unavailable || code == StatusCode::ProtocolError ||
            code == StatusCode::DeadlineExceeded) {
            pooled.mark_failed();
        }

        last = Status::make(code, (*pooled).last_error());

        if (code == StatusCode::NotFound) {
            return last;  // a truthful answer, not a failure to retry
        }
        if (!is_retryable_failure(code)) {
            return last;
        }
    }

    return last;
}

}  // namespace

Status Client::put_item(const rank::Item& item) {
    return invoke(*impl_, /*retryable=*/true, [&item](client::Connection& connection) {
        return connection.put_item(item);
    });
}

Status Client::get_item(rank::ItemId id, rank::Item& item) {
    return invoke(*impl_, /*retryable=*/true, [id, &item](client::Connection& connection) {
        return connection.get_item(id, item);
    });
}

Status Client::delete_item(rank::ItemId id) {
    return invoke(*impl_, /*retryable=*/true, [id](client::Connection& connection) {
        return connection.delete_item(id);
    });
}

Status Client::recommend(const rank::UserSignal& signal, std::uint32_t count,
                         std::vector<rank::RankedItem>& items) {
    // The compliance-driven default. A recording recommendation that fails
    // mid-flight may already have charged its caps, so retrying it risks
    // double-counting. See ClientConfig::retry_recording_recommendations.
    return invoke(*impl_, impl_->config.retry_recording_recommendations,
                  [&](client::Connection& connection) {
                      return connection.recommend(signal, count, items, /*dry_run=*/false);
                  });
}

Status Client::preview(const rank::UserSignal& signal, std::uint32_t count,
                       std::vector<rank::RankedItem>& items) {
    // No side effects, so always safe to retry.
    return invoke(*impl_, /*retryable=*/true, [&](client::Connection& connection) {
        return connection.recommend(signal, count, items, /*dry_run=*/true);
    });
}

Status Client::stats(Stats& stats) {
    proto::Stats wire;
    const Status status = invoke(*impl_, /*retryable=*/true, [&wire](client::Connection& conn) {
        return conn.fetch_stats(wire);
    });
    if (!status) {
        return status;
    }

    // Copied field by field rather than re-exported. Tedious exactly once, and
    // it is what keeps a protocol change from becoming an API change.
    stats.requests = wire.requests;
    stats.gets = wire.gets;
    stats.puts = wire.puts;
    stats.deletes = wire.deletes;
    stats.recommends = wire.recommends;
    stats.hits = wire.hits;
    stats.misses = wire.misses;
    stats.evictions = wire.evictions;
    stats.entries = wire.entries;
    stats.capacity = wire.capacity;
    stats.policy_allowed = wire.policy_allowed;
    stats.policy_exposure_blocked = wire.policy_exposure_blocked;
    stats.policy_frequency_blocked = wire.policy_frequency_blocked;
    stats.policy_store_full = wire.policy_store_full;
    stats.policy_tracked = wire.policy_tracked;
    return Status::success();
}

}  // namespace lrd::sdk
