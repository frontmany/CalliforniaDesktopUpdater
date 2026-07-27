#include "apply.hpp"
#include "args.hpp"
#include "log.hpp"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path defaultLogFile() {
    wchar_t* localAppData = nullptr;
    std::filesystem::path base;
    if (_wdupenv_s(&localAppData, nullptr, L"LOCALAPPDATA") == 0 && localAppData != nullptr) {
        base = localAppData;
        free(localAppData);
    } else {
        base = std::filesystem::temp_directory_path();
    }
    return base / L"Calls" / L"logs" / L"updater.log";
}

}  // namespace

// WIN32 subsystem (no console window); args come from the raw command line.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argvRaw = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> argv;
    argv.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        argv.emplace_back(argvRaw[i]);
    }
    LocalFree(argvRaw);

    const auto args = updater::Args::parse(argv);
    if (!args) {
        updater::log::init(defaultLogFile());
        updater::log::error("bad arguments: {}", args.error());
        return updater::kBadArgs;
    }

    updater::log::init(args->logFile.empty() ? defaultLogFile() : args->logFile);

    const int code = args->mode == updater::Args::Mode::Apply ? updater::runApply(*args)
                                                              : updater::runRepair(*args);
    updater::log::info("exit code {}", code);
    updater::log::flushAll();
    return code;
}
