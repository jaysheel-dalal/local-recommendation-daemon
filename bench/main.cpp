// lrd_bench - end-to-end load generator for the daemon.
//
// Drives N client threads over real UNIX domain sockets and reports throughput
// plus p50/p90/p99/p99.9 latency. This measures what a client actually
// experiences: syscalls, framing, codec, lock and all.
//
// For isolating the cache lock from the IPC cost that dominates it, see
// lrd_cache_bench, which drives the same LockedCache in-process with no sockets
// in the way.

#include "latency.hpp"
#include "workload.hpp"

#include "lrd/client/connection.hpp"
#include "lrd/common/errors.hpp"
#include "lrd/common/log.hpp"
#include "lrd/common/version.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
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
using lrd::client::CallStatus;
using lrd::client::Connection;

struct Options {
    std::string socket_path = "/tmp/lrd.sock";
    std::size_t threads = 4;
    std::size_t requests_per_thread = 20000;
    std::size_t warmup_per_thread = 2000;
    std::size_t key_count = 10000;
    std::size_t value_size = 128;
    double read_ratio = 0.9;
    double zipf_theta = 0.99;
    double target_rate = 0.0;  ///< requests/sec/thread; 0 = closed loop
    std::size_t trials = 3;    ///< repeats per point; the median is reported
    bool sweep = false;
    bool show_help = false;
};

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "\n"
        "  --socket PATH      daemon socket (default: /tmp/lrd.sock)\n"
        "  --threads N        concurrent client connections (default: 4)\n"
        "  --requests N       measured requests per thread (default: 20000)\n"
        "  --warmup N         unmeasured requests per thread first (default: 2000)\n"
        "  --keys N           key space size (default: 10000)\n"
        "  --value-size N     value bytes (default: 128)\n"
        "  --read-ratio F     fraction of GETs, 0..1 (default: 0.9)\n"
        "  --zipf F           skew; 0 = uniform (default: 0.99)\n"
        "  --rate F           open-loop target requests/sec/thread (default: 0 = closed loop)\n"
        "  --trials N         repeats per point, median reported (default: 3)\n"
        "  --sweep            run 1,2,4,8,16 threads and print the scaling curve\n"
        "  --help\n",
        argv0);
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

bool parse_size(const char* text, std::size_t& out) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0) {
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
        } else if (arg == "--sweep") {
            out.sweep = true;
        } else if (arg == "--socket" && has_value) {
            out.socket_path = argv[++i];
        } else if (arg == "--threads" && has_value) {
            if (!parse_size(argv[++i], out.threads)) return false;
        } else if (arg == "--requests" && has_value) {
            if (!parse_size(argv[++i], out.requests_per_thread)) return false;
        } else if (arg == "--warmup" && has_value) {
            out.warmup_per_thread = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--keys" && has_value) {
            if (!parse_size(argv[++i], out.key_count)) return false;
        } else if (arg == "--value-size" && has_value) {
            if (!parse_size(argv[++i], out.value_size)) return false;
        } else if (arg == "--read-ratio" && has_value) {
            if (!parse_double(argv[++i], out.read_ratio)) return false;
        } else if (arg == "--zipf" && has_value) {
            if (!parse_double(argv[++i], out.zipf_theta)) return false;
        } else if (arg == "--rate" && has_value) {
            if (!parse_double(argv[++i], out.target_rate)) return false;
        } else if (arg == "--trials" && has_value) {
            if (!parse_size(argv[++i], out.trials)) return false;
        } else {
            std::fprintf(stderr, "lrd_bench: bad argument '%.*s'\n", static_cast<int>(arg.size()),
                         arg.data());
            return false;
        }
    }
    return true;
}

struct RunResult {
    LatencySummary latency;
    double ops_per_sec = 0;
    std::uint64_t errors = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t misses = 0;
};

/// One client thread's workload.
void run_thread(const Options& opts, std::size_t thread_index, const std::vector<std::string>& keys,
                const std::string& value, LatencySamples& samples, std::atomic<std::uint64_t>& errors,
                std::atomic<std::uint64_t>& mismatches, std::atomic<std::uint64_t>& misses,
                std::atomic<bool>& start_flag) {
    // One Connection per thread. Connection is deliberately not thread-safe -
    // it is a single request/response stream, and interleaving two threads'
    // frames on it would desynchronise the protocol.
    Connection connection = Connection::connect(opts.socket_path);

    // Seeded per thread so the threads do not all walk the same key sequence in
    // lockstep, but deterministically so a run is reproducible.
    std::mt19937_64 rng(0x9E3779B97F4A7C15ULL + thread_index);
    KeyDistribution distribution(opts.key_count, opts.zipf_theta);
    std::uniform_real_distribution<double> coin(0.0, 1.0);

    std::string scratch;
    scratch.reserve(opts.value_size + 64);

    // Warmup. Excluded from the numbers because the first requests on a fresh
    // connection pay for buffer growth, page faults and an empty cache - real
    // costs, but one-off ones that would otherwise land entirely in the tail.
    for (std::size_t i = 0; i < opts.warmup_per_thread; ++i) {
        const std::string& key = keys[distribution.next(rng)];
        if (coin(rng) < opts.read_ratio) {
            (void)connection.get(key, scratch);
        } else {
            (void)connection.put(key, value);
        }
    }

    // All threads start measuring together, so the wall-clock window matches
    // the period during which every thread was actually loading the server.
    while (!start_flag.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const auto run_start = std::chrono::steady_clock::now();
    const auto interval = opts.target_rate > 0.0
                              ? std::chrono::nanoseconds(static_cast<std::int64_t>(
                                    1'000'000'000.0 / opts.target_rate))
                              : std::chrono::nanoseconds(0);

    for (std::size_t i = 0; i < opts.requests_per_thread; ++i) {
        const std::string& key = keys[distribution.next(rng)];
        const bool is_read = coin(rng) < opts.read_ratio;

        // Open-loop mode, and why it exists.
        //
        // A closed-loop generator sends request i+1 only after i completes. If
        // the server stalls for 10 ms, a closed-loop client simply sends fewer
        // requests during the stall - so the stall contributes one slow sample
        // instead of the hundreds of delayed requests a real client with a
        // fixed arrival rate would have accumulated. That is *coordinated
        // omission*, and it systematically understates tail latency.
        //
        // With --rate, requests are scheduled at fixed intervals and latency is
        // measured from the time a request was *due*, not from when we got
        // round to sending it. Queueing delay then appears in the numbers,
        // which is the whole point.
        std::chrono::steady_clock::time_point due = run_start;
        if (interval.count() > 0) {
            due = run_start + interval * static_cast<std::int64_t>(i);
            std::this_thread::sleep_until(due);
        }

        const auto issued = std::chrono::steady_clock::now();
        const auto measure_from = (interval.count() > 0) ? due : issued;

        CallStatus status = CallStatus::Ok;
        if (is_read) {
            status = connection.get(key, scratch);
            if (status == CallStatus::NotFound) {
                misses.fetch_add(1, std::memory_order_relaxed);
            } else if (status == CallStatus::Ok && scratch != value) {
                mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            status = connection.put(key, value);
        }

        samples.add(std::chrono::steady_clock::now() - measure_from);

        if (status != CallStatus::Ok && status != CallStatus::NotFound) {
            errors.fetch_add(1, std::memory_order_relaxed);
            if (status == CallStatus::ConnectionLost) {
                return;
            }
        }
    }
}

RunResult run_once(const Options& opts, std::size_t threads) {
    const std::vector<std::string> keys = make_keys(opts.key_count);
    const std::string value = make_value(opts.value_size);

    std::vector<LatencySamples> samples;
    samples.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t) {
        samples.emplace_back(opts.requests_per_thread);
    }

    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> mismatches{0};
    std::atomic<std::uint64_t> misses{0};
    std::atomic<bool> start_flag{false};
    std::atomic<bool> reported{false};

    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point finished;

    {
        std::vector<std::jthread> workers;
        workers.reserve(threads);
        for (std::size_t t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                // An exception escaping a thread's entry function calls
                // std::terminate, which is how a mistyped --socket turned into
                // "terminate called recursively" and no usable diagnostic. The
                // same rule the thread pool's workers follow applies here.
                try {
                    run_thread(opts, t, keys, value, samples[t], errors, mismatches, misses,
                               start_flag);
                } catch (const std::exception& e) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    if (!reported.exchange(true)) {
                        std::fprintf(stderr, "lrd_bench: client thread failed: %s\n", e.what());
                    }
                }
                // A thread that gives up must still release the others, which
                // would otherwise spin on start_flag forever.
                start_flag.store(true, std::memory_order_release);
            });
        }

        // Give every thread time to connect and finish its warmup before the
        // clock starts; otherwise the first thread's measured window would
        // include the others still setting up.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        started = std::chrono::steady_clock::now();
        start_flag.store(true, std::memory_order_release);
        // jthreads join here.
    }
    finished = std::chrono::steady_clock::now();

    RunResult result;
    result.latency = summarize(samples);
    const double seconds = std::chrono::duration<double>(finished - started).count();
    result.ops_per_sec = seconds > 0 ? static_cast<double>(result.latency.count) / seconds : 0.0;
    result.errors = errors.load();
    result.mismatches = mismatches.load();
    result.misses = misses.load();
    return result;
}

void describe(const Options& opts) {
    std::printf("lrd_bench: %s\n", std::string(lrd::build_info()).c_str());
    std::printf("  socket      %s\n", opts.socket_path.c_str());
    std::printf("  keys        %zu (zipf theta %.2f)\n", opts.key_count, opts.zipf_theta);
    std::printf("  value       %zu bytes\n", opts.value_size);
    std::printf("  read ratio  %.2f\n", opts.read_ratio);
    std::printf("  requests    %zu per thread (after %zu warmup)\n", opts.requests_per_thread,
                opts.warmup_per_thread);
    std::printf("  trials      %zu per point (median reported)\n", opts.trials);
    std::printf("  mode        %s\n",
                opts.target_rate > 0.0 ? "open loop (coordinated-omission corrected)"
                                       : "closed loop (see note on coordinated omission)");
    std::printf("\n");
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

    // Errors are what matter here; per-connection chatter would drown the table.
    lrd::log_set_level(lrd::LogLevel::Error);

    try {
        describe(opts);
        print_summary_header();

        std::uint64_t total_errors = 0;
        std::uint64_t total_mismatches = 0;

        const auto measure = [&](std::size_t threads) {
            std::vector<RunResult> runs;
            runs.reserve(opts.trials);
            for (std::size_t trial = 0; trial < opts.trials; ++trial) {
                runs.push_back(run_once(opts, threads));
                total_errors += runs.back().errors;
                total_mismatches += runs.back().mismatches;
            }

            double lowest = runs.front().ops_per_sec;
            double highest = runs.front().ops_per_sec;
            for (const RunResult& run : runs) {
                lowest = std::min(lowest, run.ops_per_sec);
                highest = std::max(highest, run.ops_per_sec);
            }

            const RunResult& median =
                median_by(runs, [](const RunResult& r) { return r.ops_per_sec; });
            print_summary_row(threads, median.ops_per_sec, median.latency);
            if (opts.trials > 1) {
                std::printf("%8s %10.0f-%.0f ops/sec across %zu trials\n", "", lowest, highest,
                            opts.trials);
            }
        };

        if (opts.sweep) {
            for (const std::size_t threads : kSweepThreads) {
                measure(threads);
            }
        } else {
            measure(opts.threads);
        }

        std::printf("\n");
        if (total_mismatches > 0) {
            std::printf("WARNING: %llu value mismatches - the numbers above are meaningless\n",
                        static_cast<unsigned long long>(total_mismatches));
            return 1;
        }
        if (total_errors > 0) {
            std::printf("WARNING: %llu request errors\n",
                        static_cast<unsigned long long>(total_errors));
        }
        return 0;
    } catch (const lrd::SystemError& e) {
        if (e.code() == std::errc::connection_refused ||
            e.code() == std::errc::no_such_file_or_directory) {
            std::fprintf(stderr, "lrd_bench: no daemon on %s - start lrdd first\n",
                         opts.socket_path.c_str());
            return 1;
        }
        std::fprintf(stderr, "lrd_bench: fatal: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lrd_bench: fatal: %s\n", e.what());
        return 1;
    }
}
