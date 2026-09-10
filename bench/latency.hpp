#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace lrd::bench {

/// Per-thread latency samples.
///
/// ## Why per-thread, and not one shared histogram
///
/// The obvious design is a single histogram guarded by a mutex, updated after
/// every request. It is also the design that quietly ruins the measurement:
/// every thread would contend on that mutex once per operation, so the
/// benchmark would be measuring its own instrumentation as much as the server.
/// The contention it adds grows with thread count - exactly the axis being
/// varied - so it would bias the very curve we are drawing.
///
/// Each thread therefore appends to a vector it alone owns, with no
/// synchronisation of any kind. The vectors are merged and sorted once, after
/// the clock has stopped and every thread has joined.
///
/// The storage is reserved up front for the same reason: a std::vector growing
/// mid-run would put an occasional reallocation and copy inside the timed
/// region, showing up as a spurious tail latency that belongs to the benchmark
/// rather than to the daemon.
class LatencySamples {
public:
    explicit LatencySamples(std::size_t expected_count) { nanos.reserve(expected_count); }

    void add(std::chrono::nanoseconds duration) {
        nanos.push_back(static_cast<std::uint64_t>(duration.count()));
    }

    std::vector<std::uint64_t> nanos;
};

struct LatencySummary {
    std::size_t count = 0;
    double min_us = 0;
    double p50_us = 0;
    double p90_us = 0;
    double p99_us = 0;
    double p999_us = 0;
    double max_us = 0;
    double mean_us = 0;
};

/// Nearest-rank percentile: the smallest observed value at or above the given
/// rank.
///
/// Deliberately not interpolated. Linear interpolation between two neighbouring
/// samples reports a latency that no request actually experienced, which for a
/// tail figure is exactly the wrong kind of tidiness - "p99 = 214 us" should
/// mean some request really took 214 us.
[[nodiscard]] inline double percentile_us(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double rank = std::ceil(p * static_cast<double>(sorted.size()));
    auto index = static_cast<std::size_t>(rank);
    index = std::clamp<std::size_t>(index, 1, sorted.size()) - 1;
    return static_cast<double>(sorted[index]) / 1000.0;
}

/// Merges per-thread samples and computes the summary. Sorts internally, so it
/// is O(n log n) - which is fine, because it runs once with the clock stopped.
[[nodiscard]] inline LatencySummary summarize(const std::vector<LatencySamples>& per_thread) {
    std::vector<std::uint64_t> all;
    std::size_t total = 0;
    for (const LatencySamples& samples : per_thread) {
        total += samples.nanos.size();
    }
    all.reserve(total);
    for (const LatencySamples& samples : per_thread) {
        all.insert(all.end(), samples.nanos.begin(), samples.nanos.end());
    }

    LatencySummary summary;
    summary.count = all.size();
    if (all.empty()) {
        return summary;
    }

    std::sort(all.begin(), all.end());

    long double sum = 0;
    for (const std::uint64_t value : all) {
        sum += static_cast<long double>(value);
    }

    summary.min_us = static_cast<double>(all.front()) / 1000.0;
    summary.max_us = static_cast<double>(all.back()) / 1000.0;
    summary.mean_us = static_cast<double>(sum / static_cast<long double>(all.size())) / 1000.0;
    summary.p50_us = percentile_us(all, 0.50);
    summary.p90_us = percentile_us(all, 0.90);
    summary.p99_us = percentile_us(all, 0.99);
    summary.p999_us = percentile_us(all, 0.999);
    return summary;
}

/// Picks the median run by throughput.
///
/// This host is a virtualised one and run-to-run variance was measured at
/// roughly +/-40%, which is more than any change step 7 is likely to make. A
/// single run is therefore an anecdote, not a measurement. Reporting the median
/// of several - rather than the mean - keeps one scheduler hiccup from moving
/// the answer, and printing the spread alongside it keeps the reader honest
/// about how much precision is really there.
template <typename Result, typename Throughput>
[[nodiscard]] const Result& median_by(const std::vector<Result>& runs, Throughput throughput) {
    std::vector<std::size_t> order(runs.size());
    for (std::size_t i = 0; i < runs.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return throughput(runs[a]) < throughput(runs[b]); });
    return runs[order[runs.size() / 2]];
}

inline void print_summary_header() {
    std::printf("%8s %10s %10s %9s %9s %9s %9s %9s\n", "threads", "ops/sec", "ops", "p50 us",
                "p90 us", "p99 us", "p99.9 us", "max us");
}

inline void print_summary_row(std::size_t threads, double ops_per_sec,
                              const LatencySummary& summary) {
    std::printf("%8zu %10.0f %10zu %9.1f %9.1f %9.1f %9.1f %9.1f\n", threads, ops_per_sec,
                summary.count, summary.p50_us, summary.p90_us, summary.p99_us, summary.p999_us,
                summary.max_us);
}

}  // namespace lrd::bench
