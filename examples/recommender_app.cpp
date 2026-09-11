// An example "app" consuming the lrd SDK.
//
// This file is the milestone for step 11, and what it *does not* include is the
// point: only <lrd/sdk/...> and the two public data headers. No sockets, no
// codec, no framing, no protobuf, nothing from src/. If this stops compiling
// after an internal change, the SDK has leaked its implementation.
//
// It also links only the `lrd::sdk` target - not lrd_core - which is what makes
// that claim checkable rather than aspirational. scripts/verify-sdk-install.sh
// goes further: it installs the SDK to a throwaway prefix and builds this file
// against it with find_package(lrd), from outside the source tree entirely.

#include <lrd/rank/item.hpp>
#include <lrd/rank/signal.hpp>
#include <lrd/sdk/client.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

lrd::rank::Timestamp now() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
}

/// Publishes a small catalogue so there is something to rank.
bool publish_catalogue(lrd::sdk::Client& client) {
    static constexpr const char* kCategories[] = {"tech", "sport", "travel", "food"};
    static constexpr const char* kAdvertisers[] = {"acme", "globex", "initech"};

    for (int i = 0; i < 24; ++i) {
        lrd::rank::Item item;
        item.id = static_cast<lrd::rank::ItemId>(i + 1);
        item.category = kCategories[i % 4];
        item.advertiser = kAdvertisers[i % 3];
        item.base_score = 0.1 + 0.9 * static_cast<double>(i % 10) / 9.0;
        item.created_at = now() - std::chrono::hours{i * 6};
        item.expires_at = lrd::rank::from_epoch_millis(0);  // never expires

        if (const lrd::sdk::Status status = client.put_item(item); !status) {
            std::fprintf(stderr, "put_item(%d) failed: %s (%s)\n", i + 1,
                         lrd::sdk::to_string(status.code()), status.message().c_str());
            return false;
        }
    }
    return true;
}

void print_slate(const char* label, const std::vector<lrd::rank::RankedItem>& items) {
    std::printf("\n%s\n", label);
    if (items.empty()) {
        std::printf("  (nothing eligible)\n");
        return;
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        std::printf("  %zu. item %-3llu  %-8s  %-8s  %.4f\n", i + 1,
                    static_cast<unsigned long long>(items[i].id), items[i].category.c_str(),
                    items[i].advertiser.c_str(), items[i].score);
    }
}

}  // namespace

int main(int argc, char** argv) {
    lrd::sdk::ClientConfig config;
    config.socket_path = (argc > 1) ? argv[1] : "/tmp/lrd.sock";
    config.request_timeout = std::chrono::seconds{2};
    config.max_connections = 4;

    std::unique_ptr<lrd::sdk::Client> client;
    if (const lrd::sdk::Status status = lrd::sdk::Client::connect(config, client); !status) {
        // Note the shape: no try/catch anywhere in this file. Failure is a
        // returned value, which is the whole point of the Status type.
        std::fprintf(stderr, "cannot connect: %s\n", status.message().c_str());
        return 1;
    }

    if (!publish_catalogue(*client)) {
        return 1;
    }

    lrd::rank::UserSignal signal;
    signal.affinities = {{"tech", 1.0}, {"travel", 0.4}};
    signal.excluded_categories = {"food"};

    // preview() ranks without recording, so it can be called repeatedly to see
    // what would be served without spending any exposure.
    std::vector<lrd::rank::RankedItem> preview;
    if (const lrd::sdk::Status status = client->preview(signal, 5, preview); !status) {
        std::fprintf(stderr, "preview failed: %s\n", status.message().c_str());
        return 1;
    }
    print_slate("preview (records nothing):", preview);

    // recommend() is the recording call - each returned item is charged against
    // its caps.
    std::vector<lrd::rank::RankedItem> slate;
    if (const lrd::sdk::Status status = client->recommend(signal, 5, slate); !status) {
        std::fprintf(stderr, "recommend failed: %s\n", status.message().c_str());
        return 1;
    }
    print_slate("recommend (records exposure):", slate);

    // A miss is an ordinary answer, not an error - which is why is_not_found()
    // exists rather than making the caller compare codes.
    lrd::rank::Item missing;
    if (const lrd::sdk::Status status = client->get_item(999999, missing);
        status.is_not_found()) {
        std::printf("\nitem 999999: not found (as expected)\n");
    } else if (!status) {
        std::fprintf(stderr, "get_item failed: %s\n", status.message().c_str());
        return 1;
    }

    lrd::sdk::Stats stats;
    if (const lrd::sdk::Status status = client->stats(stats); !status) {
        std::fprintf(stderr, "stats failed: %s\n", status.message().c_str());
        return 1;
    }

    std::printf("\ndaemon counters\n");
    std::printf("  items stored      %llu / %llu\n",
                static_cast<unsigned long long>(stats.entries),
                static_cast<unsigned long long>(stats.capacity));
    std::printf("  recommendations   %llu\n",
                static_cast<unsigned long long>(stats.recommends));
    std::printf("  policy allowed    %llu\n",
                static_cast<unsigned long long>(stats.policy_allowed));
    std::printf("  exposure blocked  %llu\n",
                static_cast<unsigned long long>(stats.policy_exposure_blocked));
    std::printf("  connections open  %zu\n", client->open_connections());

    return 0;
}
