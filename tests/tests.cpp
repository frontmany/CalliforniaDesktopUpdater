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

#include <windows.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>

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
    return fileEntry("Calls.exe", shaClient, 1) + "," + fileEntry("updater.exe", shaUpdater, 1);
}

void writeFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readFile(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

fs::path freshDir(const std::string& name) {
    const auto dir = fs::temp_directory_path() / "updater-tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
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

    // Regression guard for the Client.exe -> Calls.exe rename: parse must NEVER
    // require a file by name -- that rule shipped inside released binaries and
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
    std::string out;
    CHECK(updater::base64Decode("aGVsbG8=", out) && out == "hello");
    CHECK(updater::base64Decode("", out) && out.empty());
    CHECK(!updater::base64Decode("a", out));
    CHECK(!updater::base64Decode("ab=c", out));
    CHECK(!updater::base64Decode("!!!!", out));
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
        .relaunchExe = "Calls.exe",
        .ops = {{updater::FileOp::Kind::Replace, "a.dll", std::string(64, 'a')},
                {updater::FileOp::Kind::Remove, "b.dll", ""}},
    };
    CHECK(journal.save(dir).has_value());
    const auto loaded = updater::Journal::load(dir);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK(loaded->state == "swapping");
        CHECK(loaded->version == "1.2.3");
        CHECK(loaded->ops.size() == 2);
        CHECK(loaded->ops[1].kind == updater::FileOp::Kind::Remove);
    }
    CHECK(!updater::Journal::load(freshDir("journal-none")).has_value());
}

// --- Apply state machine (no signature: exercised via repair paths) ---------

// Full runApply needs a valid signature; the signing key lives outside tests.
// The swap/rollback/repair mechanics are exercised through Journal + repair,
// which skip signature checks by design (pre-verified before swap).

void testRepairRollForward() {
    const auto install = freshDir("repair-install");
    writeFile(install / "Calls.exe", "old-exe");
    writeFile(install / "stay.dll", "stay");
    writeFile(install / "old.dll", "old");

    // Simulate a crash mid-swap: incoming holds the new files, journal says swapping.
    writeFile(install / ".update" / "incoming" / "Calls.exe", "new-exe");
    writeFile(install / ".update" / "incoming" / "fresh.dll", "fresh");
    writeFile(install / ".update" / "manifest.json", "{\"new\":\"manifest\"}");

    updater::Journal journal{
        .state = "swapping",
        .version = "2.0.0",
        .relaunchExe = "Calls.exe",
        .ops = {{updater::FileOp::Kind::Replace, "Calls.exe", ""},
                {updater::FileOp::Kind::Add, "fresh.dll", ""},
                {updater::FileOp::Kind::Remove, "old.dll", ""}},
    };
    CHECK(journal.save(install).has_value());

    updater::Args args;
    args.mode = updater::Args::Mode::Repair;
    args.installDir = install;
    args.noRelaunch = true;
    CHECK(updater::runRepair(args) == updater::kOk);

    CHECK(readFile(install / "Calls.exe") == "new-exe");
    CHECK(readFile(install / "fresh.dll") == "fresh");
    CHECK(readFile(install / "stay.dll") == "stay");
    CHECK(!fs::exists(install / "old.dll"));
    CHECK(!fs::exists(install / ".update"));
    CHECK(readFile(install / "manifest.json") == "{\"new\":\"manifest\"}");
}

void testRepairHalfSwapped() {
    // Crash after some ops already ran: replace done, add not, remove not.
    const auto install = freshDir("repair-half");
    writeFile(install / "Calls.exe", "new-exe");  // already swapped
    writeFile(install / ".update" / "backup" / "Calls.exe", "old-exe");
    writeFile(install / ".update" / "incoming" / "fresh.dll", "fresh");
    writeFile(install / "old.dll", "old");
    writeFile(install / ".update" / "manifest.json", "m2");

    updater::Journal journal{
        .state = "swapping",
        .version = "2.0.0",
        .relaunchExe = "Calls.exe",
        .ops = {{updater::FileOp::Kind::Replace, "Calls.exe", ""},
                {updater::FileOp::Kind::Add, "fresh.dll", ""},
                {updater::FileOp::Kind::Remove, "old.dll", ""}},
    };
    CHECK(journal.save(install).has_value());

    updater::Args args;
    args.mode = updater::Args::Mode::Repair;
    args.installDir = install;
    args.noRelaunch = true;
    CHECK(updater::runRepair(args) == updater::kOk);

    CHECK(readFile(install / "Calls.exe") == "new-exe");
    CHECK(readFile(install / "fresh.dll") == "fresh");
    CHECK(!fs::exists(install / "old.dll"));
    CHECK(!fs::exists(install / ".update"));
}

void testRepairStagedDiscards() {
    const auto install = freshDir("repair-staged");
    writeFile(install / "Calls.exe", "exe");
    writeFile(install / ".update" / "incoming" / "x.dll", "x");
    updater::Journal journal{
        .state = "staged",
        .version = "2.0.0",
        .relaunchExe = "Calls.exe",
        .ops = {{updater::FileOp::Kind::Add, "x.dll", ""}},
    };
    CHECK(journal.save(install).has_value());

    updater::Args args;
    args.mode = updater::Args::Mode::Repair;
    args.installDir = install;
    args.noRelaunch = true;
    CHECK(updater::runRepair(args) == updater::kOk);
    CHECK(readFile(install / "Calls.exe") == "exe");
    CHECK(!fs::exists(install / ".update"));
    CHECK(!fs::exists(install / "x.dll"));
}

void testRepairNoJournal() {
    const auto install = freshDir("repair-nothing");
    writeFile(install / "Calls.exe", "exe");
    updater::Args args;
    args.mode = updater::Args::Mode::Repair;
    args.installDir = install;
    args.noRelaunch = true;
    CHECK(updater::runRepair(args) == updater::kNothingToDo);
}

void testArgs() {
    using updater::Args;
    const auto ok = Args::parse(
        {L"updater.exe", L"apply", L"--install-dir", L"C:\\x", L"--staging-dir", L"C:\\y",
         L"--parent-pid", L"123", L"--relaunch-exe", L"Calls.exe", L"--no-relaunch"});
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
}

}  // namespace

int main() {
    testManifestParsing();
    testDiff();
    testBase64();
    testSha256();
    testJournalRoundTrip();
    testRepairRollForward();
    testRepairHalfSwapped();
    testRepairStagedDiscards();
    testRepairNoJournal();
    testArgs();

    if (gFailures == 0) {
        std::cout << "all tests passed\n";
    } else {
        std::cout << gFailures << " failure(s)\n";
    }
    return gFailures;
}
