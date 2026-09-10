// lrdd - Local Recommendation Daemon.
//
// Step 2: a framed request/response daemon over a UNIX domain socket. It speaks
// the protocol in docs/protocol.md - GET, PUT, DELETE, STATS - against an
// in-memory store.
//
// Known limitations, both deliberate and both scheduled:
//   * One connection at a time. The thread pool and concurrent server are
//     steps 4-5.
//   * No graceful shutdown: Ctrl-C kills the process before ~UnixListener can
//     unlink the socket file, which leaves the stale-socket case that
//     UnixListener::bind cleans up on the next start. Step 5 adds a self-pipe.

#include "lrd/common/errors.hpp"
#include "lrd/common/log.hpp"
#include "lrd/common/version.hpp"
#include "lrd/daemon/handler.hpp"
#include "lrd/net/unix_socket.hpp"
#include "lrd/proto/codec.hpp"
#include "lrd/proto/framing.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kDefaultSocketPath = "/tmp/lrd.sock";

constexpr std::size_t kDefaultCapacity = 10000;

struct Options {
    std::string socket_path{kDefaultSocketPath};
    std::size_t capacity = kDefaultCapacity;
    bool verbose = false;
    bool show_help = false;
    bool show_version = false;
};

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "\n"
        "  --socket PATH   unix domain socket to listen on (default: %.*s)\n"
        "  --capacity N    cache entries before LRU eviction (default: %zu)\n"
        "  --verbose       log every request\n"
        "  --version       print version and exit\n"
        "  --help          print this message and exit\n",
        argv0, static_cast<int>(kDefaultSocketPath.size()), kDefaultSocketPath.data(),
        kDefaultCapacity);
}

bool parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            out.show_help = true;
        } else if (arg == "--version" || arg == "-V") {
            out.show_version = true;
        } else if (arg == "--verbose" || arg == "-v") {
            out.verbose = true;
        } else if (arg == "--socket") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lrdd: --socket requires a path\n");
                return false;
            }
            out.socket_path = argv[++i];
        } else if (arg == "--capacity") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lrdd: --capacity requires a number\n");
                return false;
            }
            out.capacity = std::strtoul(argv[++i], nullptr, 10);
            if (out.capacity == 0) {
                std::fprintf(stderr, "lrdd: --capacity must be at least 1\n");
                return false;
            }
        } else {
            std::fprintf(stderr, "lrdd: unknown argument '%.*s'\n", static_cast<int>(arg.size()),
                         arg.data());
            return false;
        }
    }
    return true;
}

/// Can the connection continue after this decode failure?
///
/// The distinction from docs/protocol.md, in code: a *semantic* problem leaves
/// the stream in a known state, so we answer it and keep serving. A *framing*
/// problem means we no longer know where the next message begins, and the only
/// safe response is to hang up. Guessing at a resynchronisation point is how a
/// protocol parser turns one bad frame into an infinite stream of garbage.
bool is_recoverable(lrd::proto::DecodeError error) noexcept {
    switch (error) {
        case lrd::proto::DecodeError::FieldTooLarge:
            // The frame itself parsed cleanly - we consumed exactly its bytes -
            // so the boundary is intact and only the contents were unacceptable.
            return true;

        case lrd::proto::DecodeError::None:
        case lrd::proto::DecodeError::Truncated:
        case lrd::proto::DecodeError::BadMagic:
        case lrd::proto::DecodeError::UnsupportedVersion:
        case lrd::proto::DecodeError::ReservedFlags:
        case lrd::proto::DecodeError::UnknownType:
        case lrd::proto::DecodeError::WrongDirection:
        case lrd::proto::DecodeError::TrailingBytes:
            return false;
    }
    return false;
}

/// Serves framed requests until the peer goes away or the stream breaks.
void serve_connection(lrd::net::UnixStream& stream, lrd::daemon::Handler& handler,
                      const lrd::proto::Codec& codec, bool verbose) {
    // Both buffers live across the whole connection and are reused for every
    // message. After the first few requests they stop growing, so a connection
    // serving a million requests does no per-request allocation for framing.
    lrd::proto::ByteBuffer request_body;
    lrd::proto::ByteBuffer response_body;
    std::uint64_t served = 0;

    for (;;) {
        const lrd::proto::FrameResult frame = lrd::proto::read_frame(stream, request_body);
        if (frame.status == lrd::proto::FrameStatus::PeerClosed) {
            lrd::log_info("connection closed by peer after {} request(s)", served);
            return;
        }
        if (!frame) {
            lrd::log_warn("dropping connection: frame {} (length {})",
                          lrd::proto::to_string(frame.status), frame.length);
            return;
        }

        lrd::proto::Request request;
        const lrd::proto::DecodeError error = codec.decode(request_body, request);

        if (error != lrd::proto::DecodeError::None) {
            if (!is_recoverable(error)) {
                lrd::log_warn("dropping connection: decode {}", lrd::proto::to_string(error));
                return;
            }
            lrd::log_warn("rejecting request {}: decode {}", request.request_id,
                          lrd::proto::to_string(error));
            const lrd::proto::Response rejection = lrd::proto::make_error(
                request.request_id, lrd::proto::StatusCode::InvalidRequest,
                lrd::proto::to_string(error));
            if (!lrd::proto::write_message(stream, codec, rejection, response_body)) {
                return;
            }
            continue;
        }

        if (verbose) {
            lrd::log_debug("#{} {} key='{}' ({} value bytes)", request.request_id,
                           lrd::proto::to_string(request.type), request.key,
                           request.value.size());
        }

        const lrd::proto::Response response = handler.handle(request);
        const lrd::proto::FrameResult written =
            lrd::proto::write_message(stream, codec, response, response_body);
        if (!written) {
            if (written.status != lrd::proto::FrameStatus::PeerClosed) {
                lrd::log_warn("write failed: frame {}", lrd::proto::to_string(written.status));
            }
            return;
        }

        ++served;
    }
}

int run(const Options& opts) {
    lrd::net::UnixListener listener = lrd::net::UnixListener::bind(opts.socket_path);

    // One Handler for the whole daemon: the store must outlive individual
    // connections, or a PUT on one connection would be invisible to a GET on
    // the next.
    lrd::daemon::Handler handler(opts.capacity);
    const lrd::proto::BinaryCodec codec;
    lrd::log_info("codec: {}, cache capacity: {}", codec.name(), opts.capacity);

    for (;;) {
        lrd::net::UnixStream stream = listener.accept();
        lrd::log_info("accepted connection (fd {})", stream.native_handle());
        serve_connection(stream, handler, codec, opts.verbose);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        print_usage(argv[0]);
        return 2;
    }
    if (opts.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    const std::string_view build = lrd::build_info();
    if (opts.show_version) {
        std::printf("%.*s\n", static_cast<int>(build.size()), build.data());
        return 0;
    }

    if (opts.verbose) {
        lrd::log_set_level(lrd::LogLevel::Debug);
    }
    lrd::log_info("{}", build);

    try {
        return run(opts);
    } catch (const std::exception& e) {
        lrd::log_error("fatal: {}", e.what());
        return 1;
    }
}
