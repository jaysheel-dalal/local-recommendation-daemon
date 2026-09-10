#include "lrd/daemon/server.hpp"

#include "lrd/common/errors.hpp"
#include "lrd/common/log.hpp"
#include "lrd/proto/framing.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <thread>
#include <utility>

namespace lrd::daemon {

namespace {

/// The server a signal handler should stop.
///
/// A signal handler is a free function with no context, so the target has to
/// live at file scope. std::atomic rather than a plain pointer because the
/// handler may run on any thread at any moment; the static_assert is the part
/// that makes this legal rather than merely conventional - a signal handler may
/// only touch an atomic that is lock-free, since a lock-based one could
/// deadlock against the very thread it interrupted.
std::atomic<Server*> g_signal_target{nullptr};
static_assert(std::atomic<Server*>::is_always_lock_free,
              "signal handlers may only touch lock-free atomics");

void handle_stop_signal(int /*signal_number*/) {
    // Everything in here must be async-signal-safe. That rules out almost the
    // entire standard library - no allocation, no locks, no printf, no
    // std::format. write() is on the safe list, which is the whole reason the
    // self-pipe trick exists.
    const int saved_errno = errno;

    if (Server* server = g_signal_target.load(std::memory_order_relaxed); server != nullptr) {
        server->request_stop();
    }

    // Restoring errno matters. We interrupted a thread that may have been
    // between a failing syscall and its check of errno; leaving our own value
    // behind would make it diagnose a completely unrelated failure.
    errno = saved_errno;
}

}  // namespace

Server::StopPipe Server::make_stop_pipe() {
    int fds[2] = {-1, -1};
    // O_CLOEXEC for the same reason as everywhere else. O_NONBLOCK so that a
    // storm of signals filling the pipe makes the handler's write() fail
    // harmlessly instead of blocking inside a signal handler - which would hang
    // the process.
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        throw_errno("pipe2");
    }
    return StopPipe{Fd(fds[0]), Fd(fds[1])};
}

namespace {

}  // namespace

Server::Server(ServerConfig config)
    : config_(std::move(config)),
      listener_(net::UnixListener::bind(config_.socket_path)),
      handler_(config_.cache_capacity),
      stop_(make_stop_pipe()),
      pool_(config_.thread_count, config_.max_queued_connections) {
    log_info("codec: {}, cache capacity: {}, worker threads: {}, connection queue: {}",
             codec_.name(), config_.cache_capacity, config_.thread_count,
             config_.max_queued_connections);
}

Server::~Server() {
    // Idempotent belt-and-braces: if run() exited normally these have already
    // happened, but a Server destroyed without run() ever being called - or
    // unwound by an exception - must still not leave workers blocked on
    // sockets.
    registry_.stop_all();
    pool_.shutdown(concurrency::ShutdownPolicy::Discard);

    if (g_signal_target.load(std::memory_order_relaxed) == this) {
        g_signal_target.store(nullptr, std::memory_order_relaxed);
    }
}

void Server::request_stop() noexcept {
    const unsigned char byte = 1;
    // Deliberately ignoring the result. If the pipe is full, a stop is already
    // pending and one more byte adds nothing; if it is closed, we are already
    // shutting down. Neither is worth handling, and neither can be handled
    // safely from a signal handler anyway.
    const ssize_t written = ::write(stop_.write.get(), &byte, 1);
    (void)written;
}

void Server::install_signal_handlers(Server& server) {
    g_signal_target.store(&server, std::memory_order_relaxed);

    struct sigaction action{};
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    // Deliberately NOT SA_RESTART. We want an interrupted poll() to return
    // EINTR so the loop gets a chance to notice - the pipe write is the primary
    // mechanism, and EINTR is the belt to its braces.
    action.sa_flags = 0;

    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);

    // SIGPIPE is ignored process-wide as a second line of defence. Every write
    // already uses MSG_NOSIGNAL, but a stray write() to a dead peer from any
    // future code path should not be able to kill the daemon.
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ignore.sa_flags = 0;
    ::sigaction(SIGPIPE, &ignore, nullptr);
}

void Server::drain_stop_pipe() noexcept {
    // The pipe is non-blocking, so this empties it and then returns EAGAIN.
    std::array<unsigned char, 64> scratch{};
    while (::read(stop_.read.get(), scratch.data(), scratch.size()) > 0) {
        // keep draining
    }
}

void Server::accept_one() {
    net::UnixStream stream = listener_.accept();
    accepted_.fetch_add(1, std::memory_order_relaxed);

    if (config_.verbose) {
        log_debug("accepted connection (fd {})", stream.native_handle());
    }

    // The stream is moved into the task. If post() refuses it, the Task is
    // destroyed on return and the UnixStream's destructor closes the socket -
    // so a rejected connection is cleaned up with no explicit close anywhere.
    // That is RAII doing the error handling.
    const concurrency::PostStatus status =
        pool_.post([this, stream = std::move(stream)]() mutable {
            serve_connection(std::move(stream));
        });

    if (status != concurrency::PostStatus::Accepted) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        log_warn("refusing connection: {}", concurrency::to_string(status));
    }
}

void Server::run() {
    log_info("serving on {}", listener_.path());

    bool stopping = false;
    while (!stopping) {
        std::array<pollfd, 2> fds{};
        fds[0].fd = listener_.native_handle();
        fds[0].events = POLLIN;
        fds[1].fd = stop_.read.get();
        fds[1].events = POLLIN;

        const int ready = ::poll(fds.data(), fds.size(), -1);

        if (ready < 0) {
            if (errno == EINTR) {
                // A signal arrived. Its handler has already written to the
                // pipe, so simply looping round will see it on the next poll.
                continue;
            }
            throw_errno("poll");
        }

        // Stop first: if both are ready during shutdown, stopping wins.
        if ((fds[1].revents & POLLIN) != 0) {
            drain_stop_pipe();
            stopping = true;
            break;
        }

        if ((fds[0].revents & POLLIN) != 0) {
            try {
                accept_one();
            } catch (const SystemError& e) {
                if (e.code() == std::errc::connection_aborted) {
                    // The client vanished between the kernel queueing it and us
                    // accepting. The listener is fine; carry on.
                    continue;
                }
                if (e.code() == std::errc::too_many_files_open ||
                    e.code() == std::errc::too_many_files_open_in_system) {
                    // The classic accept-loop trap: out of descriptors, so
                    // accept() fails, but the listener stays readable and the
                    // loop spins at 100% CPU failing forever. Backing off turns
                    // a livelock into a slow period that recovers by itself as
                    // connections close.
                    log_error("out of file descriptors; backing off");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                throw;
            }
        }
    }

    log_info("shutting down");

    // Order matters, and each step exists for a reason.
    //
    // 1. Stop listening and remove the socket file, so no new client can
    //    connect while we are tearing down.
    listener_.close();

    // 2. Break every live connection. Workers are blocked in read() and nothing
    //    else would wake them; this makes those reads return 0, which the
    //    connection loop already handles as a peer disconnect. It also latches
    //    the registry closed, so a connection still sitting in the pool's queue
    //    is abandoned rather than served.
    registry_.stop_all();

    // 3. Discard rather than Drain. A queued task here is an unserved
    //    connection, not useful work - and running it would only mean accepting
    //    a client we are about to disconnect. Discarding destroys the queued
    //    tasks, whose captured UnixStreams close their sockets on the way out.
    pool_.shutdown(concurrency::ShutdownPolicy::Discard);

    log_info("stopped: {} accepted, {} refused, {} completed", accepted_.load(), rejected_.load(),
             completed_.load());
}

void Server::serve_connection(net::UnixStream stream) {
    // Registering can fail, and that is not an error: it means shutdown began
    // between this connection being queued and a worker picking it up. The
    // stream closes as this function returns.
    const ConnectionGuard guard(registry_, stream.native_handle());
    if (!guard) {
        return;
    }

    // Both buffers live for the whole connection and are reused for every
    // message, so a connection serving a million requests does no per-request
    // allocation for framing. They are function-local, which means per-worker -
    // no sharing, no synchronisation, no false sharing between workers.
    proto::ByteBuffer request_body;
    proto::ByteBuffer response_body;
    std::uint64_t served = 0;

    for (;;) {
        const proto::FrameResult frame = proto::read_frame(stream, request_body);

        if (frame.status == proto::FrameStatus::PeerClosed) {
            break;
        }
        if (!frame) {
            // Also the path taken on shutdown: stop_all() shuts the socket down
            // and the pending read returns as a peer disconnect.
            if (frame.status != proto::FrameStatus::Truncated || !registry_.stopping()) {
                log_warn("dropping connection: frame {} (length {})",
                         proto::to_string(frame.status), frame.length);
            }
            break;
        }

        proto::Request request;
        const proto::DecodeError error = codec_.decode(request_body, request);

        if (error != proto::DecodeError::None) {
            // A semantic problem leaves the stream in a known state, so it is
            // answered and serving continues. A framing problem means we no
            // longer know where the next message starts, and the only safe move
            // is to hang up. FieldTooLarge is the only recoverable case: the
            // frame parsed cleanly and only its contents were unacceptable.
            if (error != proto::DecodeError::FieldTooLarge) {
                log_warn("dropping connection: decode {}", proto::to_string(error));
                break;
            }

            const proto::Response rejection =
                proto::make_error(request.request_id, proto::StatusCode::InvalidRequest,
                                  proto::to_string(error));
            if (!proto::write_message(stream, codec_, rejection, response_body)) {
                break;
            }
            continue;
        }

        if (config_.verbose) {
            log_debug("#{} {} key='{}' ({} value bytes)", request.request_id,
                      proto::to_string(request.type), request.key, request.value.size());
        }

        // handle() is thread-safe; every worker shares this one Handler, which
        // is the point - a PUT on one connection must be visible to a GET on
        // another.
        const proto::Response response = handler_.handle(request);

        if (!proto::write_message(stream, codec_, response, response_body)) {
            break;
        }
        ++served;
    }

    completed_.fetch_add(1, std::memory_order_relaxed);
    if (config_.verbose) {
        log_debug("connection closed after {} request(s)", served);
    }
}

ServerStats Server::stats() const noexcept {
    return ServerStats{
        accepted_.load(std::memory_order_relaxed),
        rejected_.load(std::memory_order_relaxed),
        completed_.load(std::memory_order_relaxed),
    };
}

}  // namespace lrd::daemon
