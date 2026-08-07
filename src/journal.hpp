#pragma once

#include "manifest.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace updater {

/// Crash-recovery journal at <install>\.update\journal.json. Written before
/// the swap starts; deleted on commit or clean rollback. Its presence means
/// an apply did not finish — `repair` reads it and rolls forward.
struct Journal {
    // "staged": incoming files copied, swap not started (safe to discard).
    // "swapping": renames may have happened (roll forward idempotently).
    std::string state;
    std::string version;
    std::string relaunchExe;
    std::vector<FileOp> ops;

    [[nodiscard]] static std::filesystem::path pathIn(const std::filesystem::path& installDir);

    /// nullopt: no journal on disk. unexpected: a journal exists but cannot be
    /// parsed — callers must NOT treat that as "nothing was mid-swap", the
    /// .update tree may hold the only copies of displaced files.
    [[nodiscard]] static std::expected<std::optional<Journal>, std::string> load(
        const std::filesystem::path& installDir);

    /// Atomic write (temp + MOVEFILE_REPLACE_EXISTING over the target).
    [[nodiscard]] std::expected<void, std::string> save(
        const std::filesystem::path& installDir) const;

    /// True when the journal file is gone afterwards.
    static bool remove(const std::filesystem::path& installDir);
};

}  // namespace updater
