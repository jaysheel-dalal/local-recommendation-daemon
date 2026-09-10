// lrd_cli - command-line client for the daemon.
//
//   lrd_cli get KEY
//   lrd_cli put KEY VALUE
//   lrd_cli put KEY --size N     generate an N-byte value
//   lrd_cli del KEY
//   lrd_cli stats
//   lrd_cli pipeline N           N put/get pairs down one connection
//
// `pipeline` is the framing test that matters: many messages back to back on a
// single stream, where a one-byte error in a length prefix desynchronises
// everything after it.

#include "lrd/client/connection.hpp"
#include "lrd/common/errors.hpp"
#include "lrd/common/log.hpp"
#include "lrd/proto/message.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {

using lrd::client::CallStatus;
using lrd::client::Connection;

constexpr std::string_view kDefaultSocketPath = "/tmp/lrd.sock";

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [--socket PATH] COMMAND [args]\n"
        "\n"
        "  get KEY               fetch a value\n"
        "  put KEY VALUE         store a value\n"
        "  put KEY --size N      store a generated N-byte value\n"
        "  del KEY               delete a key\n"
        "  stats                 daemon counters\n"
        "  pipeline N            N put/get pairs on one connection\n"
        "\n"
        "  --socket PATH         daemon socket (default: %.*s)\n"
        "  --codec NAME          wire codec, must match the daemon (default: binary)\n",
        argv0, static_cast<int>(kDefaultSocketPath.size()), kDefaultSocketPath.data());
}

std::string generate_value(std::size_t size) {
    std::string value(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        value[i] = static_cast<char>('a' + (i % 26));
    }
    return value;
}

int report(CallStatus status, const Connection& connection, std::string_view what) {
    if (status == CallStatus::Ok) {
        return 0;
    }
    if (status == CallStatus::NotFound) {
        lrd::log_info("{}: not found", what);
        return 1;
    }
    lrd::log_error("{}: {} ({})", what, lrd::client::to_string(status), connection.last_error());
    return 1;
}

int cmd_get(Connection& connection, std::string_view key) {
    std::string value;
    const CallStatus status = connection.get(key, value);
    if (status == CallStatus::Ok) {
        // Value to stdout, diagnostics to stderr, so the tool composes in a
        // shell pipeline.
        std::printf("%.*s\n", static_cast<int>(value.size()), value.data());
    }
    return report(status, connection, "get");
}

int cmd_put(Connection& connection, std::string_view key, std::string_view value) {
    const CallStatus status = connection.put(key, value);
    if (status == CallStatus::Ok) {
        lrd::log_info("put '{}' ({} bytes)", key, value.size());
    }
    return report(status, connection, "put");
}

int cmd_delete(Connection& connection, std::string_view key) {
    const CallStatus status = connection.remove(key);
    if (status == CallStatus::Ok) {
        lrd::log_info("deleted '{}'", key);
    }
    return report(status, connection, "del");
}

int cmd_stats(Connection& connection) {
    lrd::proto::Stats stats;
    const CallStatus status = connection.fetch_stats(stats);
    if (status != CallStatus::Ok) {
        return report(status, connection, "stats");
    }

    // Hit rate and occupancy are the two numbers that actually say whether a
    // cache is sized correctly, and neither is on the wire - both are derived
    // here from the raw counters.
    const auto pct = [](std::uint64_t part, std::uint64_t whole) {
        return whole == 0 ? 0.0
                          : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    };

    std::printf("requests   %llu\ngets       %llu\nputs       %llu\ndeletes    %llu\n"
                "hits       %llu\nmisses     %llu\nhit rate   %.1f%%\n"
                "evictions  %llu\nentries    %llu / %llu (%.1f%% full)\n",
                static_cast<unsigned long long>(stats.requests),
                static_cast<unsigned long long>(stats.gets),
                static_cast<unsigned long long>(stats.puts),
                static_cast<unsigned long long>(stats.deletes),
                static_cast<unsigned long long>(stats.hits),
                static_cast<unsigned long long>(stats.misses),
                pct(stats.hits, stats.hits + stats.misses),
                static_cast<unsigned long long>(stats.evictions),
                static_cast<unsigned long long>(stats.entries),
                static_cast<unsigned long long>(stats.capacity),
                pct(stats.entries, stats.capacity));
    return 0;
}

/// Many small messages down one connection, verifying every value comes back
/// intact. This is what catches an off-by-one in the length prefix: the first
/// request would succeed and everything after it would be read from the wrong
/// offset.
int cmd_pipeline(Connection& connection, std::size_t count) {
    const auto started = std::chrono::steady_clock::now();
    std::size_t mismatches = 0;

    for (std::size_t i = 0; i < count; ++i) {
        const std::string key = "key-" + std::to_string(i);
        const std::string expected = "value-" + std::to_string(i * 7919);

        if (const CallStatus status = connection.put(key, expected); status != CallStatus::Ok) {
            return report(status, connection, "pipeline put");
        }

        std::string actual;
        if (const CallStatus status = connection.get(key, actual); status != CallStatus::Ok) {
            return report(status, connection, "pipeline get");
        }
        if (actual != expected) {
            ++mismatches;
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    const double per_request = count == 0 ? 0.0 : static_cast<double>(micros) / (2.0 * static_cast<double>(count));

    lrd::log_info("{} put/get pairs in {} us ({:.2f} us per request), {} mismatch(es)", count,
                  micros, per_request, mismatches);
    return mismatches == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path{kDefaultSocketPath};
    std::string codec_name{"binary"};
    std::vector<std::string_view> args;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--socket") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lrd_cli: --socket requires a path\n");
                return 2;
            }
            socket_path = argv[++i];
            continue;
        }
        if (arg == "--codec") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lrd_cli: --codec requires a name\n");
                return 2;
            }
            codec_name = argv[++i];
            continue;
        }
        args.push_back(arg);
    }

    if (args.empty()) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        Connection connection = Connection::connect(socket_path, codec_name);
        const std::string_view command = args[0];

        if (command == "get" && args.size() == 2) {
            return cmd_get(connection, args[1]);
        }
        if (command == "put" && args.size() == 3) {
            if (args[1].empty()) {
                std::fprintf(stderr, "lrd_cli: key must not be empty\n");
                return 2;
            }
            return cmd_put(connection, args[1], args[2]);
        }
        if (command == "put" && args.size() == 4 && args[2] == "--size") {
            // std::string first: a string_view is not guaranteed
            // NUL-terminated, and strtoull reads until one.
            const auto size = static_cast<std::size_t>(std::stoull(std::string(args[3])));
            return cmd_put(connection, args[1], generate_value(size));
        }
        if (command == "del" && args.size() == 2) {
            return cmd_delete(connection, args[1]);
        }
        if (command == "stats" && args.size() == 1) {
            return cmd_stats(connection);
        }
        if (command == "pipeline" && args.size() == 2) {
            const auto count = static_cast<std::size_t>(std::stoull(std::string(args[1])));
            return cmd_pipeline(connection, count);
        }

        std::fprintf(stderr, "lrd_cli: unrecognised command\n");
        print_usage(argv[0]);
        return 2;
    } catch (const lrd::SystemError& e) {
        if (e.code() == std::errc::connection_refused ||
            e.code() == std::errc::no_such_file_or_directory) {
            lrd::log_error("no daemon listening on {} - start lrdd first", socket_path);
            return 1;
        }
        lrd::log_error("fatal: {}", e.what());
        return 1;
    } catch (const std::exception& e) {
        lrd::log_error("fatal: {}", e.what());
        return 1;
    }
}
