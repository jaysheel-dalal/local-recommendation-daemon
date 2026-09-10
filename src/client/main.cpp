// lrd_cli - step 1 echo client.
//
// Sends a payload to the daemon and reads exactly the same number of bytes
// back, verifying they match. With a large --repeat the payload comfortably
// exceeds the socket buffer, which forces the kernel to split it across many
// reads and writes - the partial-transfer behaviour that read_exact and
// write_all exist to absorb. Running with --repeat 1 and --repeat 100000 and
// getting identical correctness is the point of the exercise.
//
// In Phase 2 this directory becomes the client SDK. For now it is a test tool.

#include "lrd/common/errors.hpp"
#include "lrd/common/log.hpp"
#include "lrd/net/unix_socket.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view kDefaultSocketPath = "/tmp/lrd.sock";

struct Options {
    std::string socket_path{kDefaultSocketPath};
    std::string message{"hello from lrd_cli"};
    std::size_t repeat = 1;
    bool show_help = false;
};

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "\n"
        "  --socket PATH   daemon socket (default: %.*s)\n"
        "  --message TEXT  payload to echo\n"
        "  --repeat N      repeat the payload N times (use a big N to force\n"
        "                  partial reads/writes)\n"
        "  --help          print this message and exit\n",
        argv0, static_cast<int>(kDefaultSocketPath.size()), kDefaultSocketPath.data());
}

bool parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            out.show_help = true;
        } else if (arg == "--socket" && i + 1 < argc) {
            out.socket_path = argv[++i];
        } else if (arg == "--message" && i + 1 < argc) {
            out.message = argv[++i];
        } else if (arg == "--repeat" && i + 1 < argc) {
            out.repeat = std::strtoul(argv[++i], nullptr, 10);
            if (out.repeat == 0) {
                std::fprintf(stderr, "lrd_cli: --repeat must be >= 1\n");
                return false;
            }
        } else {
            std::fprintf(stderr, "lrd_cli: bad argument '%.*s'\n", static_cast<int>(arg.size()),
                         arg.data());
            return false;
        }
    }
    return true;
}

int run(const Options& opts) {
    std::string payload;
    payload.reserve(opts.message.size() * opts.repeat);
    for (std::size_t i = 0; i < opts.repeat; ++i) {
        payload += opts.message;
    }

    lrd::net::UnixStream stream = lrd::net::UnixStream::connect(opts.socket_path);

    const auto started = std::chrono::steady_clock::now();

    // Send and receive must overlap. Writing the whole payload first and only
    // then reading the reply deadlocks the moment the payload outgrows the
    // socket buffers, and it is worth being precise about why:
    //
    //   client blocks in write_all, its send buffer full
    //     -> the daemon echoes what it has read, filling its own send buffer
    //        and the client's receive buffer, and blocks in write_all too
    //          -> nobody is left to drain either direction. Both sides wait
    //             forever.
    //
    // Roughly 200 KB of kernel buffering hides this: it works perfectly in
    // testing and hangs on the first large request in production. The fix is to
    // drain the reply on one thread while the other fills the pipe.
    //
    // This is also the structural reason real protocols frame their messages
    // and bound their sizes - which is exactly what step 2 adds. A
    // request/response protocol with small messages never reaches this state,
    // and that is a design property, not luck.
    lrd::net::IoResult sent;
    std::thread sender([&] { sent = stream.write_all(payload.data(), payload.size()); });

    // Reading back *exactly* payload.size() bytes is the assertion. A naive
    // client would issue one read(), get a fraction of the payload, and either
    // report corruption or hang waiting for a message boundary that a stream
    // socket never provides.
    std::vector<char> echoed(payload.size());
    const lrd::net::IoResult received = stream.read_exact(echoed.data(), echoed.size());
    sender.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    if (!sent) {
        lrd::log_error("send failed after {} bytes: {}", sent.transferred,
                       lrd::describe_errno(sent.error, "write"));
        return 1;
    }

    if (received.status == lrd::net::IoStatus::PeerClosed) {
        lrd::log_error("daemon closed after {} of {} bytes", received.transferred, payload.size());
        return 1;
    }
    if (!received) {
        lrd::log_error("read failed: {}", lrd::describe_errno(received.error, "read"));
        return 1;
    }

    const bool matches = std::string_view(echoed.data(), echoed.size()) == payload;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    lrd::log_info("echoed {} bytes in {} us, payload {}", payload.size(), micros,
                  matches ? "matches" : "DIFFERS");

    if (payload.size() <= 256) {
        std::printf("%.*s\n", static_cast<int>(echoed.size()), echoed.data());
    }

    return matches ? 0 : 1;
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

    try {
        return run(opts);
    } catch (const lrd::SystemError& e) {
        // Worth distinguishing: "nothing is listening" is the overwhelmingly
        // common failure for a client and deserves an actionable message rather
        // than a raw errno string.
        if (e.code() == std::errc::connection_refused ||
            e.code() == std::errc::no_such_file_or_directory) {
            lrd::log_error("no daemon listening on {} - start lrdd first", opts.socket_path);
            return 1;
        }
        lrd::log_error("fatal: {}", e.what());
        return 1;
    } catch (const std::exception& e) {
        lrd::log_error("fatal: {}", e.what());
        return 1;
    }
}
