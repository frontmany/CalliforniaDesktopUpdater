#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace updater {

/// Names the processes currently holding `files` open, e.g.
/// "Callifornia.exe (pid 4812), MsMpEng.exe (pid 3120)". Empty when nothing
/// holds them or the answer cannot be obtained.
///
/// This is the one question a failed swap always raises and never used to
/// answer. Without it, "rename failed (error 32)" in the log names the file and
/// stops there, and the difference between an antivirus mid-scan, a straggling
/// child process and a second copy of the client the user forgot about is
/// exactly the difference between the three fixes. Uses the Restart Manager
/// (rstrtmgr.dll, a system component -- never a file this binary could be asked
/// to replace, so the no-DLL-dependencies rule is not at stake).
[[nodiscard]] std::string lockOwners(const std::vector<std::filesystem::path>& files);

}  // namespace updater
