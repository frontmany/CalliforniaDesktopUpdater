#include "apply.hpp"

#include "app_paths.hpp"
#include "fsops.hpp"
#include "journal.hpp"
#include "lock_owners.hpp"
#include "log.hpp"
#include "manifest.hpp"
#include "public_key.hpp"
#include "sha256.hpp"
#include "verify.hpp"
#include "watchdog.hpp"
#include "win_raii.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <expected>
#include <fstream>
#include <mutex>
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
constexpr wchar_t kApplyMutexName[] = L"Local\\CalliforniaUpdaterApplying";
constexpr wchar_t kRunOnceKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
constexpr wchar_t kRunOnceValue[] = L"CalliforniaUpdaterRepair";

// Whole-run ceiling. Generous enough for the worst legitimate case -- a full
// reinstall is ~1400 files and a quarter of a gigabyte copied, hashed and
// renamed on a slow disk -- and short enough that a user is never left without
// their app for an afternoon. Timing out mid-stage touches nothing; timing out
// mid-swap leaves the journal, which repair rolls forward.
constexpr auto kRunBudget = std::chrono::minutes{10};

// How long every swap target gets to be released before the swap starts. See
// waitForSwapTargetsUnlocked -- this is the fix for the failure this file was
// losing multi-version updates to.
constexpr auto kSwapUnlockGrace = std::chrono::seconds{30};

/// The apply mutex, held as a process-wide value rather than only as a member,
/// so the watchdog thread can hand it back before starting the app: the client
/// checks this mutex at startup and exits without a word while it is held.
std::atomic<HANDLE> gApplyMutex{nullptr};

void releaseApplyMutex() {
    if (HANDLE handle = gApplyMutex.exchange(nullptr); handle != nullptr) {
        ReleaseMutex(handle);
        CloseHandle(handle);
    }
}

/// Owns the single-instance apply mutex for the duration of a run.
class ApplyMutex {
public:
    ApplyMutex() {
        HANDLE handle = CreateMutexW(nullptr, TRUE, kApplyMutexName);
        mOwned = handle != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
        if (!mOwned) {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
            return;
        }
        gApplyMutex.store(handle);
    }

    // Copying would release/close the mutex twice.
    ApplyMutex(const ApplyMutex&) = delete;
    ApplyMutex& operator=(const ApplyMutex&) = delete;

    // Handing it back early is relaunch()'s job, through releaseApplyMutex():
    // the watchdog thread has to be able to do the same from outside any
    // scope, and two ways to release one mutex is one too many.
    ~ApplyMutex() { releaseApplyMutex(); }

    [[nodiscard]] bool owned() const { return mOwned; }

private:
    bool mOwned{false};
};

/// What the run is working on, readable from the watchdog thread. The version
/// is the only string the timeout path needs and the only one worth a lock.
std::mutex gRunVersionMutex;
std::string gRunVersion;

void setRunVersion(const std::string& version) {
    const std::scoped_lock lock{gRunVersionMutex};
    gRunVersion = version;
}

[[nodiscard]] std::string runVersion() {
    const std::scoped_lock lock{gRunVersionMutex};
    return gRunVersion;
}

/// True once the client is known to be gone, so the timeout path knows whether
/// starting it again is a rescue or a second copy.
std::atomic<bool> gParentGone{false};

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
            gParentGone.store(true);
            return true;  // pid no longer exists — the parent already exited
        }
        // ERROR_ACCESS_DENIED and friends: the pid exists but is not ours to
        // watch (recycled onto another user's process, elevation). Fail safe:
        // report "still alive" and let the client retry on its next launch
        // rather than swap files under a process we cannot observe.
        log::error("cannot watch parent pid {} (error {}); aborting", pid, error);
        return false;
    }
    const bool exited = WaitForSingleObject(process.get(), 30'000) == WAIT_OBJECT_0;
    gParentGone.store(exited);
    return exited;
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

/// Wait for everything the swap is about to touch to be released, not only the
/// executable we relaunch.
///
/// This is what a multi-version jump used to fall over. A step between adjacent
/// releases replaces exactly one file -- Callifornia.exe -- which is the one
/// file that did get an unlock grace, so those updates worked. Anything that
/// skips a release also drops the media stack (Qt6Multimedia, the ffmpeg codec
/// DLLs, the multimedia plugins) and replaces the D3D compiler: files the app
/// had mapped seconds earlier and that an antivirus re-scans the moment the
/// process holding them dies. Those got no grace at all, only the rename
/// backoff, and one straggling handle failed the whole update -- every time,
/// because the same jump produces the same op list. The mechanism is the same
/// one repair already used on its targets; apply simply never had it.
///
/// Advisory, not a gate. isFileLocked probes with no sharing at all, so a file
/// an indexer merely has open for reading reads as locked here while renaming
/// it would have been fine. Refusing the update on that would trade a rare
/// failure for a common one -- so this waits, says what it found, and lets the
/// rename retries be the real test.
void waitForSwapTargetsUnlocked(const std::filesystem::path& installDir,
                                const std::vector<FileOp>& ops) {
    using namespace std::chrono;
    watchdog::setPhase(watchdog::Phase::UnlockWait);

    std::vector<std::filesystem::path> pending;
    for (const auto& op : ops) {
        auto target = installDir / relative(op.path);
        if (fsops::isFileLocked(target)) {
            pending.push_back(std::move(target));
        }
    }
    if (pending.empty()) {
        return;
    }
    log::info("{} of {} swap targets still open; waiting up to {} s", pending.size(), ops.size(),
              duration_cast<seconds>(kSwapUnlockGrace).count());

    const auto deadline = steady_clock::now() + kSwapUnlockGrace;
    while (!pending.empty() && steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds{500});
        // Only the stragglers are re-probed: a full pass over a thousand-file
        // op list twice a second is a lot of opens for no new information.
        std::erase_if(pending, [](const std::filesystem::path& file) {
            return !fsops::isFileLocked(file);
        });
    }
    if (pending.empty()) {
        log::info("all swap targets released");
        return;
    }
    // Naming the holder is the difference between "error 32" and knowing
    // whether to wait longer, kill a straggler, or tell the user their
    // antivirus is doing it.
    const auto owners = lockOwners(pending);
    log::warn("{} swap target(s) still open after the grace, first is {}{}", pending.size(),
              pending.front().string(),
              owners.empty() ? "" : "; held by " + owners);
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
    watchdog::setPhase(watchdog::Phase::Relaunch);
    // Before, not after: the client tests this mutex on startup and quits
    // without a window when it is held, so a relaunch that still owns it is a
    // race whose losing side leaves the machine with no Callifornia running and
    // nothing to say why.
    releaseApplyMutex();
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

/// JSON string escaping. Details carry Windows paths, and an unescaped
/// backslash makes the file invalid JSON -- which mattered the moment the
/// client started showing this text to the user instead of discarding it.
std::string jsonEscaped(const std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char character : text) {
        switch (character) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                out += ' ';  // control characters have no place in a message
            } else {
                out += character;
            }
        }
    }
    return out;
}

void writeLastApply(const std::filesystem::path& stateDir,
                    const std::string& status,
                    const std::string& version,
                    const std::string& detail) {
    std::error_code ec;
    std::filesystem::create_directories(stateDir, ec);
    std::ofstream file{stateDir / L"last-apply.json", std::ios::binary | std::ios::trunc};
    if (file) {
        // Tiny fixed shape; hand-written to avoid pulling a JSON writer in for
        // four fields. `phase` is what turns a report of "the update did
        // nothing" into a report of which step it died on.
        file << "{\"status\":\"" << jsonEscaped(status) << "\",\"version\":\""
             << jsonEscaped(version) << "\",\"phase\":\""
             << jsonEscaped(watchdog::toString(watchdog::phase())) << "\",\"detail\":\""
             << jsonEscaped(detail) << "\"}";
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

/// An op with no incoming file left. Either the swap already happened, in which
/// case the file is at the target and there is nothing to do, or the file is
/// gone from both places.
///
/// The second case used to be reported as the first. It is not a theoretical
/// one: security software that decides a freshly written executable is malware
/// deletes it, and this project ships unsigned Windows binaries, which is most
/// of what such a decision is made of. Called "already done", the swap
/// continued, committed, and deleted .update\backup -- the last copy of the
/// version that worked -- leaving an install with no application in it and a
/// report saying the update succeeded.
[[nodiscard]] std::expected<void, std::string> landed(const std::filesystem::path& target,
                                                      const FileOp& op) {
    if (std::filesystem::exists(target)) {
        return {};  // both steps already done
    }
    return std::unexpected(op.path + " is missing from both the update and the install");
}

/// The first file an op promised to leave in the install and did not. Empty
/// when the swap landed everything.
[[nodiscard]] std::string firstMissingTarget(const std::filesystem::path& installDir,
                                             const std::vector<FileOp>& ops) {
    for (const auto& op : ops) {
        if (op.kind == FileOp::Kind::Remove) {
            continue;
        }
        if (!std::filesystem::exists(installDir / relative(op.path))) {
            return op.path;
        }
    }
    return {};
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
        return landed(target, op);
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
        return landed(target, op);
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

std::size_t pruneSatisfiedOps(std::vector<FileOp>& ops,
                              const std::filesystem::path& stagingDir,
                              const std::filesystem::path& installDir) {
    const auto planned = ops.size();
    // A staged file that is missing is not automatically a hole in the
    // download. If the file already sitting in the install hashes to what the
    // new manifest asks for, that op is done, and dropping it is not a
    // shortcut -- it is the truth. What survives this test is a real gap and
    // still fails the apply.
    std::erase_if(ops, [&](const FileOp& op) {
        if (op.kind == FileOp::Kind::Remove ||
            std::filesystem::exists(stagingDir / relative(op.path))) {
            return false;
        }
        const auto installed = sha256File(installDir / relative(op.path));
        return installed && *installed == op.sha256;
    });
    return planned - ops.size();
}

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

    // Nothing below is allowed to run forever. Individually every wait and
    // retry is already bounded, but the guarantee the user needs is that the
    // updater stops and gives them their app back no matter which step turns
    // out to be the one nobody bounded.
    watchdog::arm(kRunBudget, [installDir = args.installDir, relaunchExe = args.relaunchExe,
                               stateDir, noRelaunch = args.noRelaunch]() {
        writeLastApply(stateDir, "error", runVersion(),
                       std::string{"the update timed out while "} +
                           watchdog::toString(watchdog::phase()));
        // Only if the client is actually gone. Starting a second copy on top of
        // a live one would be a worse failure than the timeout.
        if (!noRelaunch && gParentGone.load()) {
            relaunch(installDir, relaunchExe);
        }
        releaseApplyMutex();
    });

    // ---- WAIT_PARENT ----
    watchdog::setPhase(watchdog::Phase::WaitParent);
    if (!waitForParentExit(args.parentPid)) {
        // The client is still up. Deliberately no relaunch: it is already
        // running, and touching the install under a live process is the one
        // thing this whole binary exists to avoid.
        log::error("parent process still running; aborting");
        writeLastApply(stateDir, "error", "", "the app did not exit");
        watchdog::disarm();
        return kParentAlive;
    }
    if (!waitForUnlock(args.installDir / relative(args.relaunchExe))) {
        // The process is gone but its executable is still held. Nothing has
        // been touched, so the update simply waits for the next attempt -- but
        // the user asked for a restart and is currently looking at no app at
        // all, so put one back.
        log::error("{} still locked after the app exited; aborting", args.relaunchExe);
        const auto owners = lockOwners({args.installDir / relative(args.relaunchExe)});
        if (!owners.empty()) {
            log::error("{} is held by {}", args.relaunchExe, owners);
        }
        writeLastApply(stateDir, "error", "",
                       "the program file was still in use after the app closed");
        if (!args.noRelaunch) {
            relaunch(args.installDir, args.relaunchExe);
        }
        watchdog::disarm();
        return kParentAlive;
    }

    // ---- PREFLIGHT ----
    watchdog::setPhase(watchdog::Phase::Preflight);
    if (!fsops::probeWritable(args.installDir)) {
        log::error("install dir is not writable: {}", args.installDir.string());
        writeLastApply(stateDir, "error", "", "the install folder cannot be written to");
        if (!args.noRelaunch) {
            relaunch(args.installDir, args.relaunchExe);
        }
        watchdog::disarm();
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
        watchdog::disarm();
        return code;
    };

    watchdog::setPhase(watchdog::Phase::Verify);
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
    setRunVersion(manifestVersion);  // so a timeout is attributable to a version too

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

    // ---- RECONCILE ----
    if (const auto planned = ops.size();
        pruneSatisfiedOps(ops, args.stagingDir, args.installDir) > 0) {
        log::info("{} of {} ops are already satisfied by the install; dropped",
                  planned - ops.size(), planned);
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
        watchdog::disarm();
        return kOk;
    }
    log::info("update to {}: {} ops", newManifest->version, ops.size());

    // Cheap existence pass; content is verified against the manifest hash by
    // copyVerified while staging below, still before any swap begins. What
    // reaches here is a genuine gap in the download -- RECONCILE above has
    // already forgiven every op the install itself satisfies.
    for (const auto& op : ops) {
        if (op.kind == FileOp::Kind::Remove) {
            continue;
        }
        if (!std::filesystem::exists(args.stagingDir / relative(op.path))) {
            return fail(kVerifyFailed, "the downloaded update is missing " + op.path);
        }
    }

    // ---- STAGE_LOCAL ---- (cross-volume copy; swap below is same-volume renames)
    watchdog::setPhase(watchdog::Phase::StageLocal);
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
        // Explicitly, rather than relying on a copy above having made it: an
        // update that only drops files copies nothing, so .update never
        // existed, the staged manifest could not be written and the whole
        // release failed as "install dir not writable" -- the same way every
        // time, on an install that was perfectly writable.
        std::error_code ec;
        std::filesystem::create_directories(args.installDir / L".update", ec);
        if (ec) {
            return fail(kNotWritable, "cannot create the update work folder");
        }
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

    // Last chance for stragglers to let go before anything is renamed. Costs a
    // pause the user does not see; buys the update that used to be lost to one
    // DLL an antivirus was still reading.
    waitForSwapTargetsUnlocked(args.installDir, ops);

    // ---- SWAP ----
    watchdog::setPhase(watchdog::Phase::Swap);
    journal.state = "swapping";
    if (const auto saved = journal.save(args.installDir); !saved) {
        discardUpdateState(args.installDir);
        return fail(kNotWritable, saved.error());
    }

    for (std::size_t index = 0; index < ops.size(); ++index) {
        if (const auto swapped = swapOne(args.installDir, ops[index]); !swapped) {
            log::error("swap failed at {}: {}", ops[index].path, swapped.error());
            const auto blocked = args.installDir / relative(ops[index].path);
            if (const auto owners = lockOwners({blocked}); !owners.empty()) {
                log::error("{} is held by {}", ops[index].path, owners);
            }
            // Rollback is what the reserve was held back for.
            watchdog::setPhase(watchdog::Phase::Rollback);
            watchdog::releaseReserve();
            const bool rolledBack = rollbackSwap(args.installDir, ops, index + 1);
            // Discard the update only when every restore step succeeded AND
            // the old executable set is verifiably back in place.
            const auto exe = args.installDir / relative(args.relaunchExe);
            if (rolledBack && std::filesystem::exists(exe)) {
                discardUpdateState(args.installDir);
                writeLastApply(stateDir, "error", newManifest->version,
                               "the update could not replace " + ops[index].path +
                                   " and the previous version was put back");
                if (!args.noRelaunch) {
                    relaunch(args.installDir, args.relaunchExe);
                }
                watchdog::disarm();
                return kSwapFailedRolledBack;
            }
            log::error("rollback incomplete; journal left for repair");
            writeLastApply(stateDir, "broken", newManifest->version,
                           "the update stopped partway through and could not be undone");
            // Half-swapped, so the journal stays and repair owns the outcome --
            // but the app the user was using is gone from the screen right now.
            // Start whatever is in place: a client that comes up finds the
            // journal and hands off to repair, and one that cannot start is a
            // fact worth learning immediately rather than at the next logon.
            if (!args.noRelaunch && std::filesystem::exists(exe)) {
                relaunch(args.installDir, args.relaunchExe);
            }
            watchdog::disarm();
            return kSwapFailedBroken;
        }
    }

    // ---- VERIFY ----
    // Committing destroys .update\backup, which until this line holds the only
    // copy of the version that was working. Nothing checked that the files just
    // installed are still on disk -- and they can fail to be, seconds after
    // landing, because an antivirus decided the new executable was malware and
    // deleted it. Unsigned binaries with no reputation are what that decision is
    // mostly made of, and this release is unsigned. Committing anyway leaves an
    // install with no application in it, no backup, and a report saying the
    // update succeeded.
    watchdog::setPhase(watchdog::Phase::Verify);
    if (const auto missing = firstMissingTarget(args.installDir, ops); !missing.empty()) {
        log::error("{} is gone from the install moments after it was written", missing);
        watchdog::setPhase(watchdog::Phase::Rollback);
        watchdog::releaseReserve();
        const bool rolledBack = rollbackSwap(args.installDir, ops, ops.size());
        const auto exe = args.installDir / relative(args.relaunchExe);
        const std::string detail =
            missing + " was removed right after it was installed, most likely by security "
                      "software";
        if (rolledBack && std::filesystem::exists(exe)) {
            discardUpdateState(args.installDir);
            writeLastApply(stateDir, "error", newManifest->version,
                           detail + ", and the previous version was put back");
            if (!args.noRelaunch) {
                relaunch(args.installDir, args.relaunchExe);
            }
            watchdog::disarm();
            return kSwapFailedRolledBack;
        }
        log::error("rollback incomplete; journal left for repair");
        writeLastApply(stateDir, "broken", newManifest->version, detail);
        if (!args.noRelaunch && std::filesystem::exists(exe)) {
            relaunch(args.installDir, args.relaunchExe);
        }
        watchdog::disarm();
        return kSwapFailedBroken;
    }

    // ---- COMMIT ----
    watchdog::setPhase(watchdog::Phase::Commit);
    if (const auto committed = commit(args.installDir); !committed) {
        // Files are fully swapped; only bookkeeping failed. Repair finishes it.
        log::error("commit incomplete: {}", committed.error());
        writeLastApply(stateDir, "broken", newManifest->version, committed.error());
        // The new files are all in place, so this install is startable and the
        // leftover journal is bookkeeping for repair to tidy. No reason to
        // leave the user staring at nothing while that happens.
        if (!args.noRelaunch) {
            relaunch(args.installDir, args.relaunchExe);
        }
        watchdog::disarm();
        return kSwapFailedBroken;
    }
    fsops::removeTreeQuiet(args.stagingDir);
    writeLastApply(stateDir, "ok", newManifest->version, "");
    log::info("update to {} applied", newManifest->version);

    // ---- RELAUNCH ----
    if (!args.noRelaunch) {
        relaunch(args.installDir, args.relaunchExe);
    }
    watchdog::disarm();
    return kOk;
}

int runRepair(const Args& args) {
    const ApplyMutex mutex;
    if (!mutex.owned()) {
        return kBadArgs;
    }

    const auto stateDir = stateDirFor(args);

    // Repair is launched by a client that then exits, handing over its place on
    // the user's screen. Every way out of this function therefore owes them a
    // running app -- the old habit of returning quietly on the paths that could
    // not finish is what turned an unfinished update into an application that
    // appeared to do nothing at all when its icon was clicked.
    const auto startApp = [&args](const std::string& exeName) {
        if (args.noRelaunch) {
            return;
        }
        const auto exe = args.installDir / relative(exeName);
        if (!std::filesystem::exists(exe)) {
            log::error("repair: {} is not there to start", exe.string());
            return;
        }
        relaunch(args.installDir, exeName);
    };

    watchdog::arm(kRunBudget, [installDir = args.installDir, relaunchExe = args.relaunchExe,
                               stateDir, noRelaunch = args.noRelaunch]() {
        writeLastApply(stateDir, "error", runVersion(),
                       std::string{"the repair timed out while "} +
                           watchdog::toString(watchdog::phase()));
        if (!noRelaunch && std::filesystem::exists(installDir / relative(relaunchExe))) {
            relaunch(installDir, relaunchExe);
        }
        releaseApplyMutex();
    });
    const auto finish = [&](const int code) {
        watchdog::disarm();
        return code;
    };

    const auto loaded = Journal::load(args.installDir);
    if (!loaded) {
        // Corrupt or unreadable journal. If backup/ or incoming/ hold files,
        // they may be the only copies of displaced originals — never delete
        // them blind.
        const auto updateDir = args.installDir / L".update";
        if (dirHasEntries(updateDir / L"backup") || dirHasEntries(updateDir / L"incoming")) {
            log::error("repair: {}; preserving .update for recovery", loaded.error());
            writeLastApply(stateDir, "broken", "", loaded.error());
            startApp(args.relaunchExe);
            return finish(kSwapFailedBroken);
        }
        log::warn("repair: {}; no staged/backup files, cleaning up", loaded.error());
        discardUpdateState(args.installDir);
        startApp(args.relaunchExe);
        return finish(kNothingToDo);
    }
    if (!loaded->has_value()) {
        // No journal: nothing was mid-swap. Drop leftovers if any.
        if (std::filesystem::exists(args.installDir / L".update")) {
            log::info("repair: no journal; cleaning leftovers");
            discardUpdateState(args.installDir);
        }
        startApp(args.relaunchExe);
        return finish(kNothingToDo);
    }
    const Journal& journal = **loaded;

    log::info("repair: journal state={} version={}", journal.state, journal.version);
    setRunVersion(journal.version);
    if (journal.state == "staged") {
        // Swap never started -- everything valuable is still in place.
        discardUpdateState(args.installDir);
        log::info("repair: discarded staged update (client will re-check)");
        startApp(journal.relaunchExe);
        return finish(kOk);
    }

    // state == "swapping": roll forward. Incoming files were hash-verified
    // before the swap began; each op is idempotent.
    watchdog::setPhase(watchdog::Phase::UnlockWait);
    if (!waitForUnlock(args.installDir / relative(journal.relaunchExe))) {
        log::error("repair: install exe still locked");
        // Something is running out of this install: it is not this repair's to
        // fight, and whatever it is, the user is not staring at an empty
        // screen. Leave the journal for the next start.
        return finish(kParentAlive);
    }
    // RunOnce can fire after the client auto-started at logon: give every swap
    // target the same unlock grace instead of burning the rename backoff on a
    // file that is mapped into a running process. One shared grace, not thirty
    // seconds each -- a full reinstall's op list is over a thousand files, and
    // one at a time that is most of a day.
    waitForSwapTargetsUnlocked(args.installDir, journal.ops);
    // Unlike apply, repair does refuse on a target that is still held. It is
    // running because a swap was already interrupted once, and starting a
    // second one into a file somebody has open is how a half-finished update
    // becomes an unfinished one. The journal keeps; the next start tries again.
    for (const auto& op : journal.ops) {
        if (fsops::isFileLocked(args.installDir / relative(op.path))) {
            log::error("repair: {} still locked; leaving the journal for the next attempt",
                       op.path);
            return finish(kParentAlive);
        }
    }
    watchdog::setPhase(watchdog::Phase::Swap);
    for (const auto& op : journal.ops) {
        if (const auto swapped = swapOne(args.installDir, op); !swapped) {
            log::error("repair: swap still failing at {}: {}", op.path, swapped.error());
            // Rolling forward is impossible, and leaving it here would leave a
            // journal that makes the client hand over to repair on every start
            // and get this same answer -- an install that can never be run and
            // never be fixed. Put the previous version back instead: an app one
            // version behind still starts, still updates, and still tells its
            // user something.
            watchdog::setPhase(watchdog::Phase::Rollback);
            watchdog::releaseReserve();
            const bool restored = rollbackSwap(args.installDir, journal.ops, journal.ops.size());
            const auto exe = args.installDir / relative(journal.relaunchExe);
            if (restored && std::filesystem::exists(exe)) {
                discardUpdateState(args.installDir);
                log::warn("repair: rolled back to the previous version");
                writeLastApply(stateDir, "error", journal.version,
                               "the update could not be finished and the previous version was "
                               "put back");
                startApp(journal.relaunchExe);
                return finish(kSwapFailedRolledBack);
            }
            log::error("repair: rollback incomplete; journal kept");
            writeLastApply(stateDir, "broken", journal.version, swapped.error());
            startApp(journal.relaunchExe);
            return finish(kSwapFailedBroken);
        }
    }
    // Same guard as apply: the backup is about to be destroyed, so what it was
    // insuring against had better not have already happened.
    watchdog::setPhase(watchdog::Phase::Verify);
    if (const auto missing = firstMissingTarget(args.installDir, journal.ops); !missing.empty()) {
        log::error("repair: {} is not in the install after the swap; rolling back", missing);
        watchdog::setPhase(watchdog::Phase::Rollback);
        watchdog::releaseReserve();
        const bool restored = rollbackSwap(args.installDir, journal.ops, journal.ops.size());
        const auto exe = args.installDir / relative(journal.relaunchExe);
        if (restored && std::filesystem::exists(exe)) {
            discardUpdateState(args.installDir);
            writeLastApply(stateDir, "error", journal.version,
                           missing + " was removed right after it was installed, most likely by "
                                     "security software, and the previous version was put back");
            startApp(journal.relaunchExe);
            return finish(kSwapFailedRolledBack);
        }
        writeLastApply(stateDir, "broken", journal.version, missing + " is missing from the install");
        startApp(journal.relaunchExe);
        return finish(kSwapFailedBroken);
    }

    watchdog::setPhase(watchdog::Phase::Commit);
    if (const auto committed = commit(args.installDir); !committed) {
        log::error("repair: commit failed: {}", committed.error());
        writeLastApply(stateDir, "broken", journal.version, committed.error());
        // Files are all in place; only bookkeeping is behind. Startable.
        startApp(journal.relaunchExe);
        return finish(kSwapFailedBroken);
    }
    writeLastApply(stateDir, "ok", journal.version, "repaired");
    log::info("repair: rolled forward to {}", journal.version);
    startApp(journal.relaunchExe);
    return finish(kOk);
}

}  // namespace updater
