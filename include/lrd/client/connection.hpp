#pragma once

#include "lrd/net/unix_socket.hpp"
#include "lrd/proto/codec.hpp"
#include "lrd/proto/message.hpp"
#include "lrd/proto/wire.hpp"
#include "lrd/rank/item.hpp"
#include "lrd/rank/signal.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lrd::client {

/// Outcome of one call. Flattens the three failure layers - transport, framing,
/// protocol - into the vocabulary a caller can act on.
enum class CallStatus : std::uint8_t {
    Ok,
    NotFound,        ///< The item is absent. Not an error for get or delete.
    InvalidRequest,  ///< The daemon rejected the request.
    ServerError,     ///< The daemon failed internally.
    ConnectionLost,  ///< Transport died; this Connection is finished.
    ProtocolError,   ///< The daemon's reply did not make sense.
};

[[nodiscard]] const char* to_string(CallStatus status) noexcept;

/// A single client connection to the daemon.
///
/// The seed of the Phase 2 SDK (step 11 grows it into one). Even at this size it
/// does the work an SDK exists to do: owning the socket, hiding framing and byte
/// order, reusing buffers, matching responses to requests, and translating three
/// layers of failure into one status.
///
/// Not thread-safe: one Connection is one request/response stream, and
/// interleaving calls from two threads would interleave their frames. Sharing
/// across threads means a connection per thread - which is what step 11's pooled
/// Client will do on the caller's behalf.
class Connection {
public:
    /// Connects to the daemon. Throws SystemError if the socket cannot be
    /// reached, and std::invalid_argument for an unknown codec name.
    ///
    /// `codec_name` must match the daemon's. There is no negotiation - see
    /// ServerConfig::codec_name for why that is a deliberate simplification.
    [[nodiscard]] static Connection connect(std::string_view socket_path,
                                            std::string_view codec_name = "binary");

    explicit Connection(net::UnixStream stream, std::string_view codec_name = "binary");

    /// Stores or replaces an item.
    [[nodiscard]] CallStatus put_item(const rank::Item& item);

    /// Fetches an item by id. Returns NotFound with `item` untouched if absent.
    [[nodiscard]] CallStatus get_item(rank::ItemId id, rank::Item& item);

    /// Returns NotFound if the id was not present.
    [[nodiscard]] CallStatus delete_item(rank::ItemId id);

    /// Ranks candidates against `signal` and returns the best `count`.
    ///
    /// `dry_run` asks the daemon to rank without recording anything. Inert until
    /// step 10 introduces exposure accounting; exposed now so that a caller
    /// written today keeps working when it starts to matter.
    [[nodiscard]] CallStatus recommend(const rank::UserSignal& signal, std::uint32_t count,
                                       std::vector<rank::RankedItem>& items,
                                       bool dry_run = false);

    [[nodiscard]] CallStatus fetch_stats(proto::Stats& stats);

    /// Detail for the last non-Ok status, for logs and error messages.
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    [[nodiscard]] bool connected() const noexcept { return stream_.valid(); }

private:
    /// Sends one request and reads its response. Every public method is a thin
    /// wrapper over this, which keeps request-id matching and error translation
    /// in exactly one place.
    [[nodiscard]] CallStatus call(const proto::Request& request, proto::Response& response);

    [[nodiscard]] CallStatus fail(CallStatus status, std::string message);

    /// Pulls the expected alternative out of a response body, or reports a
    /// protocol error naming what arrived instead.
    ///
    /// A template rather than five near-identical blocks: every method needs the
    /// same "is this the reply I asked for" check, and std::get_if on the variant
    /// makes it one line per call site.
    template <typename Expected>
    [[nodiscard]] const Expected* expect(const proto::Response& response, CallStatus& status);

    net::UnixStream stream_;
    std::unique_ptr<proto::Codec> codec_;
    proto::ByteBuffer read_buffer_;
    proto::ByteBuffer write_buffer_;
    std::uint64_t next_request_id_ = 1;
    std::string last_error_;
};

}  // namespace lrd::client
