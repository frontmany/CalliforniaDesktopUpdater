#include "lock_owners.hpp"

#include <windows.h>
#include <restartmanager.h>

#include <array>
#include <string>
#include <vector>

namespace updater {
namespace {

// The Restart Manager caps a query at this many affected applications; more
// than a handful would be noise in a log line anyway.
constexpr UINT kMaxApps = 32;

std::string narrow(const wchar_t* wide) {
    if (wide == nullptr || wide[0] == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size - 1, nullptr, nullptr);
    return out;
}

class RestartManagerSession {
public:
    RestartManagerSession() {
        mOpened = RmStartSession(&mHandle, 0, mKey.data()) == ERROR_SUCCESS;
    }

    RestartManagerSession(const RestartManagerSession&) = delete;
    RestartManagerSession& operator=(const RestartManagerSession&) = delete;

    ~RestartManagerSession() {
        if (mOpened) {
            RmEndSession(mHandle);
        }
    }

    [[nodiscard]] bool opened() const { return mOpened; }
    [[nodiscard]] DWORD handle() const { return mHandle; }

private:
    DWORD mHandle{0};
    std::array<WCHAR, CCH_RM_SESSION_KEY + 1> mKey{};
    bool mOpened{false};
};

}  // namespace

std::string lockOwners(const std::vector<std::filesystem::path>& files) {
    if (files.empty()) {
        return {};
    }
    const RestartManagerSession session;
    if (!session.opened()) {
        return {};
    }

    std::vector<std::wstring> paths;
    std::vector<LPCWSTR> pathPointers;
    paths.reserve(files.size());
    pathPointers.reserve(files.size());
    for (const auto& file : files) {
        paths.push_back(file.wstring());
        pathPointers.push_back(paths.back().c_str());
    }
    if (RmRegisterResources(session.handle(), static_cast<UINT>(pathPointers.size()),
                            pathPointers.data(), 0, nullptr, 0, nullptr) != ERROR_SUCCESS) {
        return {};
    }

    UINT needed = 0;
    UINT count = kMaxApps;
    std::vector<RM_PROCESS_INFO> apps(kMaxApps);
    DWORD reason = 0;
    if (RmGetList(session.handle(), &needed, &count, apps.data(), &reason) != ERROR_SUCCESS) {
        // ERROR_MORE_DATA included: a truncated answer is still an answer, but
        // the buffer is not filled in that case, so there is nothing to report.
        return {};
    }

    std::string out;
    for (UINT index = 0; index < count && index < kMaxApps; ++index) {
        const auto name = narrow(apps[index].strAppName);
        if (name.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += name + " (pid " + std::to_string(apps[index].Process.dwProcessId) + ")";
    }
    return out;
}

}  // namespace updater
