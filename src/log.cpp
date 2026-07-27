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
    gFile = _wfopen(file.c_str(), L"ab");
#else
    gFile = std::fopen(file.c_str(), "ab");
#endif
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

}
