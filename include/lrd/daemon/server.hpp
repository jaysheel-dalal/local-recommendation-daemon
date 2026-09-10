#pragma once

#include "lrd/common/fd.hpp"
#include "lrd/concurrency/thread_pool.hpp"
#include "lrd/daemon/connection_registry.hpp"
#include "lrd/daemon/handler.hpp"
#include "lrd/net/unix_socket.hpp"
#include "lrd/proto/codec.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lrd::daemon {

struct ServerConfig {
    std::string socket_path;
    std::size_t cache_capacity = 10000;

    /// Independently locked stripes of the cache; must be a power of two.
    /// See docs/benchmarks.md for how this number was chosen rather than
    /// guessed - and for why it does not move the end-to-end numbers.
    std::size_t cache_shards = 16;

    std::size_t thread_count = 4;

    /// Connections accepted but not yet picked up by a worker. Small on
    /// purpose: a long queue here does not help, because a queued connection is
    /// getting no service at all. Better to refuse quickly than to accept and
    /// stall - see the starvation note on run().
    std::size_t max_queued_connections = 64;

    bool verbose = false;
};

struct ServerStats {
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;   ///< Accepted by the kernel, refused by us.
    std::uint64_t completed = 0;  ///< Connections served to completion.
};

/// The concurrent daemon: one acceptor thread feeding a pool of workers, each
/// of which owns a connection for its lifetime.
///
/// ## Concurrency model, and the failure mode it accepts
///
/// A worker takes a connection and serves it until the peer disconnects. That
/// is the simplest model that works and the easiest to reason about, and it has
/// one sharp edge that has to be stated rather than discovered:
///
/// **More concurrent connections than worker threads means the surplus get no
/// service at all.** They sit in the queue while the busy workers block in
/// read() waiting for their own clients, which may be idle. Eight threads serve
/// eight connections; the ninth waits for one of them to hang up.
///
/// This is fine for the intended shape - a handful of local clients on a
/// device - and it is the right tradeoff for a Phase 1 whose point is the
/// locking strategy. It would not be fine for a server facing many idle-ish
/// connections.
///
/// The fix is to stop dispatching *connections* and start dispatching
/// *requests*: an epoll loop owns every socket, and posts a task only when a
/// connection actually has bytes ready. Workers then never block on a client,
/// so N threads can serve thousands of connections. That is a real
/// restructuring - connection state has to move out of the worker's stack and
/// into a per-connection object, and responses need ordering guarantees - which
/// is why it is documented here rather than half-done.
///
/// Until then the queue is bounded and overload is answered honestly: a
/// connection that cannot be queued is closed immediately rather than accepted
/// and ignored.
class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Accepts connections until request_stop() or a signal. Blocks.
    void run();

    /// Asks run() to return. Safe to call from any thread **and from a signal
    /// handler**: it does nothing but write one byte to a pipe, which is on the
    /// short list of async-signal-safe operations.
    void request_stop() noexcept;

    /// Routes SIGINT and SIGTERM to request_stop() on `server`.
    ///
    /// Static because a signal handler is a free function with no context of
    /// its own; the server is reached through a file-scope pointer. One server
    /// per process is assumed, which for a daemon is not much of an assumption.
    static void install_signal_handlers(Server& server);

    [[nodiscard]] ServerStats stats() const noexcept;
    [[nodiscard]] const Handler& handler() const noexcept { return handler_; }

    /// The bound socket path, for tests that need to connect to it.
    [[nodiscard]] const std::string& socket_path() const noexcept { return listener_.path(); }

private:
    struct StopPipe;
    [[nodiscard]] static StopPipe make_stop_pipe();

    void serve_connection(net::UnixStream stream);
    void accept_one();
    void drain_stop_pipe() noexcept;

    ServerConfig config_;
    net::UnixListener listener_;
    Handler handler_;
    ConnectionRegistry registry_;
    proto::BinaryCodec codec_;

    /// The self-pipe. A signal can arrive while the acceptor is blocked in
    /// accept(), and nothing about setting a flag would wake it. Writing a byte
    /// to a pipe that the acceptor is also poll()-ing does wake it - this is the
    /// self-pipe trick, and it is why the accept loop polls rather than simply
    /// calling accept().
    ///
    /// Linux has signalfd, which is tidier. This uses a plain pipe because it
    /// works identically on macOS and the BSDs, and the whole point of the
    /// exercise is that these concepts carry across to Darwin.
    ///
    /// Both ends live in one struct returned by value, rather than as two
    /// members initialised from one helper. The two-member version compiled
    /// fine and was silently broken: members are initialised in *declaration*
    /// order regardless of how the constructor's init-list is written, so a
    /// helper that filled in the second member while initialising the first was
    /// writing to an object whose own initialisation had not run yet - and
    /// which promptly overwrote the descriptor with -1. Every write to the stop
    /// pipe then failed silently and shutdown hung. Returning both ends
    /// together makes the ordering hazard structurally impossible.
    struct StopPipe {
        Fd read;
        Fd write;
    };
    StopPipe stop_;

    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> completed_{0};

    /// Declared last: its destructor joins the workers, and they use every
    /// member above it. Member destruction runs in reverse declaration order,
    /// so this guarantees the pool is drained before the handler, registry and
    /// listener it touches are gone.
    concurrency::ThreadPool pool_;
};

}  // namespace lrd::daemon
