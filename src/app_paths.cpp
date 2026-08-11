#include "app_paths.hpp"

#include <cstdlib>

namespace updater {

std::filesystem::path localAppDataBase() {
    wchar_t* localAppData = nullptr;
    std::filesystem::path base;
    if (_wdupenv_s(&localAppData, nullptr, L"LOCALAPPDATA") == 0 && localAppData != nullptr) {
        base = localAppData;
        free(localAppData);
    } else {
        base = std::filesystem::temp_directory_path();
    }
    return base;
}

std::filesystem::path defaultStateDir() {
    return localAppDataBase() / L"Callifornia" / L"updates";
}

std::filesystem::path defaultLogFile() {
    return localAppDataBase() / L"Callifornia" / L"logs" / L"updater.log";
}

}  // namespace updater
