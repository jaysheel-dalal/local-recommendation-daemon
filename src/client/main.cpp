// lrd_cli - command-line client for the daemon.
//
//   lrd_cli put-item ID CATEGORY SCORE [--advertiser NAME] [--age-hours H]
//                                      [--expires-in-hours H]
//   lrd_cli get-item ID
//   lrd_cli del-item ID
//   lrd_cli recommend --signal tech=0.8,sport=0.2 [--exclude gambling,politics]
//                     [--count N] [--dry-run]
//   lrd_cli stats
//   lrd_cli seed N                 populate N generated items
//
// `recommend` is the step 9 milestone: a ranked, filtered slate produced from a
// local signal that never leaves the calling process as raw activity.

#include "lrd/client/connection.hpp"
#include "lrd/common/errors.hpp"
#include "lrd/common/log.hpp"
#include "lrd/rank/item.hpp"
#include "lrd/rank/signal.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using lrd::client::CallStatus;
using lrd::client::Connection;
using lrd::rank::Item;
using lrd::rank::ItemId;
using lrd::rank::RankedItem;
using lrd::rank::UserSignal;

constexpr std::string_view kDefaultSocketPath = "/tmp/lrd.sock";

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [--socket PATH] [--codec NAME] COMMAND [args]\n"
        "\n"
        "  put-item ID CATEGORY SCORE      store an item\n"
        "      --advertiser NAME           default: acme\n"
        "      --age-hours H               how long ago it was created (default: 0)\n"
        "      --expires-in-hours H        0 means never (default: 0)\n"
        "  get-item ID                     fetch an item by id\n"
        "  del-item ID                     delete an item\n"
        "  recommend                       ranked slate for a local signal\n"
        "      --signal tech=0.8,sport=0.2 category affinities\n"
        "      --exclude gambling,politics hard filter\n"
        "      --count N                   how many to return (default: 5)\n"
        "      --dry-run                   rank without recording\n"
        "  seed N                          store N generated items\n"
        "  stats                           daemon counters\n"
        "\n"
        "  --socket PATH                   daemon socket (default: %.*s)\n"
        "  --codec NAME                    must match the daemon (default: binary)\n",
        argv0, static_cast<int>(kDefaultSocketPath.size()), kDefaultSocketPath.data());
}

/// std::from_chars rather than std::stoull: no exceptions, no locale, no
/// allocation, and it reports exactly where parsing stopped - so trailing
/// garbage is an error rather than being silently ignored the way strtoull does.
template <typename T>
bool parse_number(std::string_view text, T& out) {
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parse_double(std::string_view text, double& out) {
    // from_chars for floating point needs the string NUL-terminated in practice
    // on some standard libraries; strtod with an explicit end check is the
    // portable equivalent and still rejects trailing garbage.
    const std::string owned(text);
    char* end = nullptr;
    out = std::strtod(owned.c_str(), &end);
    return end == owned.c_str() + owned.size() && !owned.empty();
}

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(delimiter, start);
        if (end == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

/// Parses "tech=0.8,sport=0.2" into affinities.
bool parse_signal(std::string_view text, UserSignal& signal) {
    if (text.empty()) {
        return true;
    }
    for (const std::string_view entry : split(text, ',')) {
        if (entry.empty()) {
            continue;
        }
        const std::size_t equals = entry.find('=');
        if (equals == std::string_view::npos) {
            std::fprintf(stderr, "lrd_cli: expected category=weight, got '%.*s'\n",
                         static_cast<int>(entry.size()), entry.data());
            return false;
        }
        double weight = 0.0;
        if (!parse_double(entry.substr(equals + 1), weight)) {
            std::fprintf(stderr, "lrd_cli: bad weight in '%.*s'\n",
                         static_cast<int>(entry.size()), entry.data());
            return false;
        }
        signal.affinities.push_back(
            lrd::rank::CategoryAffinity{std::string(entry.substr(0, equals)), weight});
    }
    return true;
}

lrd::rank::Timestamp now_ms() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
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

void print_item(const Item& item) {
    std::printf("id         %llu\n", static_cast<unsigned long long>(item.id));
    std::printf("category   %s\n", item.category.c_str());
    std::printf("advertiser %s\n", item.advertiser.c_str());
    std::printf("base score %.6f\n", item.base_score);
    std::printf("created    %lld ms\n",
                static_cast<long long>(lrd::rank::to_epoch_millis(item.created_at)));
    std::printf("expires    %lld ms%s\n",
                static_cast<long long>(lrd::rank::to_epoch_millis(item.expires_at)),
                item.never_expires() ? "  (never)" : "");
}

int cmd_recommend(Connection& connection, const UserSignal& signal, std::uint32_t count,
                  bool dry_run) {
    std::vector<RankedItem> items;
    const CallStatus status = connection.recommend(signal, count, items, dry_run);
    if (status != CallStatus::Ok) {
        return report(status, connection, "recommend");
    }

    if (items.empty()) {
        lrd::log_info("no eligible candidates");
        return 0;
    }

    std::printf("%-4s %12s %-16s %-14s %s\n", "rank", "id", "category", "advertiser", "score");
    int rank = 1;
    for (const RankedItem& item : items) {
        std::printf("%-4d %12llu %-16s %-14s %.6f\n", rank++,
                    static_cast<unsigned long long>(item.id), item.category.c_str(),
                    item.advertiser.c_str(), item.score);
    }
    return 0;
}

int cmd_stats(Connection& connection) {
    lrd::proto::Stats stats;
    const CallStatus status = connection.fetch_stats(stats);
    if (status != CallStatus::Ok) {
        return report(status, connection, "stats");
    }

    const auto pct = [](std::uint64_t part, std::uint64_t whole) {
        return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    };

    std::printf("requests    %llu\ngets        %llu\nputs        %llu\ndeletes     %llu\n"
                "recommends  %llu\nhits        %llu\nmisses      %llu\nhit rate    %.1f%%\n"
                "evictions   %llu\nentries     %llu / %llu (%.1f%% full)\n",
                static_cast<unsigned long long>(stats.requests),
                static_cast<unsigned long long>(stats.gets),
                static_cast<unsigned long long>(stats.puts),
                static_cast<unsigned long long>(stats.deletes),
                static_cast<unsigned long long>(stats.recommends),
                static_cast<unsigned long long>(stats.hits),
                static_cast<unsigned long long>(stats.misses),
                pct(stats.hits, stats.hits + stats.misses),
                static_cast<unsigned long long>(stats.evictions),
                static_cast<unsigned long long>(stats.entries),
                static_cast<unsigned long long>(stats.capacity),
                pct(stats.entries, stats.capacity));

    std::printf("\ncompliance\n"
                "  allowed            %llu\n"
                "  exposure blocked   %llu\n"
                "  frequency blocked  %llu\n"
                "  store full         %llu\n"
                "  items tracked      %llu\n",
                static_cast<unsigned long long>(stats.policy_allowed),
                static_cast<unsigned long long>(stats.policy_exposure_blocked),
                static_cast<unsigned long long>(stats.policy_frequency_blocked),
                static_cast<unsigned long long>(stats.policy_store_full),
                static_cast<unsigned long long>(stats.policy_tracked));
    return 0;
}

/// Populates the daemon with a spread of categories, advertisers, scores and
/// ages, so that `recommend` has something with structure to rank.
int cmd_seed(Connection& connection, std::size_t count) {
    static constexpr const char* kCategories[] = {"tech",   "sport",  "travel",
                                                  "food",   "music",  "gaming"};
    static constexpr const char* kAdvertisers[] = {"acme", "globex", "initech", "umbrella"};
    constexpr std::size_t kCategoryCount = sizeof(kCategories) / sizeof(kCategories[0]);
    constexpr std::size_t kAdvertiserCount = sizeof(kAdvertisers) / sizeof(kAdvertisers[0]);

    const auto now = now_ms();
    std::size_t stored = 0;

    for (std::size_t i = 0; i < count; ++i) {
        Item item;
        item.id = static_cast<ItemId>(i + 1);
        item.category = kCategories[i % kCategoryCount];
        item.advertiser = kAdvertisers[i % kAdvertiserCount];
        // Scores spread over (0, 1] so ranking has something to order by.
        item.base_score = 0.05 + 0.95 * static_cast<double>(i % 20) / 19.0;
        // Ages spread over ten days, so the recency term matters too.
        item.created_at = now - std::chrono::hours{static_cast<int>(i % 240)};
        item.expires_at = lrd::rank::from_epoch_millis(0);

        if (const CallStatus status = connection.put_item(item); status != CallStatus::Ok) {
            return report(status, connection, "seed");
        }
        ++stored;
    }

    lrd::log_info("seeded {} items across {} categories and {} advertisers", stored,
                  kCategoryCount, kAdvertiserCount);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path{kDefaultSocketPath};
    std::string codec_name{"binary"};

    // Command options, collected before the command is known.
    std::string signal_text;
    std::string exclude_text;
    std::string advertiser{"acme"};
    std::uint32_t count = 5;
    long age_hours = 0;
    long expires_in_hours = 0;
    bool dry_run = false;

    std::vector<std::string_view> args;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = (i + 1) < argc;

        const auto take = [&](std::string& out) {
            if (!has_value) {
                std::fprintf(stderr, "lrd_cli: %.*s requires a value\n",
                             static_cast<int>(arg.size()), arg.data());
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--socket") {
            if (!take(socket_path)) return 2;
        } else if (arg == "--codec") {
            if (!take(codec_name)) return 2;
        } else if (arg == "--signal") {
            if (!take(signal_text)) return 2;
        } else if (arg == "--exclude") {
            if (!take(exclude_text)) return 2;
        } else if (arg == "--advertiser") {
            if (!take(advertiser)) return 2;
        } else if (arg == "--count") {
            std::string text;
            if (!take(text) || !parse_number(text, count)) {
                std::fprintf(stderr, "lrd_cli: --count must be a positive integer\n");
                return 2;
            }
        } else if (arg == "--age-hours") {
            std::string text;
            if (!take(text) || !parse_number(text, age_hours)) {
                std::fprintf(stderr, "lrd_cli: --age-hours must be an integer\n");
                return 2;
            }
        } else if (arg == "--expires-in-hours") {
            std::string text;
            if (!take(text) || !parse_number(text, expires_in_hours)) {
                std::fprintf(stderr, "lrd_cli: --expires-in-hours must be an integer\n");
                return 2;
            }
        } else {
            args.push_back(arg);
        }
    }

    if (args.empty()) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        Connection connection = Connection::connect(socket_path, codec_name);
        const std::string_view command = args[0];

        if (command == "put-item" && args.size() == 4) {
            Item item;
            if (!parse_number(args[1], item.id) || item.id == 0) {
                std::fprintf(stderr, "lrd_cli: ID must be a positive integer\n");
                return 2;
            }
            item.category = std::string(args[2]);
            if (!parse_double(args[3], item.base_score)) {
                std::fprintf(stderr, "lrd_cli: SCORE must be a number\n");
                return 2;
            }
            item.advertiser = advertiser;
            item.created_at = now_ms() - std::chrono::hours{age_hours};
            item.expires_at = expires_in_hours == 0
                                  ? lrd::rank::from_epoch_millis(0)
                                  : now_ms() + std::chrono::hours{expires_in_hours};

            const CallStatus status = connection.put_item(item);
            if (status == CallStatus::Ok) {
                lrd::log_info("stored item {} in category '{}'", item.id, item.category);
            }
            return report(status, connection, "put-item");
        }

        if (command == "get-item" && args.size() == 2) {
            ItemId id = 0;
            if (!parse_number(args[1], id)) {
                std::fprintf(stderr, "lrd_cli: ID must be a positive integer\n");
                return 2;
            }
            Item item;
            const CallStatus status = connection.get_item(id, item);
            if (status == CallStatus::Ok) {
                print_item(item);
            }
            return report(status, connection, "get-item");
        }

        if (command == "del-item" && args.size() == 2) {
            ItemId id = 0;
            if (!parse_number(args[1], id)) {
                std::fprintf(stderr, "lrd_cli: ID must be a positive integer\n");
                return 2;
            }
            const CallStatus status = connection.delete_item(id);
            if (status == CallStatus::Ok) {
                lrd::log_info("deleted item {}", id);
            }
            return report(status, connection, "del-item");
        }

        if (command == "recommend" && args.size() == 1) {
            UserSignal signal;
            if (!parse_signal(signal_text, signal)) {
                return 2;
            }
            for (const std::string_view category : split(exclude_text, ',')) {
                if (!category.empty()) {
                    signal.excluded_categories.push_back(std::string(category));
                }
            }
            return cmd_recommend(connection, signal, count, dry_run);
        }

        if (command == "seed" && args.size() == 2) {
            std::size_t items = 0;
            if (!parse_number(args[1], items) || items == 0) {
                std::fprintf(stderr, "lrd_cli: N must be a positive integer\n");
                return 2;
            }
            return cmd_seed(connection, items);
        }

        if (command == "stats" && args.size() == 1) {
            return cmd_stats(connection);
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
