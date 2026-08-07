#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace updater {

/// CLI contract (paths absolute; the client passes them):
///   updater.exe apply  --install-dir <dir> --staging-dir <dir> --parent-pid <pid>
///                      --relaunch-exe Calls.exe [--no-relaunch] [--log <file>]
///   updater.exe repair --install-dir <dir> [--relaunch-exe Calls.exe] [--log <file>]
struct Args {
    enum class Mode { Apply, Repair };
    Mode mode{Mode::Apply};
    std::filesystem::path installDir;
    std::filesystem::path stagingDir;
    std::uint32_t parentPid{0};
    std::string relaunchExe{"Calls.exe"};
    bool noRelaunch{false};
    std::filesystem::path logFile;   // empty: default %LOCALAPPDATA%\Calls\logs\updater.log
    std::filesystem::path stateDir;  // empty: default %LOCALAPPDATA%\Calls\updates.
                                     // Not a CLI flag — tests inject a temp dir here so
                                     // they never touch the real last-apply.json.

    [[nodiscard]] static std::expected<Args, std::string> parse(
        const std::vector<std::wstring>& argv);
};

}  // namespace updater
