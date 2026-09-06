#pragma once

#include <chrono>
#include <functional>

namespace updater::watchdog {

/// Which stage of a run is in progress. Recorded in the log and in
/// last-apply.json so a run that stalls or is killed says where it stopped --
/// the updater has no window and no console, so without this a failed apply is
/// indistinguishable from a hang, and there is nothing to go on afterwards.
enum class Phase {
    Startup,
    WaitParent,
    Preflight,
    Verify,
    UnlockWait,
    StageLocal,
    Swap,
    Rollback,
    Commit,
    Relaunch,
};

[[nodiscard]] const char* toString(Phase phase);

void setPhase(Phase phase);
[[nodiscard]] Phase phase();

/// Starts the hard deadline for this run. When `budget` runs out, `onExpire`
/// is called (to record why and put the app back on screen) and the process is
/// terminated -- not asked to exit. Every wait, retry and backoff in this
/// binary is individually bounded, but "bounded" times a few thousand file
/// operations is still hours, and any future stall this file does not know
/// about would be unbounded. This is the one guarantee that does not depend on
/// having predicted the failure: the updater always stops.
///
/// A second timer covers `onExpire` itself, so a callback that blocks (a log
/// write to a wedged disk, a mutex held by the stuck thread) cannot keep the
/// process alive either.
void arm(std::chrono::milliseconds budget, std::function<void()> onExpire);

/// Stops the watchdog. Call before returning normally.
void disarm();

/// Whole time left before the process is killed.
[[nodiscard]] std::chrono::milliseconds remaining();

/// Time left for ordinary work: `remaining()` minus a reserve kept back so a
/// swap that runs long still has room to roll back and relaunch the app rather
/// than being killed halfway. Retry loops must spend this, not `remaining()`.
[[nodiscard]] std::chrono::milliseconds workBudget();

/// Hands the reserve over to the caller -- rollback and relaunch are what it
/// was being kept for, so they get to spend it.
void releaseReserve();

/// True once the budget is gone. Nothing can run after this for long.
[[nodiscard]] bool expired();

}  // namespace updater::watchdog
