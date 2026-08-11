#pragma once

#include <filesystem>

namespace updater {

/// %LOCALAPPDATA%, falling back to the temp directory when unset.
[[nodiscard]] std::filesystem::path localAppDataBase();

/// %LOCALAPPDATA%\Callifornia\updates — where last-apply.json lives.
[[nodiscard]] std::filesystem::path defaultStateDir();

/// %LOCALAPPDATA%\Callifornia\logs\updater.log
[[nodiscard]] std::filesystem::path defaultLogFile();

}  // namespace updater
