#include "lrd/client/connection.hpp"
#include "lrd/common/errors.hpp"
#include "lrd/daemon/server.hpp"

#include "test_harness.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using lrd::client::CallStatus;
using lrd::client::Connection;
using lrd::daemon::Server;
using lrd::daemon::ServerConfig;
using namespace std::chrono_literals;

namespace {

std::string temp_socket_path() {
    static std::atomic<int> counter{0};
    return "/tmp/lrd_srv_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1)) + ".sock";
}

bool path_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

/// Runs a Server on its own thread and stops it on destruction, so a failing
/// assertion cannot leave a daemon thread running into the next test.
class RunningServer {
public:
    explicit RunningServer(ServerConfig config) : server_(std::move(config)) {
        thread_ = std::jthread([this] { server_.run(); });
        // run() binds before it is called, so the socket already exists; this
        // just gives the acceptor a moment to reach poll().
        std::this_thread::sleep_for(30ms);
    }

    ~RunningServer() {
        server_.request_stop();
        // jthread joins here.
    }

private:
    Server server_;
    std::jthread thread_;
};

ServerConfig config_for(const std::string& path, std::size_t threads = 4,
                        std::size_t capacity = 1000, std::size_t queue = 64) {
    ServerConfig config;
    config.socket_path = path;
    config.thread_count = threads;
    config.cache_capacity = capacity;
    config.max_queued_connections = queue;
    return config;
}

}  // namespace

LRD_TEST("a single client can put and get") {
    const std::string path = temp_socket_path();
    RunningServer server(config_for(path));

    Connection client = Connection::connect(path);
    LRD_REQUIRE(client.put("k", "v") == CallStatus::Ok);

    std::string value;
    LRD_REQUIRE(client.get("k", value) == CallStatus::Ok);
    LRD_CHECK_EQ(value, std::string("v"));
}

LRD_TEST("state written on one connection is visible on another") {
    // The whole reason a single Handler is shared across every worker. If each
    // connection had its own cache this would fail, and the daemon would be a
    // very elaborate way of talking to yourself.
    const std::string path = temp_socket_path();
    RunningServer server(config_for(path));

    Connection writer = Connection::connect(path);
    LRD_REQUIRE(writer.put("shared", "written-by-writer") == CallStatus::Ok);

    Connection reader = Connection::connect(path);
    std::string value;
    LRD_REQUIRE(reader.get("shared", value) == CallStatus::Ok);
    LRD_CHECK_EQ(value, std::string("written-by-writer"));
}

LRD_TEST("eight concurrent clients each see their own values intact") {
    // The step 5 milestone, and the case ThreadSanitizer is pointed at: eight
    // connections, eight workers, all hammering one shared locked cache.
    //
    // Each client owns a disjoint key space and checks that every value it
    // reads back is the one it wrote. A locking bug that let two workers
    // interleave inside the cache would surface here as a mismatch, not merely
    // as a crash.
    constexpr int kClients = 8;
    constexpr int kOpsPerClient = 300;

    const std::string path = temp_socket_path();
    RunningServer server(config_for(path, kClients, /*capacity=*/4096));

    std::atomic<int> mismatches{0};
    std::atomic<int> failures{0};

    {
        std::vector<std::jthread> clients;
        clients.reserve(kClients);

        for (int c = 0; c < kClients; ++c) {
            clients.emplace_back([&, c] {
                Connection connection = Connection::connect(path);

                for (int i = 0; i < kOpsPerClient; ++i) {
                    const std::string key = "c" + std::to_string(c) + "-k" + std::to_string(i);
                    const std::string expected = "c" + std::to_string(c) + "-v" +
                                                 std::to_string(i) + std::string(200, 'x');

                    if (connection.put(key, expected) != CallStatus::Ok) {
                        failures.fetch_add(1);
                        return;
                    }

                    std::string actual;
                    if (connection.get(key, actual) != CallStatus::Ok) {
                        failures.fetch_add(1);
                        return;
                    }
                    if (actual != expected) {
                        mismatches.fetch_add(1);
                    }
                }
            });
        }
    }

    LRD_CHECK_EQ(failures.load(), 0);
    LRD_CHECK_EQ(mismatches.load(), 0);
}

LRD_TEST("concurrent clients contending on the same keys stay consistent") {
    // Disjoint key spaces exercise throughput; a shared key space exercises the
    // lock. Every value is derived from its key, so any interleaving that
    // published a partial or mismatched value is detectable.
    constexpr int kClients = 6;

    const std::string path = temp_socket_path();
    RunningServer server(config_for(path, kClients, /*capacity=*/32));

    std::atomic<int> mismatches{0};

    {
        std::vector<std::jthread> clients;
        clients.reserve(kClients);
        for (int c = 0; c < kClients; ++c) {
            clients.emplace_back([&] {
                Connection connection = Connection::connect(path);
                for (int i = 0; i < 400; ++i) {
                    const std::string key = "hot-" + std::to_string(i % 50);
                    const std::string expected = "value-of-" + key;

                    if (connection.put(key, expected) != CallStatus::Ok) {
                        return;
                    }

                    std::string actual;
                    const CallStatus status = connection.get(key, actual);
                    // NotFound is legitimate: the cache holds 32 entries and 50
                    // keys are in play, so another client's writes can evict
                    // ours between the put and the get. A *wrong* value is not.
                    if (status == CallStatus::Ok && actual != expected) {
                        mismatches.fetch_add(1);
                    }
                }
            });
        }
    }

    LRD_CHECK_EQ(mismatches.load(), 0);
}

LRD_TEST("stats aggregate work from every connection") {
    const std::string path = temp_socket_path();
    RunningServer server(config_for(path, 4));

    constexpr int kClients = 4;
    constexpr int kPuts = 50;
    {
        std::vector<std::jthread> clients;
        for (int c = 0; c < kClients; ++c) {
            clients.emplace_back([&, c] {
                Connection connection = Connection::connect(path);
                for (int i = 0; i < kPuts; ++i) {
                    (void)connection.put("c" + std::to_string(c) + "-" + std::to_string(i), "v");
                }
            });
        }
    }

    Connection observer = Connection::connect(path);
    lrd::proto::Stats stats;
    LRD_REQUIRE(observer.fetch_stats(stats) == CallStatus::Ok);
    LRD_CHECK_EQ(stats.puts, static_cast<std::uint64_t>(kClients * kPuts));
}

// --------------------------------------------------------------------------
// Shutdown
// --------------------------------------------------------------------------

LRD_TEST("request_stop makes run() return and removes the socket file") {
    const std::string path = temp_socket_path();
    ServerConfig config = config_for(path);

    Server server(config);
    LRD_REQUIRE(path_exists(path));

    std::atomic<bool> returned{false};
    std::jthread runner([&] {
        server.run();
        returned.store(true);
    });

    std::this_thread::sleep_for(50ms);
    server.request_stop();
    runner.join();

    LRD_CHECK(returned.load());
    // The listener's destructor is not what removed it - run() closed the
    // listener explicitly, so the path is gone before the process exits.
    LRD_CHECK(!path_exists(path));
}

LRD_TEST("shutdown interrupts a connection blocked waiting for a request") {
    // The case the self-pipe and the connection registry exist for. A client
    // connects and then goes quiet, so its worker is parked in read() with
    // nothing to read. Without shutdown(fd) on that socket, run() would wait
    // for the client to get bored - which could be never.
    const std::string path = temp_socket_path();
    ServerConfig config = config_for(path, 2);

    Server server(config);
    std::jthread runner([&] { server.run(); });
    std::this_thread::sleep_for(50ms);

    Connection idle = Connection::connect(path);
    LRD_REQUIRE(idle.put("k", "v") == CallStatus::Ok);
    // Now the worker is blocked reading the next request that never comes.
    std::this_thread::sleep_for(50ms);

    const auto started = std::chrono::steady_clock::now();
    server.request_stop();
    runner.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Should be near-instant. Generous bound so a slow machine does not produce
    // a false failure, tight enough that a hang is unambiguous.
    LRD_CHECK(elapsed < 3s);
}

LRD_TEST("a client sees a clean disconnect when the daemon stops") {
    const std::string path = temp_socket_path();
    ServerConfig config = config_for(path, 2);

    Server server(config);
    std::jthread runner([&] { server.run(); });
    std::this_thread::sleep_for(50ms);

    Connection client = Connection::connect(path);
    LRD_REQUIRE(client.put("k", "v") == CallStatus::Ok);

    server.request_stop();
    runner.join();

    // The next call fails as a lost connection rather than hanging or
    // returning nonsense.
    std::string value;
    const CallStatus status = client.get("k", value);
    LRD_CHECK(status == CallStatus::ConnectionLost);
    LRD_CHECK(!client.connected());
}

LRD_TEST("stopping an idle server is prompt") {
    const std::string path = temp_socket_path();
    Server server(config_for(path, 4));

    std::jthread runner([&] { server.run(); });
    std::this_thread::sleep_for(50ms);

    const auto started = std::chrono::steady_clock::now();
    server.request_stop();
    runner.join();

    LRD_CHECK(std::chrono::steady_clock::now() - started < 2s);
}

LRD_TEST("a second daemon cannot bind the same live socket") {
    const std::string path = temp_socket_path();
    RunningServer first(config_for(path));

    bool threw = false;
    try {
        Server second(config_for(path));
    } catch (const lrd::SystemError& e) {
        threw = true;
        LRD_CHECK(e.code() == std::errc::address_in_use);
    }
    LRD_CHECK(threw);
}
