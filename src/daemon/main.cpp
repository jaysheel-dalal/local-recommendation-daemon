// lrdd - Local Recommendation Daemon.
//
// Step 5: a concurrent daemon. One acceptor thread polls the listening socket
// and a stop pipe; accepted connections are handed to a thread pool, and each
// worker owns its connection until the peer disconnects. A single shared
// Handler guards the cache with a mutex and its counters with atomics.
//
// Known limitation, documented rather than discovered: more concurrent
// connections than worker threads means the surplus wait in the queue with no
// service, because workers block in read() on their own clients. See the
// commentary on lrd::daemon::Server for why, and what fixing it would involve.

#include "lrd/common/errors.hpp"
#include "lrd/common/log.hpp"
#include "lrd/common/version.hpp"
#include "lrd/daemon/server.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::string_view kDefaultSocketPath = "/tmp/lrd.sock";
constexpr std::size_t kDefaultCapacity = 10000;
constexpr std::size_t kDefaultQueue = 64;
constexpr std::size_t kDefaultShards = 16;

/// hardware_concurrency() may return 0 when it cannot tell, so it always needs
/// a fallback. Four is a reasonable guess for a device-class machine.
std::size_t default_thread_count() {
    const unsigned detected = std::thread::hardware_concurrency();
    return detected == 0 ? 4 : static_cast<std::size_t>(detected);
}

struct Options {
    lrd::daemon::ServerConfig config;
    bool show_help = false;
    bool show_version = false;
};

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "\n"
        "  --socket PATH   unix domain socket to listen on (default: %.*s)\n"
        "  --capacity N    cache entries before LRU eviction (default: %zu)\n"
        "  --shards N      cache shards, power of two (default: %zu)\n"
        "  --threads N     worker threads (default: one per core)\n"
        "  --queue N       connections queued awaiting a worker (default: %zu)\n"
        "  --verbose       log every request\n"
        "  --version       print version and exit\n"
        "  --help          print this message and exit\n",
        argv0, static_cast<int>(kDefaultSocketPath.size()), kDefaultSocketPath.data(),
        kDefaultCapacity, kDefaultShards, kDefaultQueue);
}

bool parse_size(const char* text, std::size_t& out, const char* name) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0) {
        std::fprintf(stderr, "lrdd: %s must be a positive integer\n", name);
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

bool parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            out.show_help = true;
        } else if (arg == "--version" || arg == "-V") {
            out.show_version = true;
        } else if (arg == "--verbose" || arg == "-v") {
            out.config.verbose = true;
        } else if (arg == "--socket" && i + 1 < argc) {
            out.config.socket_path = argv[++i];
        } else if (arg == "--capacity" && i + 1 < argc) {
            if (!parse_size(argv[++i], out.config.cache_capacity, "--capacity")) {
                return false;
            }
        } else if (arg == "--shards" && i + 1 < argc) {
            if (!parse_size(argv[++i], out.config.cache_shards, "--shards")) {
                return false;
            }
        } else if (arg == "--threads" && i + 1 < argc) {
            if (!parse_size(argv[++i], out.config.thread_count, "--threads")) {
                return false;
            }
        } else if (arg == "--queue" && i + 1 < argc) {
            if (!parse_size(argv[++i], out.config.max_queued_connections, "--queue")) {
                return false;
            }
        } else {
            std::fprintf(stderr, "lrdd: bad argument '%.*s'\n", static_cast<int>(arg.size()),
                         arg.data());
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    opts.config.socket_path = std::string(kDefaultSocketPath);
    opts.config.cache_capacity = kDefaultCapacity;
    opts.config.thread_count = default_thread_count();
    opts.config.max_queued_connections = kDefaultQueue;
    opts.config.cache_shards = kDefaultShards;

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

    if (opts.config.verbose) {
        lrd::log_set_level(lrd::LogLevel::Debug);
    }
    lrd::log_info("{}", build);

    try {
        lrd::daemon::Server server(opts.config);

        // Installed after the server exists, so a signal arriving during
        // construction cannot reach a half-built object.
        lrd::daemon::Server::install_signal_handlers(server);

        server.run();
        return 0;
    } catch (const std::exception& e) {
        lrd::log_error("fatal: {}", e.what());
        return 1;
    }
}
