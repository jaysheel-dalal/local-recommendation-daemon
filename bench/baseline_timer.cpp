// lrd_baseline_timer - how late does sleep_until actually wake on this host?
//
// The open-loop mode of lrd_bench measures latency from the moment a request
// was *due*, not from when it was sent. That is the correct fix for coordinated
// omission, but it means any oversleep by the scheduler is charged to the
// server. This probe measures that scheduling floor separately, so an open-loop
// latency figure can be read as "X above the timer's own error" rather than
// taken at face value.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono;

    constexpr int kSamples = 2000;
    constexpr auto kInterval = microseconds(500);

    const auto start = steady_clock::now();
    std::vector<long long> late;
    late.reserve(kSamples);

    for (int i = 0; i < kSamples; ++i) {
        const auto due = start + kInterval * i;
        std::this_thread::sleep_until(due);
        late.push_back(duration_cast<nanoseconds>(steady_clock::now() - due).count());
    }

    std::sort(late.begin(), late.end());

    const auto at = [&late](double p) {
        const auto index =
            static_cast<std::size_t>(p * static_cast<double>(late.size() - 1));
        return static_cast<double>(late[index]) / 1000.0;
    };

    std::printf("sleep_until overshoot at 500us intervals: p50=%.1fus p90=%.1fus p99=%.1fus max=%.1fus\n",
                at(0.50), at(0.90), at(0.99), static_cast<double>(late.back()) / 1000.0);
    return 0;
}
