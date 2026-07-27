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

/// Delete a file with the same retry policy. Missing file is success.
[[nodiscard]] std::expected<void, std::string> retryDeleteFile(const std::filesystem::path& file);

/// Copy `from` to `to` (creating parent dirs), then verify the copy's SHA-256
/// equals `expectedSha256`. Used to move staged files onto the install volume.
[[nodiscard]] std::expected<void, std::string> copyVerified(const std::filesystem::path& from,
                                                            const std::filesystem::path& to,
                                                            const std::string& expectedSha256);

/// Can we create files in this directory right now?
[[nodiscard]] bool probeWritable(const std::filesystem::path& directory);

/// True while some process holds the file open (probe: exclusive open).
[[nodiscard]] bool isFileLocked(const std::filesystem::path& file);

/// Best-effort recursive delete; returns false if anything survived.
bool removeTreeQuiet(const std::filesystem::path& directory);

}
