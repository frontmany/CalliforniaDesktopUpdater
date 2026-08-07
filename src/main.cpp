#include "app_paths.hpp"
#include "apply.hpp"
#include "args.hpp"
#include "log.hpp"
#include "win_raii.hpp"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

/// Best-effort --log extraction for the parse-failure path, so a caller that
/// directed logs to a file can still see why its invocation was rejected.
std::filesystem::path requestedLogFile(const std::vector<std::wstring>& argv) {
    for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == L"--log") {
            return argv[i + 1];
        }
    }
    return {};
}

}  // namespace

// WIN32 subsystem (no console window); args come from the raw command line.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argvRaw = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argvRaw == nullptr) {
        return updater::kBadArgs;  // cannot even log: no args, no --log value
    }
    const updater::UniqueLocalMemory argvOwner{argvRaw};
    std::vector<std::wstring> argv;
    argv.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        argv.emplace_back(argvRaw[i]);
    }

    const auto args = updater::Args::parse(argv);
    if (!args) {
        const auto logFile = requestedLogFile(argv);
        updater::log::init(logFile.empty() ? updater::defaultLogFile() : logFile);
        updater::log::error("bad arguments: {}", args.error());
        return updater::kBadArgs;
    }

    updater::log::init(args->logFile.empty() ? updater::defaultLogFile() : args->logFile);

    const int code = args->mode == updater::Args::Mode::Apply ? updater::runApply(*args)
                                                              : updater::runRepair(*args);
    updater::log::info("exit code {}", code);
    updater::log::flushAll();
    return code;
}
