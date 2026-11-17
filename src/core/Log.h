#pragma once
// Minimal logging: everything goes to stdout (so ctest captures it) and,
// optionally, to a log file next to a capture.

#include <filesystem>
#include <format>
#include <string_view>

namespace bh::log {

void setFile(const std::filesystem::path& path);  // empty path closes the file
void write(std::string_view level, std::string_view message);

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write("info", std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write("warn", std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write("error", std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace bh::log
