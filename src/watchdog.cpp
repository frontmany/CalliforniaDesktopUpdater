#include "watchdog.hpp"

#include "apply.hpp"
#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

namespace updater::watchdog {
namespace {

// Kept back from the budget so a swap that runs long is not killed halfway:
// rolling the old files back and starting the app again is what the last
// stretch is for. Released explicitly once rollback begins.
constexpr auto kReserve = std::chrono::seconds{75};

// How long `onExpire` gets before the process is killed regardless. It writes
// one small file and starts one process; a minute is generous, and a callback
// that needs longer is itself the stall this file exists to end.
constexpr auto kExpiryGrace = std::chrono::seconds{60};

std::mutex gMutex;
std::condition_variable gCancelled;
// Bumped by every arm() and disarm(); a watchdog thread whose generation is no
// longer current has been called off. Cheaper and race-free where an event
// handle would have to be closed by one thread while another may still wait on
// it.
std::uint64_t gGeneration = 0;

std::atomic<Phase> gPhase{Phase::Startup};
std::atomic<bool> gReserveReleased{false};
std::atomic<long long> gDeadlineTicks{0};  // steady_clock ticks; 0 = not armed

[[nodiscard]] std::chrono::milliseconds untilDeadline() {
    const auto ticks = gDeadlineTicks.load(std::memory_order_relaxed);
    if (ticks == 0) {
        return std::chrono::milliseconds::max();
    }
    const std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::duration{ticks}};
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return std::max(left, std::chrono::milliseconds{0});
}

/// Terminate rather than exit: every handler on the way out of a normal exit is
/// another chance to block, and by this point the process has nothing left to
/// flush — the log is written and flushed line by line and last-apply.json is
/// already on disk.
[[noreturn]] void killSelf() {
    TerminateProcess(GetCurrentProcess(), kTimedOut);
    for (;;) {  // TerminateProcess does not return; the compiler cannot know that
        Sleep(1000);
    }
}

}  // namespace

const char* toString(const Phase phase) {
    switch (phase) {
    case Phase::Startup: return "starting up";
    case Phase::WaitParent: return "waiting for the app to close";
    case Phase::Preflight: return "checking the install folder";
    case Phase::Verify: return "verifying the update";
    case Phase::UnlockWait: return "waiting for files to be released";
    case Phase::StageLocal: return "copying the new files in";
    case Phase::Swap: return "replacing files";
    case Phase::Rollback: return "putting the previous version back";
    case Phase::Commit: return "finishing up";
    case Phase::Relaunch: return "starting the app";
    }
    return "an unknown step";
}

void setPhase(const Phase phase) {
    gPhase.store(phase, std::memory_order_relaxed);
    log::info("phase: {}", toString(phase));
}

Phase phase() {
    return gPhase.load(std::memory_order_relaxed);
}

void arm(const std::chrono::milliseconds budget, std::function<void()> onExpire) {
    std::uint64_t generation = 0;
    {
        const std::scoped_lock lock{gMutex};
        generation = ++gGeneration;
    }
    gCancelled.notify_all();  // call off any watchdog left from an earlier run
    gReserveReleased.store(false, std::memory_order_relaxed);
    gDeadlineTicks.store((std::chrono::steady_clock::now() + budget).time_since_epoch().count(),
                         std::memory_order_relaxed);
    log::info("watchdog armed for {} s",
              std::chrono::duration_cast<std::chrono::seconds>(budget).count());

    std::thread([budget, generation, expire = std::move(onExpire)]() {
        {
            std::unique_lock lock{gMutex};
            if (gCancelled.wait_for(lock, budget,
                                    [generation] { return gGeneration != generation; })) {
                return;  // disarmed, or superseded by a later run
            }
        }
        log::error("watchdog: {} s budget exhausted while \"{}\"; stopping",
                   std::chrono::duration_cast<std::chrono::seconds>(budget).count(),
                   toString(phase()));
        log::flushAll();

        // The callback gets a timer of its own: if recording the failure is
        // what is stuck, the process still dies on schedule.
        std::thread([]() {
            std::this_thread::sleep_for(kExpiryGrace);
            killSelf();
        }).detach();

        if (expire) {
            expire();
        }
        log::flushAll();
        killSelf();
    }).detach();
}

void disarm() {
    {
        const std::scoped_lock lock{gMutex};
        ++gGeneration;
    }
    gCancelled.notify_all();
    gDeadlineTicks.store(0, std::memory_order_relaxed);
    gReserveReleased.store(false, std::memory_order_relaxed);
}

std::chrono::milliseconds remaining() {
    return untilDeadline();
}

std::chrono::milliseconds workBudget() {
    const auto left = untilDeadline();
    if (left == std::chrono::milliseconds::max() ||
        gReserveReleased.load(std::memory_order_relaxed)) {
        return left;
    }
    return left > kReserve ? left - kReserve : std::chrono::milliseconds{0};
}

void releaseReserve() {
    gReserveReleased.store(true, std::memory_order_relaxed);
}

bool expired() {
    return untilDeadline() == std::chrono::milliseconds{0};
}

}  // namespace updater::watchdog
