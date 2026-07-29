#pragma once

#include <memory>
#include <chrono>
#include <cstdint>
#include <expected>
#include <atomic>
#include <optional>
#include <thread>
#include <mutex>
#include <string>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <fwfinder.hpp>
#endif

#ifdef __EMSCRIPTEN__
// fwfinder is not available on the web. Provide minimal stub types.
namespace Fw {
    struct FreeWiliDevice {};
    using FreeWiliDevices = std::vector<FreeWiliDevice>;
}
#endif

namespace fwog {

/// How long after the most recent requestRefresh() the worker keeps polling at
/// kFastPollMs. This is the ORIGINAL 6-second constant, with its original
/// meaning of "how long a request stays hot" intact -- what changed is what
/// happens when it expires: the worker used to EXIT (leaving the device list
/// frozen forever after, since only app startup and the Rescan button ever
/// re-armed it), and now drops to kIdlePollMs instead. See run()'s comment in
/// the .cpp for the measurements behind the two rates.
inline constexpr uint32_t kActiveScanWindowMs = 6000;

/// Poll interval inside the active window: a user who just clicked Rescan, or
/// a flash dialog that is gating a Proceed button on device identity, is
/// waiting on the answer right now.
inline constexpr uint32_t kFastPollMs = 100;

/// Poll interval outside it -- the steady-state "is anything plugged in or
/// unplugged?" watch that has to run for the app's whole lifetime.
inline constexpr uint32_t kIdlePollMs = 2000;

/// How long Fw::find_all() may keep failing before the failure replaces the
/// last good device list. Below this a transient failure leaves the previous
/// result standing rather than blanking the device bar; above it the user is
/// told. Same value, and the same intent, as the original code's single
/// timeout.
inline constexpr uint32_t kScanFailureGraceMs = 6000;

/// True when a requestRefresh() this recent still counts as "someone is
/// waiting on the answer right now" -- a Rescan click, app startup, or a flash
/// dialog holding its identity gate open.
inline bool scanRequestIsActive(std::chrono::milliseconds sinceLastRequest) noexcept
{
    return sinceLastRequest < std::chrono::milliseconds(kActiveScanWindowMs);
}

/// The interval the background scanner waits between Fw::find_all() calls,
/// given how long ago the most recent requestRefresh() was.
///
/// The whole of the scanner's rate policy, pulled out of run() so it can be
/// stated and tested without a thread or a board. The property that matters
/// most is the one that is easiest to miss: this ALWAYS returns an interval.
/// There is no "and then stop" answer. The scanner used to have one -- it
/// exited kActiveScanWindowMs after the last request, and since only app
/// startup and the Rescan button ever re-armed it, the device list then froze
/// permanently.
inline uint32_t scanPollIntervalMs(std::chrono::milliseconds sinceLastRequest) noexcept
{
    return scanRequestIsActive(sinceLastRequest) ? kFastPollMs : kIdlePollMs;
}

class fwFinderManager {
public:
    // Get the devices
    auto getDevices(bool forceRefresh) noexcept -> std::expected<Fw::FreeWiliDevices, std::string>;

    // request a refresh that is non-blocking
    auto requestRefresh(uint32_t eventType) noexcept -> void;

    // Get the singleton instance of fwFinderManager
    static auto instance() noexcept -> fwFinderManager&;

    // Check to see if the background worker thread exists. NOTE: this is now
    // true for essentially the whole life of the app (the worker no longer
    // exits when idle -- see kActiveScanWindowMs), so it answers "is there a
    // thread to stop?", which is what shutdown() needs, and NOT "are we
    // looking right now?", which is what a UI status line wants. Use
    // isActivelyScanning() for the latter.
    auto isRunning() noexcept -> bool;

    /// True while the worker is inside the kActiveScanWindowMs fast-poll
    /// window opened by the most recent requestRefresh(). This is the
    /// "(scanning...)" the device bar means; outside the window the worker is
    /// still watching, just at kIdlePollMs.
    auto isActivelyScanning() noexcept -> bool;

    /// How long ago the last SUCCESSFUL Fw::find_all() completed, or nullopt
    /// if none ever has. This is the age of the data getDevices() hands back
    /// -- the only thing that can distinguish "the selected board is still
    /// the one that was there" from "nobody has looked since before the user
    /// swapped boards". FlashDialog's identity gate refuses to approve a
    /// flash on a snapshot older than kMaxSnapshotAgeForFlash
    /// (fwFlashController.h) precisely because the second case is
    /// indistinguishable from the first by content alone.
    auto lastScanAge() noexcept -> std::optional<std::chrono::milliseconds>;

    // Stop the background thread and wait until it has fully exited. Safe to
    // call multiple times. Call this BEFORE process shutdown so the worker
    // doesn't get killed mid-call into setupapi/cfgmgr while DLLs unload.
    //
    // TERMINAL: once this has been called, requestRefresh() is a no-op
    // forever after. See its comment in the .cpp -- a re-arm arriving from
    // the UI thread during or after teardown must not resurrect a detached
    // worker that then outlives the process's DLL unload.
    auto shutdown() noexcept -> void;

private:
    fwFinderManager() = default;
    virtual ~fwFinderManager();
    fwFinderManager operator=(const fwFinderManager&) = delete;

#ifndef __EMSCRIPTEN__
    std::mutex devicesResultMutex;
    std::expected<Fw::FreeWiliDevices, std::string> devicesResult;

    std::thread backgroundThread;
    std::atomic<bool> _isRunning = false;
    std::atomic<bool> requestStop = false;
    /// Set by shutdown() and never cleared. Distinct from requestStop, which
    /// requestRefresh() legitimately clears when it starts a worker: this one
    /// is the permanent "the app is going away, never start anything again"
    /// latch.
    std::atomic<bool> shutdownRequested = false;
    /// steady_clock nanoseconds of the most recent requestRefresh(), and of
    /// the most recent successful scan (0 = never). Replaces the old
    /// `resetTimer` bool: the worker needs to know not just THAT a request
    /// arrived but WHEN, so it can size its own poll interval, and
    /// isActivelyScanning() needs the same value without a second flag that
    /// could disagree with it.
    std::atomic<int64_t> lastRequestNs = 0;
    std::atomic<int64_t> lastGoodScanNs = 0;

    auto run() noexcept -> void;
#endif
};

} // namespace fwog
