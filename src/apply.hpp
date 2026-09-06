#pragma once

#include "args.hpp"
#include "manifest.hpp"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace updater {

/// Exit codes (the client surfaces these via last-apply.json):
enum ExitCode : int {
    kOk = 0,
    kBadArgs = 2,        // bad CLI / another updater instance running
    kParentAlive = 3,    // app never exited / exe stayed locked
    kVerifyFailed = 4,   // signature or staged-file hash mismatch
    kNotWritable = 5,    // install dir not writable
    kSwapFailedRolledBack = 6,
    kSwapFailedBroken = 7,  // rollback also failed; journal left for repair
    kNothingToDo = 8,       // repair: no journal found
    // Never returned: the watchdog terminates the process with it. A run that
    // used up its whole budget cannot be trusted to unwind cleanly, which is
    // the point of killing it rather than asking it to stop.
    kTimedOut = 9,
};

/// Drops the ops whose staged file is missing but whose target in the install
/// already hashes to what the manifest asks for. Returns how many were dropped.
///
/// The client stages what its own diff asked for; the updater plans a second,
/// independent diff. They agree — unless the local manifest is unreadable here,
/// where the plan becomes a full reinstall of the whole release against a
/// staging directory holding only the files that changed. That disagreement
/// used to fail the apply outright and identically on every retry.
std::size_t pruneSatisfiedOps(std::vector<FileOp>& ops,
                              const std::filesystem::path& stagingDir,
                              const std::filesystem::path& installDir);

/// apply: wait for the app to exit, verify staging, stage locally, swap,
/// commit, relaunch. repair: roll an interrupted apply forward, or put the
/// previous version back when it cannot.
[[nodiscard]] int runApply(const Args& args);
[[nodiscard]] int runRepair(const Args& args);

}  // namespace updater
