// lrd_cache_bench - in-process contention benchmark for the locked cache.
//
// ## Why this exists separately from lrd_bench
//
// Step 2 measured the IPC round trip on this machine at roughly 105 us, almost
// all of it syscall and context-switch cost. The cache's critical section is a
// hash lookup and a list splice - hundreds of nanoseconds. The lock is
// therefore something like 0.3% of an end-to-end request.
//
// That has a consequence worth being explicit about: **lock contention is
// invisible in an end-to-end benchmark.** Sharding the cache could double its
// throughput and lrd_bench would not move, because lrd_bench is measuring
// syscalls. Reporting "sharding made no difference" from that measurement would
// be true and completely misleading.
//
// So this benchmark removes the sockets entirely and drives LockedCache
// directly from N threads. It measures the thing step 7 is about to change, at
// a resolution where the change is visible - and lrd_bench remains the honest
// answer to "what does a client actually experience".

#include "latency.hpp"
#include "workload.hpp"

#include "lrd/cache/locked_cache.hpp"
#include "lrd/common/version.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

/// Explicitly std::size_t. A braced list of plain ints here trips
/// -Wsign-conversion, which is exactly the warning we want switched on for a
/// codebase full of sizes and lengths.
constexpr std::array<std::size_t, 5> kSweepThreads{1, 2, 4, 8, 16};

using namespace lrd::bench;
using Cache = lrd::cache::LockedCache<std::string, std::string>;

struct Options {
    std::size_t threads = 4;
    std::size_t ops_per_thread = 200000;
    std::size_t warmup_per_thread = 20000;
    std::size_t key_count = 10000;
    std::size_t capacity = 5000;
    std::size_t value_size = 128;
    double read_ratio = 0.9;
    double zipf_theta = 0.99;
    std::size_t trials = 3;  ///< repeats per point; the median is reported
    bool sweep = false;
    bool show_help = false;
};

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "\n"
        "  --threads N      concurrent threads (default: 4)\n"
        "  --ops N          operations per thread (default: 200000)\n"
        "  --keys N         key space size (default: 10000)\n"
        "  --capacity N     cache entries (default: 5000)\n"
        "  --value-size N   value bytes (default: 128)\n"
        "  --read-ratio F   fraction of GETs, 0..1 (default: 0.9)\n"
        "  --zipf F         skew; 0 = uniform (default: 0.99)\n"
        "  --trials N       repeats per point, median reported (default: 3)\n"
        "  --sweep          run 1,2,4,8,16 threads and print the scaling curve\n"
        "  --help\n",
        argv0);
}

bool parse_size(const char* text, std::size_t& out) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0) {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

bool parse_double(const char* text, double& out) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

bool parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = (i + 1) < argc;

        if (arg == "--help" || arg == "-h") {
            out.show_help = true;
        } else if (arg == "--sweep") {
            out.sweep = true;
        } else if (arg == "--threads" && has_value) {
            if (!parse_size(argv[++i], out.threads)) return false;
        } else if (arg == "--ops" && has_value) {
            if (!parse_size(argv[++i], out.ops_per_thread)) return false;
        } else if (arg == "--keys" && has_value) {
            if (!parse_size(argv[++i], out.key_count)) return false;
        } else if (arg == "--capacity" && has_value) {
            if (!parse_size(argv[++i], out.capacity)) return false;
        } else if (arg == "--value-size" && has_value) {
            if (!parse_size(argv[++i], out.value_size)) return false;
        } else if (arg == "--read-ratio" && has_value) {
            if (!parse_double(argv[++i], out.read_ratio)) return false;
        } else if (arg == "--zipf" && has_value) {
            if (!parse_double(argv[++i], out.zipf_theta)) return false;
        } else if (arg == "--trials" && has_value) {
            if (!parse_size(argv[++i], out.trials)) return false;
        } else {
            std::fprintf(stderr, "lrd_cache_bench: bad argument '%.*s'\n",
                         static_cast<int>(arg.size()), arg.data());
            return false;
        }
    }
    return true;
}

struct RunResult {
    LatencySummary latency;
    double ops_per_sec = 0;
    double hit_rate = 0;
};

RunResult run_once(const Options& opts, std::size_t threads) {
    const std::vector<std::string> keys = make_keys(opts.key_count);
    const std::string value = make_value(opts.value_size);

    Cache cache(opts.capacity);

    std::vector<LatencySamples> samples;
    samples.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t) {
        samples.emplace_back(opts.ops_per_thread);
    }

    std::atomic<bool> start_flag{false};
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> reads{0};

    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point finished;

    {
        std::vector<std::jthread> workers;
        workers.reserve(threads);

        for (std::size_t t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                std::mt19937_64 rng(0x9E3779B97F4A7C15ULL + t);
                KeyDistribution distribution(opts.key_count, opts.zipf_theta);
                std::uniform_real_distribution<double> coin(0.0, 1.0);

                // Warm the cache so the measured window is steady-state rather
                // than dominated by cold-start insertions.
                for (std::size_t i = 0; i < opts.warmup_per_thread; ++i) {
                    const std::string& key = keys[distribution.next(rng)];
                    if (coin(rng) < opts.read_ratio) {
                        (void)cache.get(key);
                    } else {
                        cache.put(key, value);
                    }
                }

                while (!start_flag.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                std::uint64_t local_hits = 0;
                std::uint64_t local_reads = 0;

                for (std::size_t i = 0; i < opts.ops_per_thread; ++i) {
                    const std::string& key = keys[distribution.next(rng)];
                    const bool is_read = coin(rng) < opts.read_ratio;

                    const auto issued = std::chrono::steady_clock::now();
                    if (is_read) {
                        ++local_reads;
                        if (cache.get(key) != nullptr) {
                            ++local_hits;
                        }
                    } else {
                        cache.put(key, value);
                    }
                    samples[t].add(std::chrono::steady_clock::now() - issued);
                }

                // Counters folded in once at the end rather than incremented per
                // operation: an atomic in the inner loop would itself be a point
                // of contention and would pollute the measurement.
                hits.fetch_add(local_hits, std::memory_order_relaxed);
                reads.fetch_add(local_reads, std::memory_order_relaxed);
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        started = std::chrono::steady_clock::now();
        start_flag.store(true, std::memory_order_release);
    }
    finished = std::chrono::steady_clock::now();

    RunResult result;
    result.latency = summarize(samples);
    const double seconds = std::chrono::duration<double>(finished - started).count();
    result.ops_per_sec = seconds > 0 ? static_cast<double>(result.latency.count) / seconds : 0.0;
    const std::uint64_t total_reads = reads.load();
    result.hit_rate = total_reads == 0 ? 0.0
                                       : 100.0 * static_cast<double>(hits.load()) /
                                             static_cast<double>(total_reads);
    return result;
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

    std::printf("lrd_cache_bench: %s\n", std::string(lrd::build_info()).c_str());
    std::printf("  in-process, no sockets: isolates the cache mutex\n");
    std::printf("  keys %zu (zipf %.2f), capacity %zu, value %zu bytes, read ratio %.2f\n\n",
                opts.key_count, opts.zipf_theta, opts.capacity, opts.value_size, opts.read_ratio);

    // Latencies here are sub-microsecond, so the shared header's microsecond
    // columns would round most of them to 0.0. Nanoseconds instead.
    std::printf("  %zu trials per point; median reported, [min-max] alongside\n\n",
                opts.trials);
    std::printf("%8s %12s %10s %10s %10s %10s\n", "threads", "ops/sec", "p50 ns", "p99 ns",
                "p99.9 ns", "hit rate");

    const auto measure = [&](std::size_t threads) {
        std::vector<RunResult> runs;
        runs.reserve(opts.trials);
        for (std::size_t trial = 0; trial < opts.trials; ++trial) {
            runs.push_back(run_once(opts, threads));
        }

        double lowest = runs.front().ops_per_sec;
        double highest = runs.front().ops_per_sec;
        for (const RunResult& run : runs) {
            lowest = std::min(lowest, run.ops_per_sec);
            highest = std::max(highest, run.ops_per_sec);
        }

        const RunResult& result =
            median_by(runs, [](const RunResult& r) { return r.ops_per_sec; });
        std::printf("%8zu %12.0f %10.0f %10.0f %10.0f %9.1f%%", threads, result.ops_per_sec,
                    result.latency.p50_us * 1000.0, result.latency.p99_us * 1000.0,
                    result.latency.p999_us * 1000.0, result.hit_rate);
        if (opts.trials > 1) {
            std::printf("   [%.0f-%.0f]", lowest, highest);
        }
        std::printf("\n");
    };

    if (opts.sweep) {
        for (const std::size_t threads : kSweepThreads) {
            measure(threads);
        }
    } else {
        measure(opts.threads);
    }

    return 0;
}
