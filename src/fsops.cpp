#include "fsops.hpp"

#include "sha256.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace updater::fsops {
namespace {

constexpr int kAttempts = 10;

/// 250ms -> ~2s exponential backoff, ~12s worst case per operation.
void backoff(const int attempt) {
    const auto delay = std::min(250 * (1 << attempt), 2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
}

bool isRetryableError(const DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED ||
           error == ERROR_LOCK_VIOLATION;
}

std::string lastErrorText(const char* what, const std::filesystem::path& path) {
    return std::string(what) + " failed for " + path.string() + " (error " +
           std::to_string(GetLastError()) + ")";
}

}  // namespace

std::expected<void, std::string> retryRename(const std::filesystem::path& from,
                                             const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), 0) != 0) {
            return {};
        }
        if (!isRetryableError(GetLastError())) {
            break;
        }
        backoff(attempt);
    }
    return std::unexpected(lastErrorText("rename", from));
}

std::expected<void, std::string> retryDeleteFile(const std::filesystem::path& file) {
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (DeleteFileW(file.c_str()) != 0) {
            return {};
        }
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return {};
        }
        if (!isRetryableError(error)) {
            break;
        }
        backoff(attempt);
    }
    return std::unexpected(lastErrorText("delete", file));
}

std::expected<void, std::string> copyVerified(const std::filesystem::path& from,
                                              const std::filesystem::path& to,
                                              const std::string& expectedSha256) {
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (CopyFileW(from.c_str(), to.c_str(), FALSE) != 0) {
            const auto hash = sha256File(to);
            if (!hash) {
                return std::unexpected(hash.error());
            }
            if (*hash != expectedSha256) {
                return std::unexpected("copy verification failed for " + to.string());
            }
            return {};
        }
        if (!isRetryableError(GetLastError())) {
            break;
        }
        backoff(attempt);
    }
    return std::unexpected(lastErrorText("copy", from));
}

bool probeWritable(const std::filesystem::path& directory) {
    const auto probe = directory / L".update-write-probe";
    const HANDLE handle = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(handle);
    return true;
}

bool isFileLocked(const std::filesystem::path& file) {
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, 0 /* exclusive */, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
    }
    CloseHandle(handle);
    return false;
}

bool removeTreeQuiet(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    return !ec && !std::filesystem::exists(directory, ec);
}

}
