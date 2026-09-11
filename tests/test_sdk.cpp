#include "lrd/sdk/client.hpp"

#include "lrd/daemon/server.hpp"

#include "test_harness.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using lrd::daemon::Server;
using lrd::daemon::ServerConfig;
using lrd::sdk::Client;
using lrd::sdk::ClientConfig;
using lrd::sdk::Status;
using lrd::sdk::StatusCode;
namespace rank = lrd::rank;
using namespace std::chrono_literals;

namespace {

std::string temp_socket_path() {
    static std::atomic<int> counter{0};
    return "/tmp/lrd_sdk_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1)) + ".sock";
}

/// Runs a Server on its own thread and stops it on destruction.
class RunningServer {
public:
    explicit RunningServer(ServerConfig config) : server_(std::move(config)) {
        thread_ = std::jthread([this] { server_.run(); });
        std::this_thread::sleep_for(30ms);
    }

    ~RunningServer() { server_.request_stop(); }

    void stop_now() {
        server_.request_stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    Server server_;
    std::jthread thread_;
};

ServerConfig server_config(const std::string& path, std::size_t threads = 4) {
    ServerConfig config;
    config.socket_path = path;
    config.thread_count = threads;
    config.cache_capacity = 512;
    config.cache_shards = 8;
    return config;
}

ClientConfig client_config(const std::string& path) {
    ClientConfig config;
    config.socket_path = path;
    config.request_timeout = 2000ms;
    config.acquire_timeout = 1000ms;
    config.max_connections = 4;
    return config;
}

rank::Item item_of(rank::ItemId id, const std::string& category, double score,
                   const std::string& advertiser = "acme") {
    rank::Item item;
    item.id = id;
    item.category = category;
    item.advertiser = advertiser;
    item.base_score = score;
    item.created_at = rank::from_epoch_millis(1700000000000LL);
    item.expires_at = rank::from_epoch_millis(0);
    return item;
}

std::unique_ptr<Client> connect_or_fail(const std::string& path) {
    std::unique_ptr<Client> client;
    const Status status = Client::connect(client_config(path), client);
    LRD_REQUIRE(status.ok());
    LRD_REQUIRE(client != nullptr);
    return client;
}

}  // namespace

// --------------------------------------------------------------------------
// Connecting
// --------------------------------------------------------------------------

LRD_TEST("connecting to a daemon that is not running fails with a status, not a throw") {
    // The API's central promise: failure is a returned value. If this threw, the
    // test would abort rather than fail.
    const std::string path = temp_socket_path();

    ClientConfig config = client_config(path);
    std::unique_ptr<Client> client;
    const Status status = Client::connect(std::move(config), client);

    LRD_CHECK(!status);
    LRD_CHECK(status.code() == StatusCode::Unavailable);
    LRD_CHECK(!status.message().empty());
    // `out` is untouched on failure, so a caller cannot mistake a failed connect
    // for a usable client.
    LRD_CHECK(client == nullptr);
}

LRD_TEST("an unknown codec is reported as an invalid argument") {
    const std::string path = temp_socket_path();
    RunningServer server(server_config(path));

    ClientConfig config = client_config(path);
    config.codec = "morse";

    std::unique_ptr<Client> client;
    const Status status = Client::connect(std::move(config), client);
    LRD_CHECK(status.code() == StatusCode::InvalidArgument);
}

LRD_TEST("a zero connection limit is rejected before anything is opened") {
    const std::string path = temp_socket_path();
    ClientConfig config = client_config(path);
    config.max_connections = 0;

    std::unique_ptr<Client> client;
    LRD_CHECK(Client::connect(std::move(config), client).code() == StatusCode::InvalidArgument);
}

// --------------------------------------------------------------------------
// Basic operations
// --------------------------------------------------------------------------

LRD_TEST("put, get and delete round-trip through the SDK") {
    const std::string path = temp_socket_path();
    RunningServer server(server_config(path));
    auto client = connect_or_fail(path);

    LRD_REQUIRE(client->put_item(item_of(7, "tech", 0.75, "globex")).ok());

    rank::Item fetched;
    LRD_REQUIRE(client->get_item(7, fetched).ok());
    LRD_CHECK_EQ(fetched.id, rank::ItemId{7});
    LRD_CHECK_EQ(fetched.category, std::string("tech"));
    LRD_CHECK_EQ(fetched.advertiser, std::string("globex"));
    LRD_CHECK_EQ(fetched.base_score, 0.75);

    LRD_CHECK(client->delete_item(7).ok());
    LRD_CHECK(client->get_item(7, fetched).is_not_found());
}

LRD_TEST("a missing item reports NotFound rather than an error") {
    // is_not_found() exists so a caller does not have to compare codes for what
    // is an ordinary answer.
    const std::string path = temp_socket_path();
    RunningServer server(server_config(path));
    auto client = connect_or_fail(path);

    rank::Item item;
    const Status status = client->get_item(999999, item);
    LRD_CHECK(status.is_not_found());
    LRD_CHECK(!status.ok());
    LRD_CHECK(status.code() == StatusCode::NotFound);
}

LRD_TEST("an invalid request is reported without killing the client") {
    const std::string path = temp_socket_path();
    RunningServer server(server_config(path));
    auto client = connect_or_fail(path);

    // Item id zero is reserved as "unset" and the daemon refuses it.
    const Status status = client->put_item(item_of(0, "tech", 1.0));
    LRD_CHECK(status.code() == StatusCode::InvalidArgument);

    // The connection is still healthy - a rejected request leaves the stream in
    // a known state, so it goes back in the pool.
    LRD_CHECK(client->put_item(item_of(1, "tech", 1.0)).ok());
}

LRD_TEST("recommend and preview both return a ranked slate") {
    const std::string path = temp_socket_path();
    RunningServer server(server_config(path));
    auto client = connect_or_fail(path);

    for (rank::ItemId id = 1; id <= 12; ++id) {
        LRD_REQUIRE(client->put_item(item_of(id, (id % 2 == 0) ? "tech" : "sport",
                                             0.05 * static_cast<double>(id),
                                             "adv-" + std::to_string(id % 3)))
                        .ok());
    }

    rank::UserSignal signal;
    signal.affinities = {{"tech", 1.0}, {"sport", 0.3}};

    std::vector<rank::RankedItem> previewed;
    LRD_REQUIRE(client->preview(signal, 3, previewed).ok());
    LRD_CHECK_EQ(previewed.size(), std::size_t{3});

    std::vector<rank::RankedItem> recommended;
    LRD_REQUIRE(client->recommend(signal, 3, recommended).ok());
    LRD_CHECK_EQ(recommended.size(), std::size_t{3});

    // Scores must come back in non-increasing order.
    for (std::size_t i = 1; i < recommended.size(); ++i) {
        LRD_CHECK(recommended[i - 1].score >= recommended[i].score);
    }
}

LRD_TEST("preview records nothing and recommend does") {
    // The SDK's two ranking entry points differ only in the side effect, and the
    // stats counters are how a caller can tell.
    const std::string path = temp_socket_path();
    RunningServer server(server_config(path));
    auto client = connect_or_fail(path);

    LRD_REQUIRE(client->put_item(item_of(1, "tech", 1.0)).ok());

    rank::UserSignal signal;
    signal.affinities = {{"tech", 1.0}};

    std::vector<rank::RankedItem> items;
    for (int i = 0; i < 3; ++i) {
        LRD_REQUIRE(client->preview(signal, 1, items).ok());
    }

    lrd::sdk::Stats after_previews;
    LRD_REQUIRE(client->stats(after_previews).ok());
    LRD_CHECK_EQ(after_previews.policy_allowed, std::uint64_t{0});

    LRD_REQUIRE(client->recommend(signal, 1, items).ok());

    lrd::sdk::Stats after_recommend;
    LRD_REQUIRE(client->stats(after_recommend).ok());
    LRD_CHECK_EQ(after_recommend.policy_allowed, std::uint64_t{1});
}

LRD_TEST("stats are copied into the SDK's own struct") {
    const std::string path = temp_socket_path();
    RunningServer server(server_config(path));
    auto client = connect_or_fail(path);

    LRD_REQUIRE(client->put_item(item_of(1, "tech", 1.0)).ok());

    lrd::sdk::Stats stats;
    LRD_REQUIRE(client->stats(stats).ok());
    LRD_CHECK_EQ(stats.puts, std::uint64_t{1});
    LRD_CHECK_EQ(stats.capacity, std::uint64_t{512});
}

// --------------------------------------------------------------------------
// The connection pool
// --------------------------------------------------------------------------

LRD_TEST("one client serves many threads concurrently") {
    // The property that makes this an SDK rather than a wrapper. Connection is
    // single-stream and not thread-safe; Client is, because it pools.
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 40;

    const std::string path = temp_socket_path();
    RunningServer server(server_config(path, /*threads=*/8));
    auto client = connect_or_fail(path);

    std::atomic<int> failures{0};
    std::atomic<int> mismatches{0};

    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kOpsPerThread; ++i) {
                    const auto id = static_cast<rank::ItemId>(t * 1000 + i + 1);
                    const rank::Item expected =
                        item_of(id, "cat-" + std::to_string(t), 0.01 * static_cast<double>(i + 1));

                    if (!client->put_item(expected)) {
                        failures.fetch_add(1);
                        return;
                    }

                    rank::Item actual;
                    if (!client->get_item(id, actual)) {
                        failures.fetch_add(1);
                        return;
                    }
                    if (actual.id != expected.id || actual.category != expected.category) {
                        mismatches.fetch_add(1);
                    }
                }
            });
        }
    }

    LRD_CHECK_EQ(failures.load(), 0);
    LRD_CHECK_EQ(mismatches.load(), 0);
}

LRD_TEST("the pool never exceeds its configured size") {
    constexpr std::size_t kMax = 3;

    const std::string path = temp_socket_path();
    RunningServer server(server_config(path, /*threads=*/8));

    ClientConfig config = client_config(path);
    config.max_connections = kMax;

    std::unique_ptr<Client> client;
    LRD_REQUIRE(Client::connect(std::move(config), client).ok());

    std::atomic<std::size_t> overshoots{0};
    {
        std::vector<std::jthread> threads;
        threads.reserve(8);
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 30; ++i) {
                    (void)client->put_item(
                        item_of(static_cast<rank::ItemId>(t * 100 + i + 1), "tech", 0.5));
                    if (client->open_connections() > kMax) {
                        overshoots.fetch_add(1);
                    }
                }
            });
        }
    }

    LRD_CHECK_EQ(overshoots.load(), std::size_t{0});
    LRD_CHECK(client->open_connections() <= kMax);
}

LRD_TEST("a client survives the daemon restarting under it") {
    // The reason a failed connection is dropped rather than returned to the
    // pool. After the daemon goes away every pooled connection is dead; the
    // client must discard them and reconnect rather than hand out corpses.
    const std::string path = temp_socket_path();

    auto server = std::make_unique<RunningServer>(server_config(path));
    auto client = connect_or_fail(path);
    LRD_REQUIRE(client->put_item(item_of(1, "tech", 1.0)).ok());

    server->stop_now();
    server.reset();

    // With nothing listening, calls fail cleanly rather than hanging or
    // crashing.
    const Status down = client->put_item(item_of(2, "tech", 1.0));
    LRD_CHECK(!down);
    LRD_CHECK(down.code() == StatusCode::Unavailable);

    // Bring it back. The client has no idea this happened, and must recover.
    auto restarted = std::make_unique<RunningServer>(server_config(path));
    std::this_thread::sleep_for(50ms);

    LRD_CHECK(client->put_item(item_of(3, "tech", 1.0)).ok());
    rank::Item fetched;
    LRD_CHECK(client->get_item(3, fetched).ok());
}

// --------------------------------------------------------------------------
// Retry policy
// --------------------------------------------------------------------------

LRD_TEST("retries are disabled by setting max_retries to zero") {
    const std::string path = temp_socket_path();
    RunningServer server(server_config(path));

    ClientConfig config = client_config(path);
    config.max_retries = 0;

    std::unique_ptr<Client> client;
    LRD_REQUIRE(Client::connect(std::move(config), client).ok());
    LRD_CHECK(client->put_item(item_of(1, "tech", 1.0)).ok());
}

LRD_TEST("a recording recommendation is not retried by default") {
    // The compliance-driven decision, asserted on configuration rather than by
    // simulating a mid-call failure - which cannot be done deterministically
    // from outside the process.
    //
    // What this pins is that the default is off. The reasoning is in
    // ClientConfig::retry_recording_recommendations: retrying a recommendation
    // that the daemon may already have processed charges its exposure caps
    // twice, so the SDK chooses at-most-once for the operation with a side
    // effect.
    const ClientConfig defaults;
    LRD_CHECK(!defaults.retry_recording_recommendations);

    // And that the safe operations do retry.
    LRD_CHECK(defaults.max_retries > 0);

    // A preview has no side effect, so it is always retried regardless.
    const std::string path = temp_socket_path();
    RunningServer server(server_config(path));
    auto client = connect_or_fail(path);

    LRD_REQUIRE(client->put_item(item_of(1, "tech", 1.0)).ok());

    rank::UserSignal signal;
    signal.affinities = {{"tech", 1.0}};
    std::vector<rank::RankedItem> items;
    LRD_CHECK(client->preview(signal, 1, items).ok());
}

// --------------------------------------------------------------------------
// Timeouts
// --------------------------------------------------------------------------

LRD_TEST("acquiring a connection gives up rather than waiting forever") {
    // With one connection and a short acquire timeout, a thread that cannot get
    // the connection must return Unavailable instead of blocking indefinitely.
    // The assertion is that the whole thing finishes promptly.
    const std::string path = temp_socket_path();
    RunningServer server(server_config(path, /*threads=*/4));

    ClientConfig config = client_config(path);
    config.max_connections = 1;
    config.acquire_timeout = 100ms;

    std::unique_ptr<Client> client;
    LRD_REQUIRE(Client::connect(std::move(config), client).ok());

    const auto started = std::chrono::steady_clock::now();
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 20; ++i) {
                    (void)client->put_item(
                        item_of(static_cast<rank::ItemId>(t * 100 + i + 1), "tech", 0.5));
                }
            });
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Generous, but far below "forever" - a lost notify would blow straight
    // through this.
    LRD_CHECK(elapsed < 30s);
}
