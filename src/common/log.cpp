#include "lrd/common/log.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace lrd {

namespace {

std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

LogLevel& level_storage() noexcept {
    static LogLevel level = LogLevel::Info;
    return level;
}

constexpr std::string_view level_tag(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

}  // namespace

void log_set_level(LogLevel level) {
    level_storage() = level;
}

LogLevel log_level() noexcept {
    return level_storage();
}

void log_write(LogLevel level, std::string_view line) {
    const auto now = std::chrono::system_clock::now();

    // One formatted string, one fwrite, one lock. Building the line before
    // taking the lock keeps the critical section as short as possible - the
    // formatting is the expensive part and it needs no shared state.
    const std::string out =
        std::format("{:%H:%M:%S} {} {}\n", std::chrono::floor<std::chrono::milliseconds>(now),
                    level_tag(level), line);

    // Everything goes to stderr: it is unbuffered by default, so a log line is
    // on disk before a subsequent crash, and it keeps the daemon's diagnostic
    // output separate from anything it might one day write to stdout.
    const std::lock_guard<std::mutex> guard(log_mutex());
    std::fwrite(out.data(), 1, out.size(), stderr);
}

}  // namespace lrd
