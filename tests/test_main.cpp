#include "test_harness.hpp"

namespace lrd::test {

int run_all() {
    int failed_cases = 0;
    for (const TestCase& tc : registry()) {
        const int before = failure_count();
        std::cout << "[ RUN ] " << tc.name << "\n";
        try {
            tc.fn();
        } catch (const RequirementFailed&) {
            // Already reported by LRD_REQUIRE; the case is simply abandoned.
        } catch (const std::exception& e) {
            report_failure("<exception>", 0, std::string("uncaught: ") + e.what());
        } catch (...) {
            report_failure("<exception>", 0, "uncaught non-std exception");
        }

        if (failure_count() > before) {
            ++failed_cases;
            std::cout << "[FAILED] " << tc.name << "\n";
        } else {
            std::cout << "[  OK  ] " << tc.name << "\n";
        }
    }

    std::cout << "\n"
              << registry().size() << " case(s), " << failed_cases << " failed, "
              << failure_count() << " assertion failure(s)\n";
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace lrd::test

int main() {
    return lrd::test::run_all();
}
