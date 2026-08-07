#pragma once

#include <filesystem>
#include <format>
#include <string_view>

namespace updater::log {

/// Opens (appends to) the log file; call once at startup. Safe to skip — all
/// logging becomes a no-op when never initialized or the open fails.
void init(const std::filesystem::path& file);

void write(std::string_view level, std::string_view message);
void flushAll();

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write("INFO", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write("WARN", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write("ERROR", std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace updater::log
