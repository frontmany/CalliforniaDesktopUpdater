#include "args.hpp"

#include <windows.h>

#include <charconv>
#include <string>

namespace updater {
namespace {

/// UTF-8 conversion — relaunchExe is real data compared against manifest
/// paths (which are UTF-8 from JSON), so lossy ASCII filtering would break
/// any non-ASCII executable name.
std::string narrow(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

}  // namespace

std::expected<Args, std::string> Args::parse(const std::vector<std::wstring>& argv) {
    if (argv.size() < 2) {
        return std::unexpected("usage: updater.exe apply|repair --install-dir <dir> ...");
    }

    Args args;
    if (argv[1] == L"apply") {
        args.mode = Mode::Apply;
    } else if (argv[1] == L"repair") {
        args.mode = Mode::Repair;
    } else {
        return std::unexpected("unknown mode: " + narrow(argv[1]));
    }

    for (std::size_t index = 2; index < argv.size(); ++index) {
        const auto& flag = argv[index];
        if (flag == L"--no-relaunch") {
            args.noRelaunch = true;
            continue;
        }
        const bool takesValue = flag == L"--install-dir" || flag == L"--staging-dir" ||
                                flag == L"--parent-pid" || flag == L"--relaunch-exe" ||
                                flag == L"--log";
        if (!takesValue) {
            return std::unexpected("unknown flag: " + narrow(flag));
        }
        if (index + 1 >= argv.size()) {
            return std::unexpected(narrow(flag) + " requires a value");
        }
        const auto& value = argv[++index];

        if (flag == L"--install-dir") {
            args.installDir = value;
        } else if (flag == L"--staging-dir") {
            args.stagingDir = value;
        } else if (flag == L"--parent-pid") {
            const auto text = narrow(value);
            const auto [ptr, ec] =
                std::from_chars(text.data(), text.data() + text.size(), args.parentPid);
            if (ec != std::errc{} || ptr != text.data() + text.size()) {
                return std::unexpected("--parent-pid must be a number");
            }
        } else if (flag == L"--relaunch-exe") {
            args.relaunchExe = narrow(value);
        } else {  // --log
            args.logFile = value;
        }
    }

    if (args.installDir.empty() || !args.installDir.is_absolute()) {
        return std::unexpected("--install-dir must be an absolute path");
    }
    if (args.mode == Mode::Apply) {
        if (args.stagingDir.empty() || !args.stagingDir.is_absolute()) {
            return std::unexpected("--staging-dir must be an absolute path");
        }
        if (args.parentPid == 0) {
            return std::unexpected("--parent-pid is required for apply");
        }
    }
    // Must be exactly a file name: no separators, no drive ("C:x.exe" would
    // escape the install dir via operator/), no "." / ".." (".." always
    // exists, defeating the post-rollback health check).
    if (args.relaunchExe.empty() ||
        args.relaunchExe.find_first_of("/\\:") != std::string::npos ||
        args.relaunchExe == "." || args.relaunchExe == "..") {
        return std::unexpected("--relaunch-exe must be a bare file name");
    }
    return args;
}

}  // namespace updater
