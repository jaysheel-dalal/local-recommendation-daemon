// lrd_codec_bench - in-process encode/decode microbenchmark.
//
// ## Why this cannot be an end-to-end measurement
//
// Step 6 established the shape of this system: an IPC round trip on this host
// costs ~90 us, while encoding a 40-byte message is tens of nanoseconds. Running
// `lrd_bench --codec protobuf` and comparing throughput would measure the
// kernel, not the codec, and would report "no difference" for two formats that
// differ by an order of magnitude in CPU cost.
//
// That is the same trap `lrd_cache_bench` exists to avoid, and step 7 showed the
// cost of getting it wrong in the other direction: reasoning from an aggregate
// where the effect is buried produced a prediction that turned out to be false.
//
// So this tool measures the codec and nothing else: encode, decode, and bytes on
// the wire, per message type, with no syscalls in the loop.

#include "latency.hpp"

#include "lrd/common/version.hpp"
#include "lrd/proto/codec.hpp"
#include "lrd/proto/message.hpp"
#include "lrd/rank/item.hpp"

#ifdef LRD_WITH_PROTOBUF
#include "lrd/proto/protobuf_codec.hpp"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace lrd::bench;
using namespace lrd::proto;

struct Options {
    std::size_t iterations = 200000;
    std::size_t warmup = 20000;
    std::size_t category_size = 8;
    std::size_t recommend_items = 5;
    std::size_t trials = 3;
    bool show_help = false;
};

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "\n"
        "  --iterations N   encode/decode pairs per message type (default: 200000)\n"
        "  --category-size N  category bytes (default: 8)\n"
        "  --items N          items in a recommendation response (default: 5)\n"
        "  --trials N       repeats, median reported (default: 3)\n"
        "  --help\n",
        argv0);
}

bool parse_size(const char* text, std::size_t& out) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

bool parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = (i + 1) < argc;

        if (arg == "--help" || arg == "-h") {
            out.show_help = true;
        } else if (arg == "--iterations" && has_value) {
            if (!parse_size(argv[++i], out.iterations)) return false;
        } else if (arg == "--category-size" && has_value) {
            if (!parse_size(argv[++i], out.category_size)) return false;
        } else if (arg == "--items" && has_value) {
            if (!parse_size(argv[++i], out.recommend_items)) return false;
        } else if (arg == "--trials" && has_value) {
            if (!parse_size(argv[++i], out.trials)) return false;
        } else {
            std::fprintf(stderr, "lrd_codec_bench: bad argument '%.*s'\n",
                         static_cast<int>(arg.size()), arg.data());
            return false;
        }
    }
    return true;
}

std::string filler(std::size_t size, char base) {
    std::string out(size, base);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<char>(base + static_cast<char>(i % 26));
    }
    return out;
}

struct CaseResult {
    double encode_ns = 0;
    double decode_ns = 0;
    std::size_t wire_bytes = 0;
};

/// Times encode and decode separately.
///
/// The buffer is cleared but never freed between iterations, matching how the
/// daemon actually uses it - a scratch buffer reused for the life of a
/// connection. Measuring with a fresh allocation each time would report the
/// allocator's speed as if it were the codec's.
template <typename Message>
CaseResult time_message(const Codec& codec, const Message& message, std::size_t iterations,
                        std::size_t warmup) {
    ByteBuffer buffer;
    Message decoded;

    for (std::size_t i = 0; i < warmup; ++i) {
        buffer.clear();
        codec.encode(message, buffer);
        (void)codec.decode(buffer, decoded);
    }

    // Encode.
    const auto encode_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        buffer.clear();
        codec.encode(message, buffer);
    }
    const auto encode_elapsed = std::chrono::steady_clock::now() - encode_start;

    // Decode, from a buffer encoded once outside the timed region.
    buffer.clear();
    codec.encode(message, buffer);
    const std::size_t wire_bytes = buffer.size();

    const auto decode_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        (void)codec.decode(buffer, decoded);
    }
    const auto decode_elapsed = std::chrono::steady_clock::now() - decode_start;

    const auto per_op = [iterations](std::chrono::steady_clock::duration total) {
        return std::chrono::duration<double, std::nano>(total).count() /
               static_cast<double>(iterations);
    };

    return CaseResult{per_op(encode_elapsed), per_op(decode_elapsed), wire_bytes};
}

CaseResult median_of(std::vector<CaseResult> runs) {
    std::sort(runs.begin(), runs.end(), [](const CaseResult& a, const CaseResult& b) {
        return a.encode_ns + a.decode_ns < b.encode_ns + b.decode_ns;
    });
    return runs[runs.size() / 2];
}

void report(const char* label, const CaseResult& result) {
    std::printf("%-20s %12.1f %12.1f %12zu\n", label, result.encode_ns, result.decode_ns,
                result.wire_bytes);
}

template <typename Message>
void measure(const char* label, const Codec& codec, const Message& message,
             const Options& opts) {
    std::vector<CaseResult> runs;
    runs.reserve(opts.trials);
    for (std::size_t trial = 0; trial < opts.trials; ++trial) {
        runs.push_back(time_message(codec, message, opts.iterations, opts.warmup));
    }
    report(label, median_of(std::move(runs)));
}

lrd::rank::Item bench_item(const Options& opts) {
    lrd::rank::Item item;
    item.id = 1234567890123456789ULL;
    item.category = filler(opts.category_size, 'a');
    item.advertiser = filler(opts.category_size, 'A');
    item.base_score = 0.8125;
    item.created_at = lrd::rank::from_epoch_millis(1700000000000LL);
    item.expires_at = lrd::rank::from_epoch_millis(1800000000000LL);
    return item;
}

void run_codec(const Codec& codec, const Options& opts) {
    std::printf("\n=== %s ===\n", codec.name());
    std::printf("%-20s %12s %12s %12s\n", "message", "encode ns", "decode ns", "wire bytes");

    const std::uint64_t request_id = 1234567890123456789ULL;

    Request get;
    get.request_id = request_id;
    get.body = GetItem{42};
    measure("GetItemRequest", codec, get, opts);

    Request put;
    put.request_id = request_id;
    put.body = PutItem{bench_item(opts)};
    measure("PutItemRequest", codec, put, opts);

    Request stats_request;
    stats_request.request_id = 42;
    stats_request.body = GetStats{};
    measure("StatsRequest", codec, stats_request, opts);

    Recommend recommend;
    recommend.signal.affinities = {{"tech", 0.9}, {"sport", 0.5}, {"travel", 0.2}};
    recommend.signal.excluded_categories = {"gambling"};
    recommend.count = static_cast<std::uint32_t>(opts.recommend_items);
    Request recommend_request;
    recommend_request.request_id = request_id;
    recommend_request.body = recommend;
    measure("RecommendRequest", codec, recommend_request, opts);

    Response get_response;
    get_response.request_id = request_id;
    get_response.body = GetItemResult{StatusCode::Ok, bench_item(opts)};
    measure("GetItemResponse", codec, get_response, opts);

    Response put_response;
    put_response.request_id = request_id;
    put_response.body = PutItemResult{StatusCode::Ok};
    measure("PutItemResponse", codec, put_response, opts);

    RecommendResult recommend_result;
    recommend_result.status = StatusCode::Ok;
    for (std::size_t i = 0; i < opts.recommend_items; ++i) {
        recommend_result.items.push_back(lrd::rank::RankedItem{
            static_cast<lrd::rank::ItemId>(i + 1), 1.0 / static_cast<double>(i + 1),
            filler(opts.category_size, 'a'), filler(opts.category_size, 'A')});
    }
    Response recommend_response;
    recommend_response.request_id = request_id;
    recommend_response.body = recommend_result;
    measure("RecommendResponse", codec, recommend_response, opts);

    Response stats_response;
    stats_response.body = StatsResult{StatusCode::Ok, Stats{100, 60, 30, 10, 5, 45, 15, 4, 20, 64}};
    measure("StatsResponse", codec, stats_response, opts);
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

    std::printf("lrd_codec_bench: %s\n", std::string(lrd::build_info()).c_str());
    std::printf("  no sockets: encode/decode CPU cost and wire size only\n");
    std::printf("  category/advertiser %zu bytes, %zu recommended items, %zu iterations, "
                "median of %zu trials\n",
                opts.category_size, opts.recommend_items, opts.iterations, opts.trials);

    const BinaryCodec binary;
    run_codec(binary, opts);

#ifdef LRD_WITH_PROTOBUF
    const ProtobufCodec protobuf;
    run_codec(protobuf, opts);
#else
    std::printf("\n(protobuf codec not built - configure with -DLRD_WITH_PROTOBUF=ON)\n");
#endif

    return 0;
}
