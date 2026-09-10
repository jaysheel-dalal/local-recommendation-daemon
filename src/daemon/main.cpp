// lrdd - Local Recommendation Daemon.
//
// Step 1: a single-threaded echo server over a UNIX domain socket. It exists to
// exercise the transport layer end to end - Fd ownership, bind/listen/accept,
// partial reads, peer disconnects - before framing (step 2), the cache (step 3)
// or the thread pool (step 4) are layered on top.
//
// Known limitations, both resolved later and both deliberate for now:
//   * One connection at a time. Concurrency arrives in steps 4-5.
//   * No graceful shutdown. Ctrl-C kills the process before the UnixListener
//     destructor can unlink the socket file, which leaves the file behind -
//     exactly the stale-socket case that UnixListener::bind detects and cleans
//     up on the next start. Step 5 adds a self-pipe/signalfd so the accept loop
//     can be interrupted and shut down cleanly.

#include "lrd/common/errors.hpp"
#include "lrd/common/log.hpp"
#include "lrd/common/version.hpp"
#include "lrd/net/unix_socket.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kDefaultSocketPath = "/tmp/lrd.sock";

struct Options {
    std::string socket_path{kDefaultSocketPath};
    bool verbose = false;
    bool show_help = false;
    bool show_version = false;
};

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "\n"
        "  --socket PATH   unix domain socket to listen on (default: %.*s)\n"
        "  --verbose       log every chunk transferred\n"
        "  --version       print version and exit\n"
        "  --help          print this message and exit\n",
        argv0, static_cast<int>(kDefaultSocketPath.size()), kDefaultSocketPath.data());
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
        } else {
            std::fprintf(stderr, "lrdd: unknown argument '%.*s'\n", static_cast<int>(arg.size()),
                         arg.data());
            return false;
        }
    }
    return true;
}

/// Echoes one connection until the peer stops sending.
///
/// Note the shape: read whatever arrived, write all of it back. read_some can
/// return fewer bytes than the buffer holds and write_all can accept fewer than
/// offered per syscall, and neither of those is an error - it is just how
/// stream sockets behave. Step 2 replaces this with framed request/response
/// handling, where "whatever arrived" stops being good enough and read_exact
/// takes over.
void serve_connection(lrd::net::UnixStream& stream, bool verbose) {
    std::array<unsigned char, 64 * 1024> buffer{};
    std::size_t total = 0;

    for (;;) {
        const lrd::net::IoResult in = stream.read_some(buffer.data(), buffer.size());
        if (in.status == lrd::net::IoStatus::PeerClosed) {
            lrd::log_info("connection closed by peer after {} bytes", total);
            return;
        }
        if (!in) {
            lrd::log_warn("read failed: {}", lrd::describe_errno(in.error, "read"));
            return;
        }

        const lrd::net::IoResult out = stream.write_all(buffer.data(), in.transferred);
        if (out.status == lrd::net::IoStatus::PeerClosed) {
            lrd::log_info("peer hung up mid-write after {} of {} bytes", out.transferred,
                          in.transferred);
            return;
        }
        if (!out) {
            lrd::log_warn("write failed: {}", lrd::describe_errno(out.error, "write"));
            return;
        }

        total += in.transferred;
        if (verbose) {
            lrd::log_debug("echoed {} bytes (total {})", in.transferred, total);
        }
    }
}

int run(const Options& opts) {
    lrd::net::UnixListener listener = lrd::net::UnixListener::bind(opts.socket_path);

    for (;;) {
        lrd::net::UnixStream stream = listener.accept();
        lrd::log_info("accepted connection (fd {})", stream.native_handle());
        serve_connection(stream, opts.verbose);
        // `stream` goes out of scope here and Fd::close() runs. No explicit
        // close on any path, including the error returns inside
        // serve_connection - that is the whole point of the RAII wrapper.
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

    // One try/catch, at the top. Startup failures (a path that is too long, an
    // address already in use, a permissions problem) throw from deep inside the
    // socket code and there is nothing useful to do about them locally - so
    // they propagate here, become one clear line of output, and set the exit
    // status. Per-request I/O errors never reach this point; those are returned
    // as IoResult and handled in serve_connection.
    try {
        return run(opts);
    } catch (const std::exception& e) {
        lrd::log_error("fatal: {}", e.what());
        return 1;
    }
}
