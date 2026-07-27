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
    [[nodiscard]] static std::optional<Journal> load(const std::filesystem::path& installDir);

    /// Atomic write (temp + rename over).
    [[nodiscard]] std::expected<void, std::string> save(
        const std::filesystem::path& installDir) const;

    static void remove(const std::filesystem::path& installDir);
};

}
