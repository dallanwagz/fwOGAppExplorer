#include "flash/fwFlashPrep.h"

#include <algorithm>

namespace fwog {

bool planWritesMainFirmware(std::span<const FlashStep> plan)
{
    return std::any_of(plan.begin(), plan.end(), [](const FlashStep& s) {
        return s.cpu == TargetCpu::Main && s.action == StepAction::Write;
    });
}

PrepResult quietDisplayBeforeMainWrite(const FlashIo& io, std::span<const FlashStep> plan,
                                       const ProgressFn& progress)
{
    PrepResult r;
    if (!planWritesMainFirmware(plan)) return r;

    CpuIdentity identity = io.identify();
    // Already quiet: a DISPLAY that presents a drive of its own is sitting in
    // the bootrom, which is exactly where this would put it. And a DISPLAY
    // with no port cannot be touched at all -- proceed, as the plan always did.
    if (identity.displayVolume || !identity.displayPort) return r;

    const std::string port = *identity.displayPort;
    const size_t n = plan.size();
    const auto report = [&](std::string message, bool refresh, int elapsed) {
        if (!progress) return;
        FlashProgress p;
        p.phase = FlashPhase::Preparing;
        p.stepIndex = 0;
        p.stepCount = n;
        p.cpu = TargetCpu::Display;
        p.message = std::move(message);
        p.isRefresh = refresh;
        p.waitElapsedMs = elapsed;
        p.waitTotalMs = refresh ? kVolumeWaitMs : 0;
        progress(p);
    };

    report("rebooting the DISPLAY CPU (" + port + ") into BOOTSEL first, so the MAIN "
           "write is not disturbed by the display re-enumerating mid-copy", false, 0);
    io.touchPort(port);

    // The same wait the engine gives a touched CPU, watched through the LIVE
    // identity rather than the raw volume list: the thing that matters is not
    // "a drive appeared" but "a drive appeared AT THE DISPLAY'S HUB PORT", which
    // is what the plan's steps will read and leave alone.
    const std::string waitingMsg = "waiting for the DISPLAY CPU's RPI-RP2 drive";
    report(waitingMsg, false, 0);
    for (int waited = 0; waited < kVolumeWaitMs; ) {
        if (!io.waitTick(kVolumePollMs)) {
            r.outcome = PrepOutcome::Cancelled;
            r.message = "cancelled.";
            return r;
        }
        waited += kVolumePollMs;
        report(waitingMsg, true, waited);
        identity = io.identify();
        if (identity.displayVolume) {
            r.outcome = PrepOutcome::Quieted;
            r.message = "the DISPLAY CPU is in BOOTSEL (" + *identity.displayVolume +
                        "); it will be left alone, and the new MAIN firmware resets it "
                        "when it boots";
            report(r.message, false, 0);
            return r;
        }
    }

    r.outcome = PrepOutcome::NotQuieted;
    r.message = "the DISPLAY CPU did not come back as an RPI-RP2 drive after the reboot; "
                "going ahead without it quiet (the MAIN write may be less stable)";
    report(r.message, false, 0);
    return r;
}

} // namespace fwog
