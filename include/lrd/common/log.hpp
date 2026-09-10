#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace lrd {

enum class LogLevel { Debug, Info, Warn, Error };

/// Writes one already-formatted line. Thread-safe: the implementation holds a
/// mutex across the write so that two worker threads logging at once cannot
/// interleave halves of their lines. From step 5 onward several threads log
/// concurrently, so an unsynchronised fprintf here would be a genuine data
/// race on the FILE* - the kind TSan would (correctly) flag.
void log_write(LogLevel level, std::string_view line);

/// Sets the minimum level that will be emitted. Not synchronised with readers
/// on purpose; it is set once at startup before threads exist.
void log_set_level(LogLevel level);

[[nodiscard]] LogLevel log_level() noexcept;

namespace detail {

/// Formats then writes. Kept out of line of the macros below so the formatting
/// cost is skipped entirely when the level is filtered out.
template <typename... Args>
void log_at(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    if (level < log_level()) {
        return;
    }
    log_write(level, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace detail

/// std::format rather than printf-style varargs: the format string is checked
/// against the argument types at compile time, so a mismatched {} is a build
/// error instead of a runtime crash. printf's %s with a std::string argument
/// is undefined behaviour that compiles fine without -Wformat.
template <typename... Args>
void log_debug(std::format_string<Args...> fmt, Args&&... args) {
    detail::log_at(LogLevel::Debug, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void log_info(std::format_string<Args...> fmt, Args&&... args) {
    detail::log_at(LogLevel::Info, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void log_warn(std::format_string<Args...> fmt, Args&&... args) {
    detail::log_at(LogLevel::Warn, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void log_error(std::format_string<Args...> fmt, Args&&... args) {
    detail::log_at(LogLevel::Error, fmt, std::forward<Args>(args)...);
}

}  // namespace lrd
