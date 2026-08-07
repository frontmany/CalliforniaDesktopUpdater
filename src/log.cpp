#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace updater::log {
namespace {

std::FILE* gFile = nullptr;
std::mutex gMutex;

}  // namespace

void init(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
#ifdef _WIN32
    std::FILE* opened = _wfopen(file.c_str(), L"ab");
#else
    std::FILE* opened = std::fopen(file.c_str(), "ab");
#endif
    // Same lock as write(): init races nothing today (called once from
    // wWinMain), but a second init must not leak the first handle.
    std::scoped_lock lock{gMutex};
    if (gFile != nullptr) {
        std::fclose(gFile);
    }
    gFile = opened;
}

void write(const std::string_view level, const std::string_view message) {
    std::scoped_lock lock{gMutex};
    if (gFile == nullptr) {
        return;
    }
    const auto now = std::chrono::floor<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    const auto line = std::format("[{:%Y-%m-%d %H:%M:%S}] [{}] {}\n", now, level, message);
    std::fwrite(line.data(), 1, line.size(), gFile);
    std::fflush(gFile);
}

void flushAll() {
    std::scoped_lock lock{gMutex};
    if (gFile != nullptr) {
        std::fflush(gFile);
    }
}

}  // namespace updater::log
