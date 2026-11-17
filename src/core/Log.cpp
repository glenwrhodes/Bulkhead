#include "core/Log.h"

#include <cstdio>
#include <fstream>
#include <mutex>

namespace bh::log {
namespace {
std::mutex g_mutex;
std::ofstream g_file;
}  // namespace

void setFile(const std::filesystem::path& path) {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) g_file.close();
    if (!path.empty()) g_file.open(path, std::ios::out | std::ios::trunc);
}

void write(std::string_view level, std::string_view message) {
    std::lock_guard lock(g_mutex);
    std::string line = std::format("[{}] {}\n", level, message);
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
    if (g_file.is_open()) {
        g_file << line;
        g_file.flush();
    }
}

}  // namespace bh::log
