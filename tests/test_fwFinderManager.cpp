#include <doctest/doctest.h>
#include "device/fwFinderManager.h"

#include <chrono>

using namespace fwog;
using std::chrono::milliseconds;

// ---------------------------------------------------------------------------
// The background scanner's rate policy.
//
// The defect these back: the scan worker used to EXIT once kActiveScanWindowMs
// had passed with no new requestRefresh(), and the only two calls to
// requestRefresh() in the entire app were app startup and the Rescan button.
// Six seconds after launch the thread was gone and the device list froze at
// whatever it had last seen -- a board plugged in afterwards was never noticed,
// and one unplugged still showed as connected, in an app whose whole job is
// reporting which FreeWilis are connected.
//
// The fix keeps the 6-second window and its original meaning ("how long a
// request stays hot") but makes it select a RATE rather than end the thread.
// scanPollIntervalMs() is that policy, extracted so it can be asserted without
// a thread, a board, or a test that sleeps for seconds.
// ---------------------------------------------------------------------------

TEST_CASE("the scanner always has a next poll -- there is no 'and then stop' answer")
{
    // The regression test for the freeze itself. Whatever the age of the last
    // request, including ages far beyond anything the old code survived, the
    // policy still yields a real interval to wait before looking again.
    CHECK(scanPollIntervalMs(milliseconds(0)) > 0);
    CHECK(scanPollIntervalMs(milliseconds(kActiveScanWindowMs)) > 0);
    CHECK(scanPollIntervalMs(milliseconds(60 * 1000)) > 0);
    CHECK(scanPollIntervalMs(std::chrono::hours(8)) > 0);
}

TEST_CASE("a recent request polls fast; an old one drops to the steady-state watch")
{
    CHECK(scanPollIntervalMs(milliseconds(0)) == kFastPollMs);
    CHECK(scanPollIntervalMs(milliseconds(kActiveScanWindowMs - 1)) == kFastPollMs);
    // The boundary is exclusive: at exactly the window length the request has
    // expired.
    CHECK(scanPollIntervalMs(milliseconds(kActiveScanWindowMs)) == kIdlePollMs);
    CHECK(scanPollIntervalMs(std::chrono::hours(8)) == kIdlePollMs);
}

TEST_CASE("scanRequestIsActive agrees with the rate it selects")
{
    // isActivelyScanning() (the device bar's "(scanning...)") and the worker's
    // own sleep are both derived from this predicate, so they cannot disagree
    // about whether a request is still hot.
    CHECK(scanRequestIsActive(milliseconds(0)));
    CHECK(scanRequestIsActive(milliseconds(kActiveScanWindowMs - 1)));
    CHECK_FALSE(scanRequestIsActive(milliseconds(kActiveScanWindowMs)));

    CHECK((scanPollIntervalMs(milliseconds(10)) == kFastPollMs) == scanRequestIsActive(milliseconds(10)));
    CHECK((scanPollIntervalMs(milliseconds(99999)) == kFastPollMs) == scanRequestIsActive(milliseconds(99999)));
}

TEST_CASE("the two rates are chosen so continuous watching stays affordable and still prompt")
{
    // Measured on this project's win-msvc-release build with a FreeWili
    // attached: one Fw::find_all() costs ~160 ms of CPU. The old 100 ms loop
    // therefore burned ~62% of a core whenever it ran, which is why running it
    // forever was never an option and why the idle rate has to be materially
    // slower than the active one.
    CHECK(kIdlePollMs > kFastPollMs);
    // Worst-case hotplug detection latency is one idle interval plus one scan.
    // Anything past a couple of seconds stops feeling like detection at all.
    CHECK(kIdlePollMs <= 2000);
    // A user who just clicked Rescan is waiting; sub-frame-time granularity is
    // wasted, but this has to stay far below the idle rate to be worth having.
    CHECK(kFastPollMs <= 250);
    // The chunked sleep in run() steps by kFastPollMs, so shutdown() and a
    // fresh request are both noticed within that -- which only holds if the
    // idle interval is a whole number of chunks.
    CHECK(kIdlePollMs % kFastPollMs == 0);
}

TEST_CASE("a manager that has never completed a scan reports an UNKNOWN snapshot age, not a fresh one")
{
    // The production half of the flash dialog's fail-closed gate: before any
    // scan has succeeded there is no data whose age could vouch for a device
    // identity, and lastScanAge() must say so rather than returning something
    // that reads as "just scanned". Paired with
    // selectionUnchangedFresh(..., std::nullopt) == StaleSnapshot in
    // test_fwFlashController.cpp, this is what makes an unscanned app refuse
    // instead of approving on unverified data.
    //
    // Reads an atomic and starts nothing: no requestRefresh() is called here,
    // so no worker thread and no device enumeration happen in the test suite.
    CHECK_FALSE(fwFinderManager::instance().isRunning());
    CHECK_FALSE(fwFinderManager::instance().isActivelyScanning());
    CHECK_FALSE(fwFinderManager::instance().lastScanAge().has_value());
}
