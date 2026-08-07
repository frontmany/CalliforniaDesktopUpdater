#include "apply.hpp"

#include "app_paths.hpp"
#include "fsops.hpp"
#include "journal.hpp"
#include "log.hpp"
#include "manifest.hpp"
#include "public_key.hpp"
#include "sha256.hpp"
#include "verify.hpp"
#include "win_raii.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <expected>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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

    // Copying would release/close the mutex twice.
    ApplyMutex(const ApplyMutex&) = delete;
    ApplyMutex& operator=(const ApplyMutex&) = delete;

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

[[nodiscard]] std::expected<std::string, std::string> readFileBytes(
    const std::filesystem::path& file) {
    std::ifstream stream{file, std::ios::binary};
    if (!stream) {
        return std::unexpected("cannot open " + file.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (stream.bad()) {
        return std::unexpected("read error on " + file.string());
    }
    return buffer.str();
}

/// Converts a manifest's forward-slash relative path for std::filesystem.
std::filesystem::path relative(const std::string& manifestPath) {
    return std::filesystem::path{manifestPath, std::filesystem::path::generic_format};
}

[[nodiscard]] bool waitForParentExit(const std::uint32_t pid) {
    const UniqueHandle process{OpenProcess(SYNCHRONIZE, FALSE, pid)};
    if (!process.valid()) {
        const auto error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            return true;  // pid no longer exists — the parent already exited
        }
        // ERROR_ACCESS_DENIED and friends: the pid exists but is not ours to
        // watch (recycled onto another user's process, elevation). Fail safe:
        // report "still alive" and let the client retry on its next launch
        // rather than swap files under a process we cannot observe.
        log::error("cannot watch parent pid {} (error {}); aborting", pid, error);
        return false;
    }
    return WaitForSingleObject(process.get(), 30'000) == WAIT_OBJECT_0;
}

[[nodiscard]] bool waitForUnlock(const std::filesystem::path& file) {
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

bool setRunOnceRepair(const std::filesystem::path& installDir) {
    // GetModuleFileNameW truncates silently at the buffer size; a generous
    // fixed buffer covers long install paths (\\?\ limit is 32767).
    std::wstring selfPath(32'768, L'\0');
    const auto length =
        GetModuleFileNameW(nullptr, selfPath.data(), static_cast<DWORD>(selfPath.size()));
    if (length == 0 || length >= selfPath.size()) {
        log::error("GetModuleFileNameW failed (error {}); RunOnce repair not registered",
                   GetLastError());
        return false;
    }
    selfPath.resize(length);
    const std::wstring command =
        L"\"" + selfPath + L"\" repair --install-dir \"" + installDir.wstring() + L"\"";
    UniqueRegKey key;
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, kRunOnceKey, 0, KEY_SET_VALUE,
                                      key.receive());
    if (opened != ERROR_SUCCESS) {
        log::error("RunOnce key open failed ({}); repair not registered", opened);
        return false;
    }
    const auto set = RegSetValueExW(key.get(), kRunOnceValue, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(command.c_str()),
                                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (set != ERROR_SUCCESS) {
        log::error("RunOnce value write failed ({}); repair not registered", set);
        return false;
    }
    return true;
}

void clearRunOnceRepair() {
    UniqueRegKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunOnceKey, 0, KEY_SET_VALUE, key.receive()) !=
        ERROR_SUCCESS) {
        return;
    }
    const auto removed = RegDeleteValueW(key.get(), kRunOnceValue);
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
        log::warn("RunOnce value delete failed ({}); repair may re-run at logon", removed);
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
        const UniqueHandle thread{process.hThread};
        const UniqueHandle processHandle{process.hProcess};
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

std::filesystem::path stateDirFor(const Args& args) {
    return args.stateDir.empty() ? defaultStateDir() : args.stateDir;
}

/// Drop journal + work dirs + the RunOnce net after an update is committed,
/// cleanly rolled back, or discarded. Failures are logged; a surviving
/// journal only means repair re-runs an idempotent (or empty) op list.
void discardUpdateState(const std::filesystem::path& installDir) {
    if (!Journal::remove(installDir)) {
        log::warn("journal removal failed; repair may re-run once");
    }
    fsops::removeTreeQuiet(installDir / L".update");
    clearRunOnceRepair();
}

[[nodiscard]] bool dirHasEntries(const std::filesystem::path& directory) {
    std::error_code ec;
    return std::filesystem::exists(directory, ec) && !std::filesystem::is_empty(directory, ec);
}

/// Idempotent swap of one op: detectable-by-existence steps so `repair` can
/// re-run the whole list after a crash at any point.
[[nodiscard]] std::expected<void, std::string> swapOne(const std::filesystem::path& installDir,
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
                // A backup already exists, so the old file is safe. Whatever
                // sits at target now (recreated by the app, or debris from an
                // interrupted rollback) is disposable — clear it so incoming
                // can land.
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
    std::unreachable();
}

/// Undo a partially performed swap (fresh apply only; repair rolls forward).
/// Only the first `opsApplied` entries can have been touched. Returns false
/// when any restore step failed — the caller must NOT discard .update then.
[[nodiscard]] bool rollbackSwap(const std::filesystem::path& installDir,
                                const std::vector<FileOp>& ops,
                                const std::size_t opsApplied) {
    bool allRestored = true;
    for (std::size_t index = 0; index < opsApplied && index < ops.size(); ++index) {
        const auto& op = ops[index];
        const auto target = installDir / relative(op.path);
        const auto incoming = installDir / L".update" / L"incoming" / relative(op.path);
        const auto backup = installDir / L".update" / L"backup" / relative(op.path);

        // Put the new file back into incoming (if it landed), restore backup.
        if (op.kind != FileOp::Kind::Remove && !std::filesystem::exists(incoming) &&
            std::filesystem::exists(target)) {
            if (const auto moved = fsops::retryRename(target, incoming); !moved) {
                log::error("rollback: {}", moved.error());
                allRestored = false;
            }
        }
        if (std::filesystem::exists(backup)) {
            if (std::filesystem::exists(target)) {
                if (const auto removed = fsops::retryDeleteFile(target); !removed) {
                    log::error("rollback: {}", removed.error());
                    allRestored = false;
                }
            }
            if (const auto moved = fsops::retryRename(backup, target); !moved) {
                log::error("rollback: {}", moved.error());
                allRestored = false;
            }
        }
    }
    return allRestored;
}

/// After a successful swap: install the new manifest, drop work dirs + journal.
[[nodiscard]] std::expected<void, std::string> commit(const std::filesystem::path& installDir) {
    const auto stagedManifest = installDir / L".update" / L"manifest.json";
    const auto liveManifest = installDir / L"manifest.json";
    if (std::filesystem::exists(stagedManifest)) {
        // Atomic replace: the install never lacks a manifest.json.
        if (const auto moved = fsops::retryReplaceFile(stagedManifest, liveManifest); !moved) {
            return moved;
        }
    }
    if (!Journal::remove(installDir)) {
        // A surviving "swapping" journal makes the next start re-run the ops
        // (harmless but noisy) — surface it instead of pretending success.
        return std::unexpected("journal removal failed");
    }
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

    const auto stateDir = stateDirFor(args);
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

    std::string manifestVersion;  // filled once the manifest parses; keeps
                                  // last-apply.json attributable to a version
    const auto fail = [&](const int code, const std::string& detail) {
        log::error("apply failed: {}", detail);
        writeLastApply(stateDir, "error", manifestVersion, detail);
        if (!args.noRelaunch) {
            relaunch(args.installDir, args.relaunchExe);
        }
        return code;
    };

    const auto manifestBytes = readFileBytes(args.stagingDir / L"manifest.json");
    if (!manifestBytes) {
        return fail(kVerifyFailed, "staging manifest.json missing: " + manifestBytes.error());
    }
    const auto signatureB64 = readFileBytes(args.stagingDir / L"manifest.sig");
    if (!signatureB64) {
        return fail(kVerifyFailed, "staging manifest.sig missing: " + signatureB64.error());
    }
    const auto signature = base64Decode(*signatureB64);
    if (!signature) {
        return fail(kVerifyFailed, "manifest.sig malformed: " + signature.error());
    }
    if (!verifyManifestSignature(*manifestBytes, *signature)) {
        return fail(kVerifyFailed, "manifest signature verification failed");
    }
    const auto newManifest = Manifest::parse(*manifestBytes);
    if (!newManifest) {
        return fail(kVerifyFailed, "manifest invalid: " + newManifest.error());
    }
    if (newManifest->keyId != kUpdateKeyId) {
        // The embedded key just verified the signature, so a foreign key_id is
        // a publisher-side mix-up worth naming rather than a security event.
        return fail(kVerifyFailed, "manifest key_id mismatch: " + newManifest->keyId);
    }
    manifestVersion = newManifest->version;

    // The relaunch target must survive the update: a manifest that does not
    // ship it would leave an install nobody can start again. Deliberately
    // checked HERE against args.relaunchExe, not inside Manifest::parse --
    // which exe matters is a property of this invocation, not of the manifest
    // format. Parse used to hardcode the literal "Client.exe", and because
    // that check lived in already-shipped binaries, renaming the exe made
    // every installed client reject every future manifest with no way to fix
    // it after the fact. Never put a file name back into parse.
    // (That the manifest also ships updater.exe is enforced at publish time by
    // the Updates service, which is server-side and can be changed by deploy.)
    const auto samePathIgnoreCase = [](std::string_view left, std::string_view right) {
        return std::ranges::equal(left, right, [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    };
    if (std::ranges::none_of(newManifest->files, [&](const ManifestFile& file) {
            return samePathIgnoreCase(file.path, args.relaunchExe);
        })) {
        return fail(kVerifyFailed, args.relaunchExe + " is missing from the manifest");
    }

    std::vector<FileOp> ops;
    const auto oldManifestBytes = readFileBytes(args.installDir / L"manifest.json");
    if (oldManifestBytes) {
        const auto oldManifest = Manifest::parse(*oldManifestBytes);
        if (oldManifest) {
            ops = diffManifests(*oldManifest, *newManifest);
        } else {
            log::warn("local manifest unreadable ({}); full reinstall — files dropped from the "
                      "new release may be left behind",
                      oldManifest.error());
            ops = fullInstallOps(*newManifest);
        }
    } else {
        log::info("no local manifest; treating as fresh install");
        ops = fullInstallOps(*newManifest);
    }
    if (ops.empty()) {
        // No file changes, but the manifest itself may differ (e.g. a release
        // that only raises min_supported_version) — commit it so the client
        // stops seeing the update as pending.
        log::info("no file ops; committing manifest {} only", newManifest->version);
        const auto temp = args.installDir / L"manifest.json.tmp";
        {
            std::ofstream manifestOut{temp, std::ios::binary | std::ios::trunc};
            manifestOut.write(manifestBytes->data(),
                              static_cast<std::streamsize>(manifestBytes->size()));
            manifestOut.close();
            if (!manifestOut.good()) {
                (void)fsops::retryDeleteFile(temp);
                return fail(kNotWritable, "cannot write manifest.json.tmp");
            }
        }
        // Atomic replace — a delete-then-rename pair could crash into a state
        // with no manifest.json at all, forcing a full reinstall next time.
        if (const auto replaced =
                fsops::retryReplaceFile(temp, args.installDir / L"manifest.json");
            !replaced) {
            (void)fsops::retryDeleteFile(temp);
            return fail(kNotWritable, "manifest commit failed: " + replaced.error());
        }
        fsops::removeTreeQuiet(args.stagingDir);
        writeLastApply(stateDir, "ok", newManifest->version, "manifest only");
        if (!args.noRelaunch) {
            relaunch(args.installDir, args.relaunchExe);
        }
        return kOk;
    }
    log::info("update to {}: {} ops", newManifest->version, ops.size());

    // Cheap existence pass; content is verified against the manifest hash by
    // copyVerified while staging below, still before any swap begins.
    for (const auto& op : ops) {
        if (op.kind == FileOp::Kind::Remove) {
            continue;
        }
        if (!std::filesystem::exists(args.stagingDir / relative(op.path))) {
            return fail(kVerifyFailed, "staged file missing: " + op.path);
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
        manifestOut.write(manifestBytes->data(),
                          static_cast<std::streamsize>(manifestBytes->size()));
        manifestOut.close();  // flush BEFORE checking; a truncated staged
                              // manifest would poison the next update's diff
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
    if (!setRunOnceRepair(args.installDir)) {
        // The reboot-mid-swap net could not be armed; the journal still covers
        // ordinary crashes (the client launches repair when it sees one).
        log::warn("continuing without RunOnce repair registration");
    }

    // ---- SWAP ----
    journal.state = "swapping";
    if (const auto saved = journal.save(args.installDir); !saved) {
        discardUpdateState(args.installDir);
        return fail(kNotWritable, saved.error());
    }

    for (std::size_t index = 0; index < ops.size(); ++index) {
        if (const auto swapped = swapOne(args.installDir, ops[index]); !swapped) {
            log::error("swap failed at {}: {}", ops[index].path, swapped.error());
            const bool rolledBack = rollbackSwap(args.installDir, ops, index + 1);
            // Discard the update only when every restore step succeeded AND
            // the old executable set is verifiably back in place.
            const auto exe = args.installDir / relative(args.relaunchExe);
            if (rolledBack && std::filesystem::exists(exe)) {
                discardUpdateState(args.installDir);
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

    const auto stateDir = stateDirFor(args);
    const auto loaded = Journal::load(args.installDir);
    if (!loaded) {
        // Corrupt journal. If backup/ or incoming/ hold files, they may be the
        // only copies of displaced originals — never delete them blind.
        const auto updateDir = args.installDir / L".update";
        if (dirHasEntries(updateDir / L"backup") || dirHasEntries(updateDir / L"incoming")) {
            log::error("repair: {}; preserving .update for recovery", loaded.error());
            writeLastApply(stateDir, "broken", "", loaded.error());
            return kSwapFailedBroken;
        }
        log::warn("repair: {}; no staged/backup files, cleaning up", loaded.error());
        discardUpdateState(args.installDir);
        return kNothingToDo;
    }
    if (!loaded->has_value()) {
        // No journal: nothing was mid-swap. Drop leftovers if any.
        if (std::filesystem::exists(args.installDir / L".update")) {
            log::info("repair: no journal; cleaning leftovers");
            discardUpdateState(args.installDir);
        }
        return kNothingToDo;
    }
    const Journal& journal = **loaded;

    log::info("repair: journal state={} version={}", journal.state, journal.version);
    if (journal.state == "staged") {
        // Swap never started -- everything valuable is still in place.
        discardUpdateState(args.installDir);
        log::info("repair: discarded staged update (client will re-check)");
        return kOk;
    }

    // state == "swapping": roll forward. Incoming files were hash-verified
    // before the swap began; each op is idempotent.
    if (!waitForUnlock(args.installDir / relative(journal.relaunchExe))) {
        log::error("repair: install exe still locked");
        return kParentAlive;
    }
    // RunOnce can fire after the client auto-started at logon: give every swap
    // target the same unlock grace instead of burning the rename backoff on a
    // file that is mapped into a running process.
    for (const auto& op : journal.ops) {
        const auto target = args.installDir / relative(op.path);
        if (fsops::isFileLocked(target) && !waitForUnlock(target)) {
            log::error("repair: {} still locked", op.path);
            return kParentAlive;
        }
    }
    for (const auto& op : journal.ops) {
        if (const auto swapped = swapOne(args.installDir, op); !swapped) {
            log::error("repair: swap still failing at {}: {}", op.path, swapped.error());
            writeLastApply(stateDir, "broken", journal.version, swapped.error());
            return kSwapFailedBroken;
        }
    }
    if (const auto committed = commit(args.installDir); !committed) {
        log::error("repair: commit failed: {}", committed.error());
        writeLastApply(stateDir, "broken", journal.version, committed.error());
        return kSwapFailedBroken;
    }
    writeLastApply(stateDir, "ok", journal.version, "repaired");
    log::info("repair: rolled forward to {}", journal.version);
    if (!args.noRelaunch) {
        relaunch(args.installDir, journal.relaunchExe);
    }
    return kOk;
}

}  // namespace updater
