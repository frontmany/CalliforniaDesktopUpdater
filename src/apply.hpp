#pragma once

#include "args.hpp"

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
};

/// apply: wait for the app to exit, verify staging, stage locally, swap,
/// commit, relaunch. repair: roll an interrupted apply forward (or clean up).
[[nodiscard]] int runApply(const Args& args);
[[nodiscard]] int runRepair(const Args& args);

}  // namespace updater
