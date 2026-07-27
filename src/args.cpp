#include "args.hpp"

#include <charconv>
#include <string>

namespace updater {
namespace {

std::string narrow(const std::wstring& wide) {
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t character : wide) {
        out.push_back(character < 128 ? static_cast<char>(character) : '?');
    }
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
        const auto next = [&]() -> std::expected<std::wstring, std::string> {
            if (index + 1 >= argv.size()) {
                return std::unexpected(narrow(flag) + " requires a value");
            }
            return argv[++index];
        };

        if (flag == L"--install-dir") {
            const auto value = next();
            if (!value) {
                return std::unexpected(value.error());
            }
            args.installDir = *value;
        } else if (flag == L"--staging-dir") {
            const auto value = next();
            if (!value) {
                return std::unexpected(value.error());
            }
            args.stagingDir = *value;
        } else if (flag == L"--parent-pid") {
            const auto value = next();
            if (!value) {
                return std::unexpected(value.error());
            }
            const auto text = narrow(*value);
            const auto [ptr, ec] =
                std::from_chars(text.data(), text.data() + text.size(), args.parentPid);
            if (ec != std::errc{} || ptr != text.data() + text.size()) {
                return std::unexpected("--parent-pid must be a number");
            }
        } else if (flag == L"--relaunch-exe") {
            const auto value = next();
            if (!value) {
                return std::unexpected(value.error());
            }
            args.relaunchExe = narrow(*value);
        } else if (flag == L"--no-relaunch") {
            args.noRelaunch = true;
        } else if (flag == L"--log") {
            const auto value = next();
            if (!value) {
                return std::unexpected(value.error());
            }
            args.logFile = *value;
        } else {
            return std::unexpected("unknown flag: " + narrow(flag));
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
    if (args.relaunchExe.empty() || args.relaunchExe.find_first_of("/\\") != std::string::npos) {
        return std::unexpected("--relaunch-exe must be a bare file name");
    }
    return args;
}

}
