// Unit tests for applylib: manifest validation, diff, journal round-trip, and
// the apply/repair state machine against temp directories. No framework --
// plain CHECK macros, exit code = failure count.

#include "apply.hpp"
#include "args.hpp"
#include "fsops.hpp"
#include "journal.hpp"
#include "manifest.hpp"
#include "sha256.hpp"
#include "verify.hpp"
#include "watchdog.hpp"
#include "win_raii.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++gFailures;                                                              \
            std::cout << "FAIL " << __FUNCTION__ << ":" << __LINE__ << "  " #condition \
                      << "\n";                                                        \
        }                                                                             \
    } while (false)

namespace fs = std::filesystem;

std::string manifestJson(const std::string& version, const std::string& filesJson) {
    return std::format(
        R"({{"schema":1,"key_id":"k1","version":"{}","platform":"win-x64","channel":"stable",)"
        R"("min_supported_version":"0.1.0","created_at":"2026-01-01T00:00:00Z","notes":"",)"
        R"("files":[{}]}})",
        version, filesJson);
}

std::string fileEntry(const std::string& path, const std::string& sha, const long long size) {
    return std::format(R"({{"path":"{}","sha256":"{}","size":{}}})", path, sha, size);
}

const std::string kZeroHash(64, '0');

std::string baseFiles(const std::string& shaClient = kZeroHash,
                      const std::string& shaUpdater = kZeroHash) {
    return fileEntry("Callifornia.exe", shaClient, 1) + "," +
           fileEntry("updater.exe", shaUpdater, 1);
}

void writeFile(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    if (!out.good()) {
        // A broken fixture must fail loudly, not as a confusing later CHECK.
        ++gFailures;
        std::cout << "FAIL setup: cannot write " << path.string() << "\n";
    }
}

std::string readFile(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

fs::path freshDir(const std::string& name) {
    const auto dir = fs::temp_directory_path() / "updater-tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        ++gFailures;
        std::cout << "FAIL setup: cannot create " << dir.string() << "\n";
    }
    return dir;
}

// Every runRepair call writes last-apply.json; point it at a scratch dir so
// tests never overwrite the real %LOCALAPPDATA%\Callifornia\updates state.
updater::Args repairArgs(const fs::path& install, const std::string& name) {
    updater::Args args;
    args.mode = updater::Args::Mode::Repair;
    args.installDir = install;
    args.noRelaunch = true;
    args.stateDir = freshDir(name + "-state");
    return args;
}

// --- Manifest validation ----------------------------------------------------

void testManifestParsing() {
    CHECK(updater::Manifest::parse(manifestJson("1.2.3", baseFiles())).has_value());

    // Path attacks and malformed entries must all be rejected.
    const std::string badPaths[] = {
        "../evil.dll", "a/../b.dll", "/abs.dll", "C:/win.dll", "a\\b.dll",
        ".update/x.dll", ".update", "a//b.dll", "./x.dll", "",
    };
    for (const auto& path : badPaths) {
        const auto parsed = updater::Manifest::parse(
            manifestJson("1.2.3", baseFiles() + "," + fileEntry(path, kZeroHash, 1)));
        CHECK(!parsed.has_value());
    }

    // Case-insensitive duplicates collide on Windows.
    CHECK(!updater::Manifest::parse(
               manifestJson("1.2.3", baseFiles() + "," + fileEntry("A.dll", kZeroHash, 1) + "," +
                                         fileEntry("a.dll", kZeroHash, 1)))
               .has_value());

    // Version formats.
    for (const auto* version : {"1.2", "1.2.3.4", "01.2.3", "a.b.c", "1.2.-3", ""}) {
        CHECK(!updater::Manifest::parse(manifestJson(version, baseFiles())).has_value());
    }

    // Bad hash.
    CHECK(!updater::Manifest::parse(
               manifestJson("1.2.3", baseFiles(std::string(64, 'G')))).has_value());

    // Regression guard for the exe renames (Client.exe -> Calls.exe ->
    // Callifornia.exe): parse must NEVER require a file by name -- that rule
    // once shipped inside released binaries and
    // made every installed client reject every later manifest. Which exe has to
    // be present is decided in runApply from args.relaunchExe.
    CHECK(updater::Manifest::parse(manifestJson("1.2.3", fileEntry("anything.dll", kZeroHash, 1)))
              .has_value());
}

void testDiff() {
    const auto hashA = std::string(64, 'a');
    const auto hashB = std::string(64, 'b');
    const auto oldManifest = updater::Manifest::parse(manifestJson(
        "1.0.0", baseFiles() + "," + fileEntry("keep.dll", hashA, 1) + "," +
                     fileEntry("changed.dll", hashA, 1) + "," + fileEntry("gone.dll", hashA, 1)));
    const auto newManifest = updater::Manifest::parse(manifestJson(
        "1.1.0", baseFiles() + "," + fileEntry("keep.dll", hashA, 1) + "," +
                     fileEntry("Changed.dll", hashB, 1) + "," + fileEntry("new.dll", hashB, 1)));
    CHECK(oldManifest.has_value() && newManifest.has_value());

    const auto ops = updater::diffManifests(*oldManifest, *newManifest);
    int replaces = 0;
    int adds = 0;
    int removes = 0;
    for (const auto& op : ops) {
        switch (op.kind) {
        case updater::FileOp::Kind::Replace: ++replaces; break;
        case updater::FileOp::Kind::Add: ++adds; break;
        case updater::FileOp::Kind::Remove: ++removes; break;
        }
    }
    // Changed.dll differs (case-insensitive match), new.dll added, gone.dll removed.
    CHECK(replaces == 1);
    CHECK(adds == 1);
    CHECK(removes == 1);
}

void testBase64() {
    const auto hello = updater::base64Decode("aGVsbG8=");
    CHECK(hello.has_value() && *hello == "hello");
    const auto empty = updater::base64Decode("");
    CHECK(empty.has_value() && empty->empty());
    CHECK(!updater::base64Decode("a").has_value());
    CHECK(!updater::base64Decode("ab=c").has_value());
    CHECK(!updater::base64Decode("!!!!").has_value());
}

void testSha256() {
    const auto empty = updater::sha256Bytes("");
    CHECK(empty.has_value() &&
          *empty == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const auto abc = updater::sha256Bytes("abc");
    CHECK(abc.has_value() &&
          *abc == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void testJournalRoundTrip() {
    const auto dir = freshDir("journal");
    updater::Journal journal{
        .state = "swapping",
        .version = "1.2.3",
        .relaunchExe = "Callifornia.exe",
        .ops = {{updater::FileOp::Kind::Replace, "a.dll", std::string(64, 'a')},
                {updater::FileOp::Kind::Remove, "b.dll", ""}},
    };
    CHECK(journal.save(dir).has_value());
    const auto loaded = updater::Journal::load(dir);
    CHECK(loaded.has_value() && loaded->has_value());
    if (loaded && *loaded) {
        CHECK((*loaded)->state == "swapping");
        CHECK((*loaded)->version == "1.2.3");
        CHECK((*loaded)->ops.size() == 2);
        CHECK((*loaded)->ops[1].kind == updater::FileOp::Kind::Remove);
    }

    // Absent journal: success carrying "nothing".
    const auto none = updater::Journal::load(freshDir("journal-none"));
    CHECK(none.has_value() && !none->has_value());

    // Corrupt journal: an error, NOT "absent" — repair must not treat it as
    // "nothing was mid-swap".
    const auto corruptDir = freshDir("journal-corrupt");
    writeFile(corruptDir / ".update" / "journal.json", "{not json");
    CHECK(!updater::Journal::load(corruptDir).has_value());

    // Present but unopenable (locked, denied, failing disk) is the same answer
    // as corrupt and the opposite of absent: reading it as "no journal" sends
    // repair into the cleanup branch, which deletes .update\backup — sometimes
    // the only copy of the files a half-finished swap displaced.
    const auto lockedDir = freshDir("journal-locked");
    const auto lockedJournal = lockedDir / ".update" / "journal.json";
    writeFile(lockedJournal, "{}");
    const updater::UniqueHandle exclusive{CreateFileW(lockedJournal.c_str(), GENERIC_READ,
                                                      0 /* no sharing */, nullptr, OPEN_EXISTING,
                                                      FILE_ATTRIBUTE_NORMAL, nullptr)};
    CHECK(exclusive.valid());
    if (exclusive.valid()) {
        CHECK(!updater::Journal::load(lockedDir).has_value());
    }
}

void testPruneSatisfiedOps() {
    // The client stages only what its own diff needed; a plan built here from
    // an unreadable local manifest asks for the whole release. Ops the install
    // already satisfies must drop out instead of failing the apply forever.
    const auto root = freshDir("prune");
    const auto staging = root / "staging";
    const auto install = root / "install";
    writeFile(install / "unchanged.dll", "same");
    writeFile(install / "stale.dll", "old");
    writeFile(staging / "changed.dll", "new");

    const auto sameHash = updater::sha256Bytes("same");
    CHECK(sameHash.has_value());
    std::vector<updater::FileOp> ops{
        // Staged: kept whatever its hash.
        {updater::FileOp::Kind::Replace, "changed.dll", kZeroHash},
        // Not staged, but the installed file is already what the manifest wants.
        {updater::FileOp::Kind::Add, "unchanged.dll", sameHash.value_or("")},
        // Not staged and the installed file is something else: a real gap.
        {updater::FileOp::Kind::Replace, "stale.dll", kZeroHash},
        // Removals never need a staged file.
        {updater::FileOp::Kind::Remove, "gone.dll", ""},
    };

    CHECK(updater::pruneSatisfiedOps(ops, staging, install) == 1);
    CHECK(ops.size() == 3);
    CHECK(std::ranges::none_of(ops, [](const updater::FileOp& op) {
        return op.path == "unchanged.dll";
    }));
    // Nothing was dropped that would have hidden a genuine hole.
    CHECK(std::ranges::any_of(ops, [](const updater::FileOp& op) {
        return op.path == "stale.dll";
    }));
}

void testWatchdogBudget() {
    using namespace std::chrono;
    // Unarmed: no ceiling, so nothing in fsops shortens its retries.
    CHECK(!updater::watchdog::expired());
    CHECK(updater::watchdog::workBudget() > hours{1});

    updater::watchdog::arm(minutes{10}, [] {});
    const auto left = updater::watchdog::remaining();
    CHECK(left <= minutes{10} && left > minutes{9});
    // The reserve is held back from ordinary work so a swap that runs long
    // still has room to roll back.
    CHECK(updater::watchdog::workBudget() < updater::watchdog::remaining());
    updater::watchdog::releaseReserve();
    CHECK(updater::watchdog::workBudget() > minutes{9});

    updater::watchdog::disarm();
    CHECK(!updater::watchdog::expired());
    CHECK(updater::watchdog::workBudget() > hours{1});
}

// The whole "it can never hang" claim rests on a code path that ends in
// TerminateProcess, which is exactly the code an in-process test cannot run
// without taking the suite down with it. So run it in a process that is meant
// to die: this binary, re-launched in a mode that arms a two second budget and
// then deliberately stops making progress. Untested, this was the one part of
// the guarantee taken on faith -- and a watchdog that does not fire is worse
// than none, because it is the reason nothing else was bounded.
void testWatchdogKillsAStuckRun() {
    using namespace std::chrono;
    const auto dir = freshDir("watchdog-kill");
    const auto marker = dir / "expired.txt";

    std::wstring selfPath(32'768, L'\0');
    const auto length =
        GetModuleFileNameW(nullptr, selfPath.data(), static_cast<DWORD>(selfPath.size()));
    CHECK(length > 0 && length < selfPath.size());
    if (length == 0 || length >= selfPath.size()) {
        return;
    }
    selfPath.resize(length);

    std::wstring command = L"\"" + selfPath + L"\" --watchdog-child \"" + marker.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(selfPath.c_str(), command.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                        &process) != 0;
    CHECK(started);
    if (!started) {
        return;
    }
    CloseHandle(process.hThread);

    // Generous next to a two second budget: what is being checked is that it
    // dies at all, not to the millisecond.
    const bool died = WaitForSingleObject(process.hProcess, 20'000) == WAIT_OBJECT_0;
    CHECK(died);
    DWORD code = 0;
    if (died) {
        GetExitCodeProcess(process.hProcess, &code);
    } else {
        TerminateProcess(process.hProcess, 1);  // do not leave it behind
    }
    CloseHandle(process.hProcess);

    CHECK(code == static_cast<DWORD>(updater::kTimedOut));
    // And it recorded why before going: a process that dies silently tells the
    // client nothing, which is how the failure that started all this stayed
    // invisible.
    CHECK(fs::exists(marker));
}

// --- Apply state machine (no signature: exercised via repair paths) ---------

// Full runApply needs a valid signature; the signing key lives outside tests.
// The swap/rollback/repair mechanics are exercised through Journal + repair,
// which skip signature checks by design (pre-verified before swap).

void testRepairRollForward() {
    const auto install = freshDir("repair-install");
    writeFile(install / "Callifornia.exe", "old-exe");
    writeFile(install / "stay.dll", "stay");
    writeFile(install / "old.dll", "old");

    // Simulate a crash mid-swap: incoming holds the new files, journal says swapping.
    writeFile(install / ".update" / "incoming" / "Callifornia.exe", "new-exe");
    writeFile(install / ".update" / "incoming" / "fresh.dll", "fresh");
    writeFile(install / ".update" / "manifest.json", "{\"new\":\"manifest\"}");

    updater::Journal journal{
        .state = "swapping",
        .version = "2.0.0",
        .relaunchExe = "Callifornia.exe",
        .ops = {{updater::FileOp::Kind::Replace, "Callifornia.exe", ""},
                {updater::FileOp::Kind::Add, "fresh.dll", ""},
                {updater::FileOp::Kind::Remove, "old.dll", ""}},
    };
    CHECK(journal.save(install).has_value());

    CHECK(updater::runRepair(repairArgs(install, "repair-install")) == updater::kOk);

    CHECK(readFile(install / "Callifornia.exe") == "new-exe");
    CHECK(readFile(install / "fresh.dll") == "fresh");
    CHECK(readFile(install / "stay.dll") == "stay");
    CHECK(!fs::exists(install / "old.dll"));
    CHECK(!fs::exists(install / ".update"));
    CHECK(readFile(install / "manifest.json") == "{\"new\":\"manifest\"}");
}

void testRepairHalfSwapped() {
    // Crash after some ops already ran: replace done, add not, remove not.
    const auto install = freshDir("repair-half");
    writeFile(install / "Callifornia.exe", "new-exe");  // already swapped
    writeFile(install / ".update" / "backup" / "Callifornia.exe", "old-exe");
    writeFile(install / ".update" / "incoming" / "fresh.dll", "fresh");
    writeFile(install / "old.dll", "old");
    writeFile(install / ".update" / "manifest.json", "m2");

    // Real target hashes, as a journal always carries them: rolling an op
    // forward when its incoming file is already gone has to tell "the swap
    // finished" apart from "the new file never arrived and the old one is
    // still sitting there", and only the hash says which.
    updater::Journal journal{
        .state = "swapping",
        .version = "2.0.0",
        .relaunchExe = "Callifornia.exe",
        .ops = {{updater::FileOp::Kind::Replace, "Callifornia.exe",
                 *updater::sha256Bytes("new-exe")},
                {updater::FileOp::Kind::Add, "fresh.dll", *updater::sha256Bytes("fresh")},
                {updater::FileOp::Kind::Remove, "old.dll", ""}},
    };
    CHECK(journal.save(install).has_value());

    CHECK(updater::runRepair(repairArgs(install, "repair-half")) == updater::kOk);

    CHECK(readFile(install / "Callifornia.exe") == "new-exe");
    CHECK(readFile(install / "fresh.dll") == "fresh");
    CHECK(!fs::exists(install / "old.dll"));
    CHECK(!fs::exists(install / ".update"));
}

// The case the hash is there for. The incoming file is gone -- an antivirus
// took it between staging and the swap -- and the original is still at the
// target. Existence alone reads that as "already done", and commit then
// destroys the backup that was the last copy of a working install.
void testRepairRefusesAStaleTargetWhenTheIncomingVanished() {
    const auto install = freshDir("repair-stale");
    writeFile(install / "Callifornia.exe", "old-exe");  // never replaced
    writeFile(install / ".update" / "backup" / "Callifornia.exe", "old-exe");
    writeFile(install / ".update" / "manifest.json", "m2");

    updater::Journal journal{
        .state = "swapping",
        .version = "2.0.0",
        .relaunchExe = "Callifornia.exe",
        .ops = {{updater::FileOp::Kind::Replace, "Callifornia.exe",
                 *updater::sha256Bytes("new-exe")}},
    };
    CHECK(journal.save(install).has_value());

    CHECK(updater::runRepair(repairArgs(install, "repair-stale")) == updater::kSwapFailedRolledBack);
    // The working version is back and the update state is gone, rather than a
    // success reported over a binary that was never replaced.
    CHECK(readFile(install / "Callifornia.exe") == "old-exe");
    CHECK(!fs::exists(install / ".update"));
}

void testRepairStagedDiscards() {
    const auto install = freshDir("repair-staged");
    writeFile(install / "Callifornia.exe", "exe");
    writeFile(install / ".update" / "incoming" / "x.dll", "x");
    updater::Journal journal{
        .state = "staged",
        .version = "2.0.0",
        .relaunchExe = "Callifornia.exe",
        .ops = {{updater::FileOp::Kind::Add, "x.dll", ""}},
    };
    CHECK(journal.save(install).has_value());

    CHECK(updater::runRepair(repairArgs(install, "repair-staged")) == updater::kOk);
    CHECK(readFile(install / "Callifornia.exe") == "exe");
    CHECK(!fs::exists(install / ".update"));
    CHECK(!fs::exists(install / "x.dll"));
}

void testRepairRollsBackWhenRollForwardFails() {
    // An op that cannot be completed used to leave the journal in place and
    // return. The client hands over to repair on every start when a journal is
    // there and exits, so that state is an install nobody can run and nothing
    // can fix. Repair now undoes the swap instead, leaving the previous version
    // whole and startable.
    const auto install = freshDir("repair-give-up");
    writeFile(install / "Callifornia.exe", "old-exe");
    writeFile(install / "sub" / "doomed.dll", "still-here");
    // A FILE where the backup tree needs a DIRECTORY: the rename cannot even
    // create its destination, and fails without burning the retry backoff.
    writeFile(install / ".update" / "backup" / "sub", "in the way");

    updater::Journal journal{
        .state = "swapping",
        .version = "2.0.0",
        .relaunchExe = "Callifornia.exe",
        .ops = {{updater::FileOp::Kind::Remove, "sub/doomed.dll", ""}},
    };
    CHECK(journal.save(install).has_value());

    CHECK(updater::runRepair(repairArgs(install, "repair-give-up")) ==
          updater::kSwapFailedRolledBack);
    // Previous version intact, nothing half-removed, and no journal left to
    // send the next start round the same loop.
    CHECK(readFile(install / "Callifornia.exe") == "old-exe");
    CHECK(readFile(install / "sub" / "doomed.dll") == "still-here");
    CHECK(!fs::exists(install / ".update"));
}

// A file that is gone from both the update and the install used to read as
// "already swapped". It is the shape an antivirus leaves behind: the new
// executable is written, something decides it is malware and deletes it, and
// the only remaining copy of the old one is in .update\backup -- which commit
// then deletes too, reporting success. The install ends up with no application
// in it and nothing able to start and say why.
void testRepairAFileRemovedFromUnderItIsNotSuccess() {
    const auto install = freshDir("repair-vanished");
    writeFile(install / "Callifornia.exe", "old-exe");
    // Mid-swap: the old app.dll was moved aside, and the new one is gone.
    writeFile(install / ".update" / "backup" / "app.dll", "old-app");
    writeFile(install / ".update" / "manifest.json", "m2");

    updater::Journal journal{
        .state = "swapping",
        .version = "2.0.0",
        .relaunchExe = "Callifornia.exe",
        .ops = {{updater::FileOp::Kind::Replace, "app.dll", ""}},
    };
    CHECK(journal.save(install).has_value());

    CHECK(updater::runRepair(repairArgs(install, "repair-vanished")) ==
          updater::kSwapFailedRolledBack);
    // The old file is back rather than lost, and the manifest was NOT committed
    // over an install that does not match it.
    CHECK(readFile(install / "app.dll") == "old-app");
    CHECK(readFile(install / "Callifornia.exe") == "old-exe");
    CHECK(!fs::exists(install / ".update"));
    CHECK(!fs::exists(install / "manifest.json"));
}

void testRepairNoJournal() {
    const auto install = freshDir("repair-nothing");
    writeFile(install / "Callifornia.exe", "exe");
    CHECK(updater::runRepair(repairArgs(install, "repair-nothing")) == updater::kNothingToDo);
}

void testRepairCorruptJournalPreservesBackups() {
    // Power loss corrupted the journal mid-swap: backup/ still holds the only
    // copy of the old exe. Repair must preserve .update, not "clean up".
    const auto install = freshDir("repair-corrupt");
    writeFile(install / ".update" / "journal.json", "{truncated");
    writeFile(install / ".update" / "backup" / "Callifornia.exe", "old-exe");

    CHECK(updater::runRepair(repairArgs(install, "repair-corrupt")) ==
          updater::kSwapFailedBroken);
    CHECK(fs::exists(install / ".update" / "backup" / "Callifornia.exe"));
    CHECK(fs::exists(install / ".update" / "journal.json"));
}

void testRepairCorruptJournalNoValuablesCleansUp() {
    // Corrupt journal but nothing staged or backed up: safe to clean.
    const auto install = freshDir("repair-corrupt-empty");
    writeFile(install / "Callifornia.exe", "exe");
    writeFile(install / ".update" / "journal.json", "{truncated");

    CHECK(updater::runRepair(repairArgs(install, "repair-corrupt-empty")) ==
          updater::kNothingToDo);
    CHECK(!fs::exists(install / ".update"));
    CHECK(readFile(install / "Callifornia.exe") == "exe");
}

void testArgs() {
    using updater::Args;
    const auto ok = Args::parse(
        {L"updater.exe", L"apply", L"--install-dir", L"C:\\x", L"--staging-dir", L"C:\\y",
         L"--parent-pid", L"123", L"--relaunch-exe", L"Callifornia.exe", L"--no-relaunch"});
    CHECK(ok.has_value());
    if (ok) {
        CHECK(ok->mode == Args::Mode::Apply);
        CHECK(ok->parentPid == 123);
        CHECK(ok->noRelaunch);
    }
    CHECK(!Args::parse({L"updater.exe", L"apply", L"--install-dir", L"relative"}).has_value());
    CHECK(!Args::parse({L"updater.exe", L"apply", L"--install-dir", L"C:\\x"}).has_value());
    CHECK(!Args::parse({L"updater.exe", L"repair"}).has_value());
    CHECK(Args::parse({L"updater.exe", L"repair", L"--install-dir", L"C:\\x"}).has_value());
    CHECK(!Args::parse({L"updater.exe", L"apply", L"--install-dir", L"C:\\x", L"--staging-dir",
                        L"C:\\y", L"--parent-pid", L"123", L"--relaunch-exe", L"..\\evil.exe"})
               .has_value());
    // Drive-relative names ("C:x.exe") would resolve outside the install dir;
    // ".." always exists and defeats the rollback health check.
    CHECK(!Args::parse({L"updater.exe", L"repair", L"--install-dir", L"C:\\x",
                        L"--relaunch-exe", L"C:x.exe"})
               .has_value());
    CHECK(!Args::parse({L"updater.exe", L"repair", L"--install-dir", L"C:\\x",
                        L"--relaunch-exe", L".."})
               .has_value());
}

}  // namespace

int main(const int argc, char** argv) {
    // Child mode for testWatchdogKillsAStuckRun. Not a test itself: a process
    // whose job is to arm the watchdog, stop making progress, and be killed.
    if (argc >= 3 && std::string_view{argv[1]} == "--watchdog-child") {
        const std::string markerPath = argv[2];
        updater::watchdog::arm(std::chrono::seconds{2}, [markerPath]() {
            std::ofstream out{markerPath, std::ios::binary | std::ios::trunc};
            out << "expired";
        });
        std::this_thread::sleep_for(std::chrono::minutes{5});
        return 0;  // unreachable: the watchdog gets here first
    }

    testManifestParsing();
    testDiff();
    testBase64();
    testSha256();
    testJournalRoundTrip();
    testPruneSatisfiedOps();
    testWatchdogBudget();
    testWatchdogKillsAStuckRun();
    testRepairRollForward();
    testRepairHalfSwapped();
    testRepairRefusesAStaleTargetWhenTheIncomingVanished();
    testRepairStagedDiscards();
    testRepairRollsBackWhenRollForwardFails();
    testRepairAFileRemovedFromUnderItIsNotSuccess();
    testRepairNoJournal();
    testRepairCorruptJournalPreservesBackups();
    testRepairCorruptJournalNoValuablesCleansUp();
    testArgs();

    if (gFailures == 0) {
        std::cout << "all tests passed\n";
    } else {
        std::cout << gFailures << " failure(s)\n";
    }
    return gFailures;
}
