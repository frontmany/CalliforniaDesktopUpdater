#pragma once

#include <filesystem>

namespace updater {

/// %LOCALAPPDATA%, falling back to the temp directory when unset.
[[nodiscard]] std::filesystem::path localAppDataBase();

/// %LOCALAPPDATA%\Calls\updates — where last-apply.json lives.
[[nodiscard]] std::filesystem::path defaultStateDir();

/// %LOCALAPPDATA%\Calls\logs\updater.log
[[nodiscard]] std::filesystem::path defaultLogFile();

}  // namespace updater
