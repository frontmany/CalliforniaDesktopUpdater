#include "apply.hpp"

#include "fsops.hpp"
#include "journal.hpp"
#include "log.hpp"
#include "manifest.hpp"
#include "sha256.hpp"
#include "verify.hpp"

#include <windows.h>

#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace updater {
namespace {

// Same-session named mutex; the client checks it at startup and exits while
// an apply is in flight. (Local\ rather than Global\ -- both processes run in
// the interactive session and Local\ needs no special privileges.)
constexpr wchar_t kApplyMutexName[] = L"Local\\CallsUpdaterApplying";
constexpr wchar_t kRunOnceKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
constexpr wchar_t kRunOnceValue[] = L"CallsUpdaterRepair";

/// Owns the single-instance apply mutex for the duration of a run.
class ApplyMutex {
public:
    ApplyMutex() : mHandle(CreateMutexW(nullptr, TRUE, kApplyMutexName)) {
        mOwned = mHandle != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
        if (!mOwned && mHandle != nullptr) {
            CloseHandle(mHandle);
            mHandle = nullptr;
        }
    }

    ~ApplyMutex() {
        if (mHandle != nullptr) {
            ReleaseMutex(mHandle);
            CloseHandle(mHandle);
        }
    }

    [[nodiscard]] bool owned() const { return mOwned; }

private:
    HANDLE mHandle{nullptr};
    bool mOwned{false};
};

std::optional<std::string> readFileBytes(const std::filesystem::path& file) {
    std::ifstream stream{file, std::ios::binary};
    if (!stream) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/// Converts a manifest's forward-slash relative path for std::filesystem.
std::filesystem::path relative(const std::string& manifestPath) {
    return std::filesystem::path{manifestPath, std::filesystem::path::generic_format};
}

bool waitForParentExit(const std::uint32_t pid) {
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) {
        return true;  // already gone (or never ours to see)
    }
    const auto result = WaitForSingleObject(process, 30'000);
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
}

bool waitForUnlock(const std::filesystem::path& file) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + seconds{30};
    while (steady_clock::now() < deadline) {
        if (!std::filesystem::exists(file) || !fsops::isFileLocked(file)) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds{500});
    }
    return false;
}

void setRunOnceRepair(const std::filesystem::path& installDir) {
    wchar_t selfPath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, selfPath, MAX_PATH) == 0) {
        return;
    }
    const std::wstring command =
        L"\"" + std::wstring{selfPath} + L"\" repair --install-dir \"" + installDir.wstring() + L"\"";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunOnceKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegSetValueExW(key, kRunOnceValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

void clearRunOnceRepair() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunOnceKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kRunOnceValue);
        RegCloseKey(key);
    }
}

void relaunch(const std::filesystem::path& installDir, const std::string& exeName) {
    const auto exe = installDir / relative(exeName);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                       CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, nullptr,
                       installDir.c_str(), &startup, &process) != 0) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        log::info("relaunched {}", exe.string());
    } else {
        log::error("failed to relaunch {} (error {})", exe.string(), GetLastError());
    }
}

void writeLastApply(const std::filesystem::path& stateDir,
                    const std::string& status,
                    const std::string& version,
                    const std::string& detail) {
    std::error_code ec;
    std::filesystem::create_directories(stateDir, ec);
    std::ofstream file{stateDir / L"last-apply.json", std::ios::binary | std::ios::trunc};
    if (file) {
        // Tiny fixed shape; hand-written to avoid quoting surprises.
        file << "{\"status\":\"" << status << "\",\"version\":\"" << version
             << "\",\"detail\":\"" << detail << "\"}";
    }
}

std::filesystem::path defaultStateDir() {
    wchar_t* localAppData = nullptr;
    std::filesystem::path base;
    if (_wdupenv_s(&localAppData, nullptr, L"LOCALAPPDATA") == 0 && localAppData != nullptr) {
        base = localAppData;
        free(localAppData);
    } else {
        base = std::filesystem::temp_directory_path();
    }
    return base / L"Calls" / L"updates";
}

/// Idempotent swap of one op: detectable-by-existence steps so `repair` can
/// re-run the whole list after a crash at any point.
std::expected<void, std::string> swapOne(const std::filesystem::path& installDir,
                                         const FileOp& op) {
    const auto target = installDir / relative(op.path);
    const auto incoming = installDir / L".update" / L"incoming" / relative(op.path);
    const auto backup = installDir / L".update" / L"backup" / relative(op.path);

    switch (op.kind) {
    case FileOp::Kind::Replace:
        if (std::filesystem::exists(incoming)) {
            if (std::filesystem::exists(target) && !std::filesystem::exists(backup)) {
                if (const auto moved = fsops::retryRename(target, backup); !moved) {
                    return moved;
                }
            }
            if (std::filesystem::exists(target)) {
                // Backup exists AND target exists: previous run crashed between
                // rename-to-backup of a directory-collision? Target must go
                // before incoming lands (target is stale only when backup done).
                if (const auto removed = fsops::retryDeleteFile(target); !removed) {
                    return removed;
                }
            }
            return fsops::retryRename(incoming, target);
        }
        return {};  // incoming consumed: both steps already done
    case FileOp::Kind::Add:
        if (std::filesystem::exists(incoming)) {
            if (std::filesystem::exists(target)) {
                // Unexpected local file in the way; keep a backup like Replace.
                if (!std::filesystem::exists(backup)) {
                    if (const auto moved = fsops::retryRename(target, backup); !moved) {
                        return moved;
                    }
                } else if (const auto removed = fsops::retryDeleteFile(target); !removed) {
                    return removed;
                }
            }
            return fsops::retryRename(incoming, target);
        }
        return {};
    case FileOp::Kind::Remove:
        if (std::filesystem::exists(target) && !std::filesystem::exists(backup)) {
            return fsops::retryRename(target, backup);
        }
        if (std::filesystem::exists(target)) {
            return fsops::retryDeleteFile(target);
        }
        return {};
    }
    return {};
}

/// Undo a partially performed swap (fresh apply only; repair rolls forward).
void rollbackSwap(const std::filesystem::path& installDir, const std::vector<FileOp>& ops) {
    for (const auto& op : ops) {
        const auto target = installDir / relative(op.path);
        const auto incoming = installDir / L".update" / L"incoming" / relative(op.path);
        const auto backup = installDir / L".update" / L"backup" / relative(op.path);

        // Put the new file back into incoming (if it landed), restore backup.
        if (op.kind != FileOp::Kind::Remove && !std::filesystem::exists(incoming) &&
            std::filesystem::exists(target)) {
            if (const auto moved = fsops::retryRename(target, incoming); !moved) {
                log::error("rollback: {}", moved.error());
            }
        }
        if (std::filesystem::exists(backup)) {
            if (std::filesystem::exists(target)) {
                (void)fsops::retryDeleteFile(target);
            }
            if (const auto moved = fsops::retryRename(backup, target); !moved) {
                log::error("rollback: {}", moved.error());
            }
        }
    }
}

/// After a successful swap: install the new manifest, drop work dirs + journal.
std::expected<void, std::string> commit(const std::filesystem::path& installDir) {
    const auto stagedManifest = installDir / L".update" / L"manifest.json";
    const auto liveManifest = installDir / L"manifest.json";
    if (std::filesystem::exists(stagedManifest)) {
        if (std::filesystem::exists(liveManifest)) {
            if (const auto removed = fsops::retryDeleteFile(liveManifest); !removed) {
                return removed;
            }
        }
        if (const auto moved = fsops::retryRename(stagedManifest, liveManifest); !moved) {
            return moved;
        }
    }
    Journal::remove(installDir);
    fsops::removeTreeQuiet(installDir / L".update");
    clearRunOnceRepair();
    return {};
}

}  // namespace

int runApply(const Args& args) {
    // ---- INIT ----
    const ApplyMutex mutex;
    if (!mutex.owned()) {
        log::error("another updater instance is applying; exiting");
        return kBadArgs;
    }

    const auto stateDir = defaultStateDir();
    log::info("apply: install={} staging={} pid={}", args.installDir.string(),
              args.stagingDir.string(), args.parentPid);

    // ---- WAIT_PARENT ----
    if (!waitForParentExit(args.parentPid) ||
        !waitForUnlock(args.installDir / relative(args.relaunchExe))) {
        log::error("parent process still running / exe locked; aborting");
        writeLastApply(stateDir, "error", "", "app did not exit");
        return kParentAlive;
    }

    // ---- PREFLIGHT ----
    if (!fsops::probeWritable(args.installDir)) {
        log::error("install dir is not writable: {}", args.installDir.string());
        writeLastApply(stateDir, "error", "", "install dir not writable");
        if (!args.noRelaunch) {
            relaunch(args.installDir, args.relaunchExe);
        }
        return kNotWritable;
    }

    const auto fail = [&](const int code, const std::string& detail) {
        log::error("apply failed: {}", detail);
        writeLastApply(stateDir, "error", "", detail);
        if (!args.noRelaunch) {
            relaunch(args.installDir, args.relaunchExe);
        }
        return code;
    };

    const auto manifestBytes = readFileBytes(args.stagingDir / L"manifest.json");
    if (!manifestBytes) {
        return fail(kVerifyFailed, "staging manifest.json missing");
    }
    const auto signatureB64 = readFileBytes(args.stagingDir / L"manifest.sig");
    std::string signature;
    if (!signatureB64 || !base64Decode(*signatureB64, signature) ||
        !verifyManifestSignature(*manifestBytes, signature)) {
        return fail(kVerifyFailed, "manifest signature verification failed");
    }
    const auto newManifest = Manifest::parse(*manifestBytes);
    if (!newManifest) {
        return fail(kVerifyFailed, "manifest invalid: " + newManifest.error());
    }

    std::vector<FileOp> ops;
    const auto oldManifestBytes = readFileBytes(args.installDir / L"manifest.json");
    if (oldManifestBytes) {
        const auto oldManifest = Manifest::parse(*oldManifestBytes);
        if (oldManifest) {
            ops = diffManifests(*oldManifest, *newManifest);
        } else {
            log::warn("local manifest unreadable ({}); full reinstall", oldManifest.error());
            ops = fullInstallOps(*newManifest);
        }
    } else {
        log::info("no local manifest; treating as fresh install");
        ops = fullInstallOps(*newManifest);
    }
    if (ops.empty()) {
        log::info("nothing to do (already at {})", newManifest->version);
        writeLastApply(stateDir, "ok", newManifest->version, "already up to date");
        if (!args.noRelaunch) {
            relaunch(args.installDir, args.relaunchExe);
        }
        return kOk;
    }
    log::info("update to {}: {} ops", newManifest->version, ops.size());

    // Verify every staged file against the manifest before touching anything.
    for (const auto& op : ops) {
        if (op.kind == FileOp::Kind::Remove) {
            continue;
        }
        const auto staged = args.stagingDir / relative(op.path);
        const auto hash = sha256File(staged);
        if (!hash || *hash != op.sha256) {
            return fail(kVerifyFailed, "staged file bad or missing: " + op.path);
        }
    }

    // ---- STAGE_LOCAL ---- (cross-volume copy; swap below is same-volume renames)
    for (const auto& op : ops) {
        if (op.kind == FileOp::Kind::Remove) {
            continue;
        }
        const auto staged = args.stagingDir / relative(op.path);
        const auto incoming = args.installDir / L".update" / L"incoming" / relative(op.path);
        if (const auto copied = fsops::copyVerified(staged, incoming, op.sha256); !copied) {
            fsops::removeTreeQuiet(args.installDir / L".update");
            return fail(kVerifyFailed, "staging to install volume failed: " + copied.error());
        }
    }
    {
        std::ofstream manifestOut{args.installDir / L".update" / L"manifest.json",
                                  std::ios::binary | std::ios::trunc};
        manifestOut.write(manifestBytes->data(), static_cast<std::streamsize>(manifestBytes->size()));
        if (!manifestOut.good()) {
            fsops::removeTreeQuiet(args.installDir / L".update");
            return fail(kNotWritable, "cannot write staged manifest");
        }
    }

    Journal journal{.state = "staged",
                    .version = newManifest->version,
                    .relaunchExe = args.relaunchExe,
                    .ops = ops};
    if (const auto saved = journal.save(args.installDir); !saved) {
        fsops::removeTreeQuiet(args.installDir / L".update");
        return fail(kNotWritable, saved.error());
    }
    setRunOnceRepair(args.installDir);  // survives even a reboot mid-swap

    // ---- SWAP ----
    journal.state = "swapping";
    if (const auto saved = journal.save(args.installDir); !saved) {
        Journal::remove(args.installDir);
        fsops::removeTreeQuiet(args.installDir / L".update");
        clearRunOnceRepair();
        return fail(kNotWritable, saved.error());
    }

    for (std::size_t index = 0; index < ops.size(); ++index) {
        if (const auto swapped = swapOne(args.installDir, ops[index]); !swapped) {
            log::error("swap failed at {}: {}", ops[index].path, swapped.error());
            rollbackSwap(args.installDir, ops);
            // Verify the rollback actually restored the old executable set.
            const auto exe = args.installDir / relative(args.relaunchExe);
            if (std::filesystem::exists(exe)) {
                Journal::remove(args.installDir);
                fsops::removeTreeQuiet(args.installDir / L".update");
                clearRunOnceRepair();
                writeLastApply(stateDir, "error", newManifest->version,
                               "swap failed, rolled back: " + ops[index].path);
                if (!args.noRelaunch) {
                    relaunch(args.installDir, args.relaunchExe);
                }
                return kSwapFailedRolledBack;
            }
            log::error("rollback incomplete; journal left for repair");
            writeLastApply(stateDir, "broken", newManifest->version, "swap and rollback failed");
            return kSwapFailedBroken;
        }
    }

    // ---- COMMIT ----
    if (const auto committed = commit(args.installDir); !committed) {
        // Files are fully swapped; only bookkeeping failed. Repair finishes it.
        log::error("commit incomplete: {}", committed.error());
        writeLastApply(stateDir, "broken", newManifest->version, committed.error());
        return kSwapFailedBroken;
    }
    fsops::removeTreeQuiet(args.stagingDir);
    writeLastApply(stateDir, "ok", newManifest->version, "");
    log::info("update to {} applied", newManifest->version);

    // ---- RELAUNCH ----
    if (!args.noRelaunch) {
        relaunch(args.installDir, args.relaunchExe);
    }
    return kOk;
}

int runRepair(const Args& args) {
    const ApplyMutex mutex;
    if (!mutex.owned()) {
        return kBadArgs;
    }

    const auto stateDir = defaultStateDir();
    const auto journal = Journal::load(args.installDir);
    if (!journal) {
        // No (readable) journal: nothing was mid-swap. Drop leftovers if any.
        if (std::filesystem::exists(args.installDir / L".update")) {
            log::info("repair: no journal; cleaning leftovers");
            fsops::removeTreeQuiet(args.installDir / L".update");
            clearRunOnceRepair();
        }
        return kNothingToDo;
    }

    log::info("repair: journal state={} version={}", journal->state, journal->version);
    if (journal->state == "staged") {
        // Swap never started -- everything valuable is still in place.
        Journal::remove(args.installDir);
        fsops::removeTreeQuiet(args.installDir / L".update");
        clearRunOnceRepair();
        log::info("repair: discarded staged update (client will re-check)");
        return kOk;
    }

    // state == "swapping": roll forward. Incoming files were hash-verified
    // before the swap began; each op is idempotent.
    if (!waitForUnlock(args.installDir / relative(journal->relaunchExe))) {
        log::error("repair: install exe still locked");
        return kParentAlive;
    }
    for (const auto& op : journal->ops) {
        if (const auto swapped = swapOne(args.installDir, op); !swapped) {
            log::error("repair: swap still failing at {}: {}", op.path, swapped.error());
            writeLastApply(stateDir, "broken", journal->version, swapped.error());
            return kSwapFailedBroken;
        }
    }
    if (const auto committed = commit(args.installDir); !committed) {
        log::error("repair: commit failed: {}", committed.error());
        writeLastApply(stateDir, "broken", journal->version, committed.error());
        return kSwapFailedBroken;
    }
    writeLastApply(stateDir, "ok", journal->version, "repaired");
    log::info("repair: rolled forward to {}", journal->version);
    if (!args.noRelaunch) {
        relaunch(args.installDir, journal->relaunchExe);
    }
    return kOk;
}

}
