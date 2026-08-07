#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace updater::fsops {

/// Rename with retry/backoff on sharing violations (AV scanners, straggler
/// handles). Same-volume renames are atomic on NTFS. Creates parent dirs of
/// `to`. Fails if `to` exists.
[[nodiscard]] std::expected<void, std::string> retryRename(const std::filesystem::path& from,
                                                           const std::filesystem::path& to);

/// Like retryRename, but atomically replaces `to` when it exists
/// (MOVEFILE_REPLACE_EXISTING) — there is never a moment with no file at `to`.
[[nodiscard]] std::expected<void, std::string> retryReplaceFile(const std::filesystem::path& from,
                                                                const std::filesystem::path& to);

/// Delete a file with the same retry policy. Missing file is success.
[[nodiscard]] std::expected<void, std::string> retryDeleteFile(const std::filesystem::path& file);

/// Copy `from` to `to` (creating parent dirs), then verify the copy's SHA-256
/// equals `expectedSha256`. A destination that fails verification is deleted.
[[nodiscard]] std::expected<void, std::string> copyVerified(const std::filesystem::path& from,
                                                            const std::filesystem::path& to,
                                                            const std::string& expectedSha256);

/// Can we create files in this directory right now?
[[nodiscard]] bool probeWritable(const std::filesystem::path& directory);

/// True while the file cannot be opened exclusively. A missing file counts as
/// unlocked; every other open failure counts as locked (fail safe: callers
/// wait rather than swap a file they cannot touch).
[[nodiscard]] bool isFileLocked(const std::filesystem::path& file);

/// Best-effort recursive delete of leftovers; failures are intentionally
/// ignored (the next run retries).
void removeTreeQuiet(const std::filesystem::path& directory);

}  // namespace updater::fsops
