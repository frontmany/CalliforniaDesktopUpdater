#include "fsops.hpp"

#include "sha256.hpp"
#include "watchdog.hpp"
#include "win_raii.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace updater::fsops {
namespace {

constexpr int kAttempts = 10;

/// 250 ms -> 2 s exponential backoff; ~14 s worst case per operation
/// (no sleep after the final failed attempt).
///
/// Bounded per operation was never the same as bounded per run: a full
/// reinstall is over a thousand operations, and a machine that makes every one
/// of them retry turns "fourteen seconds" into most of a day with the app shut
/// down and nothing on screen. The sleep is capped by what is left of the run's
/// work budget, and once that is gone retrying stops altogether -- see
/// budgetExhausted().
void backoff(const int attempt) {
    const auto delay = std::chrono::milliseconds{std::min(250 * (1 << attempt), 2000)};
    std::this_thread::sleep_for(std::min(delay, watchdog::workBudget()));
}

/// No time left for patience. The operation still gets its attempts, back to
/// back; only the waiting between them is dropped.
[[nodiscard]] bool budgetExhausted() {
    return watchdog::workBudget() <= std::chrono::milliseconds{0};
}

bool isRetryableError(const DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED ||
           error == ERROR_LOCK_VIOLATION;
}

/// `error` must be captured immediately after the failing API call —
/// allocations and sleeps in between can clobber the thread's last error.
std::string errorText(const char* what, const std::filesystem::path& path, const DWORD error) {
    return std::string(what) + " failed for " + path.string() + " (error " +
           std::to_string(error) + ")";
}

std::expected<void, std::string> ensureParentDir(const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return std::unexpected("create directories failed for " + target.parent_path().string() +
                               " (" + ec.message() + ")");
    }
    return {};
}

std::expected<void, std::string> retryMove(const std::filesystem::path& from,
                                           const std::filesystem::path& to,
                                           const DWORD flags) {
    if (const auto parent = ensureParentDir(to); !parent) {
        return parent;
    }
    DWORD lastError = 0;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), flags) != 0) {
            return {};
        }
        lastError = GetLastError();
        if (!isRetryableError(lastError) || attempt + 1 == kAttempts || budgetExhausted()) {
            break;
        }
        backoff(attempt);
    }
    return std::unexpected(errorText("rename", from, lastError));
}

}  // namespace

std::expected<void, std::string> retryRename(const std::filesystem::path& from,
                                             const std::filesystem::path& to) {
    return retryMove(from, to, 0);
}

std::expected<void, std::string> retryReplaceFile(const std::filesystem::path& from,
                                                  const std::filesystem::path& to) {
    return retryMove(from, to, MOVEFILE_REPLACE_EXISTING);
}

std::expected<void, std::string> retryDeleteFile(const std::filesystem::path& file) {
    DWORD lastError = 0;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (DeleteFileW(file.c_str()) != 0) {
            return {};
        }
        lastError = GetLastError();
        if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
            return {};
        }
        if (!isRetryableError(lastError) || attempt + 1 == kAttempts || budgetExhausted()) {
            break;
        }
        backoff(attempt);
    }
    return std::unexpected(errorText("delete", file, lastError));
}

std::expected<void, std::string> copyVerified(const std::filesystem::path& from,
                                              const std::filesystem::path& to,
                                              const std::string& expectedSha256) {
    if (const auto parent = ensureParentDir(to); !parent) {
        return parent;
    }
    DWORD lastError = 0;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (CopyFileW(from.c_str(), to.c_str(), FALSE) != 0) {
            const auto hash = sha256File(to);
            if (hash && *hash == expectedSha256) {
                return {};
            }
            // Do not leave a corrupt destination behind for the caller.
            (void)retryDeleteFile(to);
            if (!hash) {
                return std::unexpected(hash.error());
            }
            return std::unexpected("copy verification failed for " + to.string());
        }
        lastError = GetLastError();
        if (!isRetryableError(lastError) || attempt + 1 == kAttempts || budgetExhausted()) {
            break;
        }
        backoff(attempt);
    }
    return std::unexpected(errorText("copy", from, lastError));
}

bool probeWritable(const std::filesystem::path& directory) {
    const auto probe = directory / L".update-write-probe";
    const UniqueHandle handle{CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                          nullptr)};
    return handle.valid();
}

bool isFileLocked(const std::filesystem::path& file) {
    const UniqueHandle handle{CreateFileW(file.c_str(), GENERIC_READ, 0 /* exclusive */, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (handle.valid()) {
        return false;
    }
    const auto error = GetLastError();
    // A missing file is free to swap; any other failure (sharing violation,
    // access denied, ...) means we could not touch it either — report locked
    // so callers wait instead of swapping blind.
    return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
}

void removeTreeQuiet(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

}  // namespace updater::fsops
