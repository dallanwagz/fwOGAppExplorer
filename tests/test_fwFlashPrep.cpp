#include <doctest/doctest.h>

#include "flash/fwFlashPrep.h"

#include <string>
#include <vector>

using namespace fwog;

namespace {

/// A board whose identity changes when its ports are touched, the way a real
/// one does: touching the DISPLAY port drops it and raises a DISPLAY drive
/// after `driveAfterTicks` ticks. Everything else is inert.
struct PrepHarness {
    CpuIdentity identity;
    int driveAfterTicks = 1;   ///< -1: the drive never appears
    int cancelAtTick = -1;     ///< absolute tick at which waitTick reports a cancel

    std::vector<std::string> touched;
    std::vector<FlashProgress> reports;
    int ticks = 0;
    int touchTick = -1;

    FlashIo io() {
        FlashIo io;
        io.identify = [this] { return identity; };
        io.touchPort = [this](const std::string& p) { touched.push_back(p); touchTick = ticks; };
        io.waitTick = [this](int) {
            ++ticks;
            if (cancelAtTick >= 0 && ticks >= cancelAtTick) return false;
            if (touchTick >= 0 && driveAfterTicks >= 0 && ticks - touchTick >= driveAfterTicks
                && identity.displayPort) {
                identity.displayPort.reset();
                identity.displayVolume = "G:/";
                identity.displaySource = IdentitySource::HubLocation;
            }
            return true;
        };
        io.findVolumes = [] { return std::vector<std::string>{}; };
        return io;
    }
    ProgressFn progress() { return [this](const FlashProgress& p) { reports.push_back(p); }; }
};

FlashStep stepFor(TargetCpu cpu, StepAction action = StepAction::Write) {
    FlashStep s;
    s.cpu = cpu;
    s.action = action;
    s.image.embeddedId = "img";
    s.description = "test";
    return s;
}

CpuIdentity bothRunning() {
    CpuIdentity id;
    id.mainPort = "COM10";  id.mainSource = IdentitySource::HubLocation;
    id.displayPort = "COM11"; id.displaySource = IdentitySource::HubLocation;
    return id;
}

} // namespace

TEST_CASE("planWritesMainFirmware is true only for a MAIN Write step")
{
    CHECK(planWritesMainFirmware(std::vector<FlashStep>{ stepFor(TargetCpu::Main) }));
    CHECK(planWritesMainFirmware(std::vector<FlashStep>{ stepFor(TargetCpu::Display),
                                                        stepFor(TargetCpu::Main) }));
    CHECK_FALSE(planWritesMainFirmware(std::vector<FlashStep>{ stepFor(TargetCpu::Display) }));
    // An ERASE of MAIN is not an install -- see the header for why that matters.
    CHECK_FALSE(planWritesMainFirmware(std::vector<FlashStep>{ stepFor(TargetCpu::Main, StepAction::Erase) }));
    CHECK_FALSE(planWritesMainFirmware(std::vector<FlashStep>{}));
}

TEST_CASE("quietDisplayBeforeMainWrite touches the DISPLAY port and waits for its drive")
{
    PrepHarness h;
    h.identity = bothRunning();
    const std::vector<FlashStep> plan{ stepFor(TargetCpu::Main) };

    const PrepResult r = quietDisplayBeforeMainWrite(h.io(), plan, h.progress());

    CHECK(r.outcome == PrepOutcome::Quieted);
    REQUIRE(h.touched.size() == 1);
    CHECK(h.touched.front() == "COM11");            // the DISPLAY port, never MAIN's
    CHECK(h.identity.displayVolume == "G:/");
    CHECK(r.message.find("G:/") != std::string::npos);
    // Every report is Preparing, none claims a step got anywhere.
    REQUIRE_FALSE(h.reports.empty());
    for (const auto& p : h.reports) {
        CHECK(p.phase == FlashPhase::Preparing);
        CHECK(p.stepIndex == 0);
        CHECK(p.stepCount == 1);
        CHECK(flashProgressFraction(p) == 0.0f);
    }
}

TEST_CASE("quietDisplayBeforeMainWrite does nothing when the plan installs no MAIN firmware")
{
    PrepHarness h;
    h.identity = bothRunning();
    SUBCASE("display-only plan") {
        const std::vector<FlashStep> plan{ stepFor(TargetCpu::Display) };
        CHECK(quietDisplayBeforeMainWrite(h.io(), plan, h.progress()).outcome == PrepOutcome::NotNeeded);
    }
    SUBCASE("erase-MAIN-only plan: the erase exists to let the DISPLAY console appear") {
        const std::vector<FlashStep> plan{ stepFor(TargetCpu::Main, StepAction::Erase) };
        CHECK(quietDisplayBeforeMainWrite(h.io(), plan, h.progress()).outcome == PrepOutcome::NotNeeded);
    }
    CHECK(h.touched.empty());
    CHECK(h.reports.empty());
}

TEST_CASE("quietDisplayBeforeMainWrite does nothing when the DISPLAY is not running or already quiet")
{
    const std::vector<FlashStep> plan{ stepFor(TargetCpu::Main) };
    SUBCASE("no DISPLAY port at all") {
        PrepHarness h;
        h.identity.mainPort = "COM10";
        CHECK(quietDisplayBeforeMainWrite(h.io(), plan, h.progress()).outcome == PrepOutcome::NotNeeded);
        CHECK(h.touched.empty());
    }
    SUBCASE("DISPLAY already presents a drive") {
        PrepHarness h;
        h.identity.mainPort = "COM10";
        h.identity.displayVolume = "G:/";
        h.identity.displaySource = IdentitySource::HubLocation;
        CHECK(quietDisplayBeforeMainWrite(h.io(), plan, h.progress()).outcome == PrepOutcome::NotNeeded);
        CHECK(h.touched.empty());
    }
}

TEST_CASE("quietDisplayBeforeMainWrite gives up without failing when no DISPLAY drive appears")
{
    PrepHarness h;
    h.identity = bothRunning();
    h.driveAfterTicks = -1;
    const std::vector<FlashStep> plan{ stepFor(TargetCpu::Main) };

    const PrepResult r = quietDisplayBeforeMainWrite(h.io(), plan, h.progress());

    CHECK(r.outcome == PrepOutcome::NotQuieted);
    CHECK(h.touched.size() == 1);
    CHECK(h.ticks == kVolumeWaitMs / kVolumePollMs);   // the full budget, no more
    CHECK(r.message.find("going ahead") != std::string::npos);
}

TEST_CASE("quietDisplayBeforeMainWrite reports a cancel as Cancelled")
{
    PrepHarness h;
    h.identity = bothRunning();
    h.driveAfterTicks = 50;
    h.cancelAtTick = 3;
    const std::vector<FlashStep> plan{ stepFor(TargetCpu::Main) };

    CHECK(quietDisplayBeforeMainWrite(h.io(), plan, h.progress()).outcome == PrepOutcome::Cancelled);
    CHECK(h.ticks == 3);
}
