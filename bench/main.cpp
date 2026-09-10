// lrd_bench - concurrent load generator and latency reporter.
//
// Step 0 scaffold. The real implementation (step 6) drives N client threads
// against the daemon, records per-thread latency samples, and reports
// throughput plus p50/p90/p99.

#include "lrd/common/version.hpp"

#include <cstdio>
#include <string_view>

int main() {
    const std::string_view build = lrd::build_info();
    std::printf("%.*s\n", static_cast<int>(build.size()), build.data());
    std::fprintf(stderr, "lrd_bench: not implemented yet (arrives in step 6)\n");
    return 0;
}
