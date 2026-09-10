#pragma once

#include "lrd/net/unix_socket.hpp"
#include "lrd/proto/codec.hpp"
#include "lrd/proto/message.hpp"
#include "lrd/proto/wire.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace lrd::client {

/// Outcome of one call. Flattens the three failure layers - transport, framing,
/// protocol - into the vocabulary a caller can actually act on.
enum class CallStatus : std::uint8_t {
    Ok,
    NotFound,        ///< The key is absent. Not an error for get/delete.
    InvalidRequest,  ///< The daemon rejected the request.
    ServerError,     ///< The daemon failed internally.
    ConnectionLost,  ///< Transport died; this Connection is finished.
    ProtocolError,   ///< The daemon's reply did not make sense.
};

[[nodiscard]] const char* to_string(CallStatus status) noexcept;

/// A single client connection to the daemon.
///
/// This is the seed of the Phase 2 SDK. Even at this size it is doing the work
/// an SDK exists to do: owning the socket, hiding framing and byte order,
/// reusing buffers, matching responses to requests, and translating three
/// layers of failure into one status a caller can switch on. Phase 2 grows it
/// into a real library - connection reuse, retries, typed item queries - rather
/// than replacing it.
///
/// Not thread-safe: one Connection is one request/response stream, and
/// interleaving calls from two threads would interleave their frames. Sharing
/// across threads means a connection per thread (which is what the step 6
/// benchmark does) or an external mutex.
class Connection {
public:
    /// Connects to the daemon. Throws SystemError if the socket cannot be
    /// reached - a constructor cannot return a status, and a half-built
    /// Connection is not a thing worth representing.
    [[nodiscard]] static Connection connect(std::string_view socket_path);

    explicit Connection(net::UnixStream stream) noexcept;

    /// Fetches `key`. Returns NotFound with `value` untouched if absent.
    [[nodiscard]] CallStatus get(std::string_view key, std::string& value);

    [[nodiscard]] CallStatus put(std::string_view key, std::string_view value);

    /// Returns NotFound if the key was not present.
    [[nodiscard]] CallStatus remove(std::string_view key);

    [[nodiscard]] CallStatus fetch_stats(proto::Stats& stats);

    /// Detail for the last non-Ok status, for logs and error messages.
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    [[nodiscard]] bool connected() const noexcept { return stream_.valid(); }

private:
    /// Sends one request and reads its response. Every public method is a thin
    /// wrapper over this, which is what keeps request-id matching and error
    /// translation in exactly one place.
    [[nodiscard]] CallStatus call(const proto::Request& request, proto::Response& response);

    [[nodiscard]] CallStatus fail(CallStatus status, std::string message);

    net::UnixStream stream_;
    proto::BinaryCodec codec_;
    proto::ByteBuffer read_buffer_;
    proto::ByteBuffer write_buffer_;
    std::uint64_t next_request_id_ = 1;
    std::string last_error_;
};

}  // namespace lrd::client
