#include "device/fwFinderManager.h"
#include <cassert>
#include <chrono>

namespace fwog {

#ifdef __EMSCRIPTEN__
// fwfinder is not available on the web. Provide no-op stubs.
fwFinderManager::~fwFinderManager() {}
auto fwFinderManager::shutdown() noexcept -> void {}
auto fwFinderManager::isRunning() noexcept -> bool { return false; }
auto fwFinderManager::isActivelyScanning() noexcept -> bool { return false; }
auto fwFinderManager::lastScanAge() noexcept -> std::optional<std::chrono::milliseconds> {
    // Never scanned, and never will be. Reporting "unknown age" rather than a
    // fresh-looking zero is what keeps every freshness gate downstream failing
    // closed on the web build instead of being handed a lie.
    return std::nullopt;
}
auto fwFinderManager::requestRefresh(uint32_t) noexcept -> void {}
auto fwFinderManager::getDevices(bool) noexcept -> std::expected<Fw::FreeWiliDevices, std::string> {
    return Fw::FreeWiliDevices{};
}
auto fwFinderManager::instance() noexcept -> fwFinderManager& {
    static fwFinderManager s; return s;
}
#else

namespace {

int64_t steadyNowNs() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::chrono::milliseconds nsToMs(int64_t ns) noexcept
{
    return std::chrono::milliseconds(ns > 0 ? ns / 1000000 : 0);
}

} // namespace

fwFinderManager::~fwFinderManager() {
    shutdown();
}

auto fwFinderManager::shutdown() noexcept -> void {
    // Order matters: shutdownRequested BEFORE requestStop, and both before the
    // spin. requestRefresh() re-checks shutdownRequested AFTER it has claimed
    // the worker slot (_isRunning false -> true), so either
    //   - it claims the slot before we read _isRunning here, in which case we
    //     see isRunning() == true and wait for it to release the slot; or
    //   - it claims the slot after, in which case it observes the flag we set
    //     here and releases the slot without spawning anything.
    // Neither ordering leaves a detached worker running past this function.
    shutdownRequested = true;
    requestStop = true;
    while (isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!isRunning());
}

// Get the devices
auto fwFinderManager::getDevices(bool forceRefresh) noexcept -> std::expected<Fw::FreeWiliDevices, std::string> {
    if (forceRefresh) {
        requestRefresh(0);
    }
    // The unlocked fast path that used to live in the `!isRunning()` branch
    // was removed deliberately: it read devicesResult without the mutex,
    // which was only safe under a single-caller assumption. A UI thread
    // polling every frame while the worker writes would race on it. Take
    // the lock unconditionally -- one mutex, no nesting, so this cannot
    // deadlock, and it returns a copy either way, so this cannot change
    // observable behaviour beyond making the read consistent.
    std::lock_guard<std::mutex> _lock(devicesResultMutex);
    return devicesResult;
}

// request a refresh that is non-blocking
auto fwFinderManager::requestRefresh(uint32_t eventType) noexcept -> void {
    // TODO: We can probably do something with this later if we need
    (void)eventType;

    // Cheap early out for the common post-teardown case; the authoritative
    // check is the one below, after the slot has been claimed.
    if (shutdownRequested) return;

    // Recorded unconditionally, including when a worker is already running:
    // this timestamp is both the worker's "stay at kFastPollMs until" deadline
    // and what isActivelyScanning() reports on. It replaces the old
    // `resetTimer` flag, which could only say "something happened" and not
    // when.
    lastRequestNs.store(steadyNowNs());

    // compare_exchange, not `if (!isRunning()) _isRunning = true;`: that
    // read-then-write let two callers both see false and both spawn a worker,
    // leaving one of them permanently unaccounted for by shutdown()'s spin
    // (which waits on a single flag). Only one caller can win the exchange.
    bool expected = false;
    if (!_isRunning.compare_exchange_strong(expected, true)) {
        return;   // a worker is already up; the timestamp above is the re-arm
    }

    // Slot claimed. requestStop is cleared FIRST and the terminal latch is
    // re-read AFTER, and that order is load-bearing: with both accesses
    // sequentially consistent, a shutdown() that got its `requestStop = true`
    // in ahead of this line must also have set shutdownRequested ahead of the
    // read below, so the read cannot miss it. The other order would admit an
    // interleaving where we clear the stop flag after shutdown() set it and
    // then still spawn -- a worker that never stops, and a shutdown() spinning
    // on isRunning() forever.
    requestStop = false;
    if (shutdownRequested) {
        _isRunning = false;
        return;
    }

    backgroundThread = std::thread(&fwFinderManager::run, this);
    backgroundThread.detach();
}

auto fwFinderManager::instance() noexcept -> fwFinderManager& {
    static fwFinderManager instance;
    return instance;
}

auto fwFinderManager::isRunning() noexcept -> bool {
    return _isRunning;
}

auto fwFinderManager::isActivelyScanning() noexcept -> bool {
    if (!_isRunning) return false;
    // The same predicate the worker sizes its own sleep with, so the status
    // line and the actual poll rate can never disagree.
    return scanRequestIsActive(nsToMs(steadyNowNs() - lastRequestNs.load()));
}

auto fwFinderManager::lastScanAge() noexcept -> std::optional<std::chrono::milliseconds> {
    const int64_t last = lastGoodScanNs.load();
    if (last == 0) return std::nullopt;   // no successful scan has ever completed
    return nsToMs(steadyNowNs() - last);
}

// The background scan loop. Runs until shutdown(); it deliberately does NOT
// exit when the kActiveScanWindowMs request window expires.
//
// It used to. That was the whole defect: six seconds after launch (or after the
// last Rescan click) the thread went away, and nothing but app startup and the
// Rescan button ever called requestRefresh(), so the device list froze at
// whatever it had last seen. A board plugged in after that was never noticed,
// and one unplugged still showed as connected -- in an app whose entire job is
// telling the user which FreeWilis are connected.
//
// TWO RATES rather than one, because the cost is real and was measured rather
// than guessed. On this machine (win-msvc-release, a FreeWili OG attached), the
// app draws ~28% of one core just rendering at vsync; the old 100 ms scan loop
// added ~62% of a core on top of that, which works out at roughly 160 ms of CPU
// per Fw::find_all() call. So:
//   - kFastPollMs (100 ms) only inside the kActiveScanWindowMs window after a
//     requestRefresh(). Someone is waiting on the answer: they clicked Rescan,
//     the app just started, or a flash dialog is gating its Proceed button on
//     device identity and holds the window open for as long as it is on screen.
//   - kIdlePollMs (2000 ms) the rest of the time: ~160 ms of CPU per 2.16 s,
//     ~7% of a core -- comfortably below what the app's own idle rendering
//     already costs -- for a worst-case hotplug-detection latency of ~2.2 s.
//
// The rejected alternative was to keep the exit and have the UI re-arm the
// worker on a timer. It cannot be made to work: the burst is 6 s long at 10 Hz,
// so a re-arm interval short enough for a decent detection latency (1-2 s, as
// first proposed) yields a ~75-86% duty cycle -- ~55% of a core, permanently,
// which is the "immortal 100 ms loop" outcome under another name -- and an
// interval long enough to bound the cost (60 s+) puts hotplug detection nearly a
// minute behind reality. Adjusting the RATE bounds both; adjusting the thread's
// lifetime bounds neither. It also spawns a thread every few seconds forever,
// where this spawns one.
auto fwFinderManager::run() noexcept -> void {
    // The failure run's start, so a transient Fw::find_all() failure does not
    // immediately blank a good device list. The original code hung this off the
    // request timer; hanging it off the failures themselves is what it was
    // always trying to express, and it now self-heals (a later success clears
    // it) instead of killing the thread.
    int64_t failingSinceNs = 0;

    while (!requestStop) {
        if (auto result = Fw::find_all(); result.has_value()) {
            failingSinceNs = 0;
            // Lets update, we have a result. This allows us to keep the old value
            // until we have a good result
            {
                std::lock_guard<std::mutex> _lock(devicesResultMutex);
                devicesResult = result.value();
            }
            // AFTER the store, never before: lastScanAge() is the age of the
            // data a reader can actually see, and a freshness gate built on a
            // timestamp published ahead of the data it describes would approve
            // a flash against the previous snapshot.
            lastGoodScanNs.store(steadyNowNs());
        } else {
            const int64_t now = steadyNowNs();
            if (failingSinceNs == 0) failingSinceNs = now;
            if (nsToMs(now - failingSinceNs) >= std::chrono::milliseconds(kScanFailureGraceMs)) {
                std::lock_guard<std::mutex> _lock(devicesResultMutex);
                devicesResult = std::unexpected(result.error());
            }
        }

        // Read AFTER the scan, not before it: Fw::find_all() takes ~160 ms, and
        // a request that arrived during it (the Rescan button, or a flash
        // dialog opening) should be honoured by THIS iteration's rate rather
        // than waiting out a two-second sleep first.
        const int64_t requestedAt = lastRequestNs.load();
        const uint32_t interval = scanPollIntervalMs(nsToMs(steadyNowNs() - requestedAt));

        // Chunked rather than one sleep_for(interval): a kIdlePollMs sleep
        // would otherwise delay shutdown() by up to two seconds on every exit,
        // and would sit on a fresh requestRefresh() (a Rescan click, or the
        // flash dialog opening) for just as long. Waking every kFastPollMs
        // costs nothing and bounds both to that.
        for (uint32_t slept = 0; slept < interval; slept += kFastPollMs) {
            if (requestStop) break;
            // A new request cuts the wait short ONLY when it would change the
            // rate -- i.e. only out of the idle interval. The `interval !=
            // kFastPollMs` half is not an optimisation: FlashDialog re-stamps
            // the request EVERY FRAME while its identity gate is on screen
            // (deliberately -- see its comment), so a plain "did the stamp
            // change?" test would break out of every fast-mode sleep before it
            // slept at all and spin Fw::find_all() flat out, at ~160 ms of CPU
            // a call. There is nothing to switch to when already fast.
            if (interval != kFastPollMs && lastRequestNs.load() != requestedAt) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(kFastPollMs));
        }
    }
    _isRunning = false;
}

#endif // !__EMSCRIPTEN__

} // namespace fwog
