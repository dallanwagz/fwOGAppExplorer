#include <doctest/doctest.h>
#include "flash/fwFlashController.h"

#include <chrono>
#include <optional>
#include <thread>

using namespace fwog;

// ---------------------------------------------------------------------------
// recoveryAnchorFor
//
// Deviates from the task brief's literal single-argument
// recoveryAnchorFor(FlashOutcome). RefusedUnidentified and Timeout are
// produced identically whichever CPU's step failed (see FlashOutcome's
// comment in fwFlashEngine.h and RecoveryAnchor's comment in
// fwRecoveryContent.h), so a single-argument function could only ever point
// at one CPU's Recovery section -- wrong for the other every time, and
// actively misleading for a MAIN-side failure (no BOOTSEL-button advice
// applies to MAIN, and the DISPLAY section's 1200-baud/FWOG_DIAG advice does
// not apply to it). fwFlashController.h's recoveryAnchorFor therefore takes
// the failing CPU explicitly; see its header comment for the full
// rationale.
//
// Final review (Fix 2): the function also takes the step counts now. A plan
// that stopped PART-WAY through a multi-step plan maps to PartialLegacyFlash
// ahead of every per-outcome answer below -- see its header comment. The
// single-step / step-0 cases below therefore pass (0, 1): nothing completed,
// one step in the plan, which is what every one of these scenarios was
// always about.
// ---------------------------------------------------------------------------

TEST_CASE("ambiguous volumes map to the two-volumes section regardless of CPU")
{
    CHECK(recoveryAnchorFor(FlashOutcome::RefusedAmbiguous, TargetCpu::Main, 0, 1)
          == RecoveryAnchor::TwoVolumes);
    CHECK(recoveryAnchorFor(FlashOutcome::RefusedAmbiguous, TargetCpu::Display, 0, 1)
          == RecoveryAnchor::TwoVolumes);
}

TEST_CASE("an unidentified MAIN CPU maps to the MAIN section, not DISPLAY")
{
    // The case a single-argument mapping would get wrong: unconditionally
    // landing a MAIN-side failure on DisplayNotIdentified tells the user
    // things that are not true of their situation.
    CHECK(recoveryAnchorFor(FlashOutcome::RefusedUnidentified, TargetCpu::Main, 0, 1)
          == RecoveryAnchor::MainNotIdentified);
    CHECK(recoveryAnchorFor(FlashOutcome::Timeout, TargetCpu::Main, 0, 1)
          == RecoveryAnchor::MainNotIdentified);
}

TEST_CASE("an unidentified DISPLAY CPU maps to the DISPLAY section")
{
    CHECK(recoveryAnchorFor(FlashOutcome::RefusedUnidentified, TargetCpu::Display, 0, 1)
          == RecoveryAnchor::DisplayNotIdentified);
    CHECK(recoveryAnchorFor(FlashOutcome::Timeout, TargetCpu::Display, 0, 1)
          == RecoveryAnchor::DisplayNotIdentified);
}

TEST_CASE("a bad or unverified image maps to the image-rejected section")
{
    CHECK(recoveryAnchorFor(FlashOutcome::BadImage, TargetCpu::Main, 0, 1)
          == RecoveryAnchor::ImageRejected);
    CHECK(recoveryAnchorFor(FlashOutcome::VerifyFailed, TargetCpu::Display, 0, 1)
          == RecoveryAnchor::ImageRejected);
}

TEST_CASE("a failed copy maps to the write-interrupted section")
{
    CHECK(recoveryAnchorFor(FlashOutcome::CopyFailed, TargetCpu::Main, 0, 1)
          == RecoveryAnchor::WriteInterrupted);
    CHECK(recoveryAnchorFor(FlashOutcome::CopyFailed, TargetCpu::Display, 0, 1)
          == RecoveryAnchor::WriteInterrupted);
}

TEST_CASE("outcomes with nothing to explain link to no Recovery section")
{
    CHECK_FALSE(recoveryAnchorFor(FlashOutcome::Success, TargetCpu::Main, 1, 1).has_value());
    CHECK_FALSE(recoveryAnchorFor(FlashOutcome::Aborted, TargetCpu::Main, 0, 1).has_value());
    CHECK_FALSE(recoveryAnchorFor(FlashOutcome::NeedsConfirmation, TargetCpu::Main, 0, 1).has_value());
    CHECK_FALSE(recoveryAnchorFor(FlashOutcome::InvalidArgument, TargetCpu::Main, 0, 1).has_value());
}

// ---------------------------------------------------------------------------
// Final review (Fix 2): a plan that stopped part-way through.
//
// RecoveryAnchor::PartialLegacyFlash used to be returned for NO outcome at
// all -- the Recovery section written for exactly this state was reachable
// only by browsing to it. The realistic path it is written for: an Original
// FreeWili (LegacyDirect) install of [MAIN, DISPLAY] whose step 0 SUCCEEDS
// (which is what destroys the display bootloader) and whose step 1 then
// refuses because the DISPLAY port could not be identified -- the likely
// outcome, since the firmware just installed publishes no FWOG_* product
// string and a running MAIN CPU suppresses the display console entirely.
// ---------------------------------------------------------------------------

TEST_CASE("a mid-plan refusal maps to the partial-install section, not the per-outcome one")
{
    // Without this, the user is sent to "The DISPLAY CPU could not be
    // identified", which tells them to install the bootloader from the Default
    // Firmware tab and never mentions that the one they had is gone.
    CHECK(recoveryAnchorFor(FlashOutcome::RefusedUnidentified, TargetCpu::Display, 1, 2)
          == RecoveryAnchor::PartialLegacyFlash);
    CHECK(recoveryAnchorFor(FlashOutcome::Timeout, TargetCpu::Display, 1, 2)
          == RecoveryAnchor::PartialLegacyFlash);
    CHECK(recoveryAnchorFor(FlashOutcome::RefusedAmbiguous, TargetCpu::Display, 1, 2)
          == RecoveryAnchor::PartialLegacyFlash);
    CHECK(recoveryAnchorFor(FlashOutcome::CopyFailed, TargetCpu::Display, 1, 2)
          == RecoveryAnchor::PartialLegacyFlash);
    CHECK(recoveryAnchorFor(FlashOutcome::BadImage, TargetCpu::Display, 1, 2)
          == RecoveryAnchor::PartialLegacyFlash);
}

TEST_CASE("cancelling mid-plan is a partial install; cancelling before anything was written is not")
{
    // fwApp.cpp's window-close guard already says a LegacyDirect plan aborted
    // between its two steps leaves exactly this state. Aborting on step 0 is
    // still just the user's own choice with nothing to explain -- the flash
    // dialog's muted "Cancelled." styling depends on getting no anchor there.
    CHECK(recoveryAnchorFor(FlashOutcome::Aborted, TargetCpu::Display, 1, 2)
          == RecoveryAnchor::PartialLegacyFlash);
    CHECK_FALSE(recoveryAnchorFor(FlashOutcome::Aborted, TargetCpu::Main, 0, 2).has_value());
}

TEST_CASE("a failure on the LAST step of a plan is not a partial install")
{
    // stepsCompleted == stepCount means every step ran; there is no half-
    // applied plan to explain, so the per-outcome section is still the right
    // place to send the user.
    CHECK(recoveryAnchorFor(FlashOutcome::Timeout, TargetCpu::Display, 2, 2)
          == RecoveryAnchor::DisplayNotIdentified);
    CHECK(recoveryAnchorFor(FlashOutcome::CopyFailed, TargetCpu::Main, 1, 1)
          == RecoveryAnchor::WriteInterrupted);
}

TEST_CASE("a NeedsConfirmation pause mid-plan still offers no Recovery link")
{
    // The plan has not stopped -- the dialog is prompting for the CPU name and
    // the resume is the intended next step. Sending the user to Recovery here
    // would talk them out of it.
    CHECK_FALSE(recoveryAnchorFor(FlashOutcome::NeedsConfirmation, TargetCpu::Display, 1, 2)
                .has_value());
}

// ---------------------------------------------------------------------------
// FlashController state machine. Exercised entirely through onProgress()/
// onFinished() -- the same seams poll() drains the worker queue through --
// so these are deterministic and touch no thread, no I/O and no hardware.
// The engine itself (runFlashPlan) already has its own coverage in
// test_fwFlashEngine.cpp; these tests are about the controller's state
// transitions on top of it, not about re-proving the engine.
// ---------------------------------------------------------------------------

TEST_CASE("a controller starts idle")
{
    FlashController c;
    CHECK(c.state() == FlashState::Idle);
    CHECK(c.progressLog().empty());
}

TEST_CASE("the log accumulates progress events in order")
{
    FlashController c;
    c.onProgress(FlashProgress{ FlashPhase::StepStarted, 0, 2, TargetCpu::Main, "first" });
    c.onProgress(FlashProgress{ FlashPhase::Copying,     0, 2, TargetCpu::Main, "second" });
    REQUIRE(c.progressLog().size() == 2);
    CHECK(c.progressLog()[0].find("first")  != std::string::npos);
    CHECK(c.progressLog()[1].find("second") != std::string::npos);
}

TEST_CASE("a NeedsConfirmation result puts the controller in the confirming state")
{
    FlashController c;
    FlashResult r;
    r.outcome = FlashOutcome::NeedsConfirmation;
    r.confirmationCpu = TargetCpu::Display;
    r.resumable = true;
    c.onFinished(r);
    CHECK(c.state() == FlashState::AwaitingConfirmation);
    REQUIRE(c.result().confirmationCpu.has_value());
    CHECK(*c.result().confirmationCpu == TargetCpu::Display);
}

TEST_CASE("a failure state carries a message the user can act on")
{
    FlashController c;
    FlashResult r;
    r.outcome = FlashOutcome::RefusedAmbiguous;
    r.message = "two RPI-RP2 volumes are mounted";
    c.onFinished(r);
    CHECK(c.state() == FlashState::Failed);
    CHECK_FALSE(c.result().message.empty());
}

TEST_CASE("a successful result puts the controller in the succeeded state")
{
    FlashController c;
    FlashResult r;
    r.outcome = FlashOutcome::Success;
    r.message = "flashing finished";
    c.onFinished(r);
    CHECK(c.state() == FlashState::Succeeded);
}

TEST_CASE("the recovery anchor tracks the CPU of the most recent progress event")
{
    // Regression for the requirement that a refusal on the MAIN CPU must
    // never be reported against the DISPLAY Recovery section: the
    // controller has to remember which CPU's step was actually running
    // when the failure landed, not default to one.
    FlashController c;
    c.onProgress(FlashProgress{ FlashPhase::StepStarted,      0, 1, TargetCpu::Main, "starting" });
    c.onProgress(FlashProgress{ FlashPhase::WaitingForVolume, 0, 1, TargetCpu::Main, "waiting" });

    FlashResult r;
    r.outcome = FlashOutcome::Timeout;
    r.message = "no RPI-RP2 volume appeared after rebooting the MAIN CPU";
    c.onFinished(r);

    CHECK(c.state() == FlashState::Failed);
    REQUIRE(c.recoveryAnchor().has_value());
    CHECK(*c.recoveryAnchor() == RecoveryAnchor::MainNotIdentified);
}

TEST_CASE("the recovery anchor follows a release-wait timeout to the CPU that just finished")
{
    // The second cause of FlashOutcome::Timeout (a volume that did not
    // release after a copy), exercised on the DISPLAY CPU this time: the
    // last progress event before the failure (WaitingForRelease) still
    // carries that step's CPU, which is what the anchor must key on.
    FlashController c;
    c.onProgress(FlashProgress{ FlashPhase::StepStarted,       1, 2, TargetCpu::Display, "starting" });
    c.onProgress(FlashProgress{ FlashPhase::Copying,           1, 2, TargetCpu::Display, "writing" });
    c.onProgress(FlashProgress{ FlashPhase::StepFinished,      1, 2, TargetCpu::Display, "done" });
    c.onProgress(FlashProgress{ FlashPhase::WaitingForRelease, 1, 2, TargetCpu::Display, "waiting for release" });

    FlashResult r;
    r.outcome = FlashOutcome::Timeout;
    r.message = "the DISPLAY CPU's RPI-RP2 volume did not disappear after being flashed";
    c.onFinished(r);

    REQUIRE(c.recoveryAnchor().has_value());
    CHECK(*c.recoveryAnchor() == RecoveryAnchor::DisplayNotIdentified);
}

TEST_CASE("cancelling while awaiting confirmation finishes as Aborted, with no recovery link")
{
    FlashController c;
    FlashResult r;
    r.outcome = FlashOutcome::NeedsConfirmation;
    r.confirmationCpu = TargetCpu::Main;
    r.resumable = true;
    c.onFinished(r);
    REQUIRE(c.state() == FlashState::AwaitingConfirmation);

    c.cancel();

    CHECK(c.state() == FlashState::Failed);
    CHECK(c.result().outcome == FlashOutcome::Aborted);
    CHECK_FALSE(c.recoveryAnchor().has_value());
}

TEST_CASE("cancel is a no-op while idle")
{
    FlashController c;
    c.cancel();
    CHECK(c.state() == FlashState::Idle);
}

TEST_CASE("confirm is a no-op unless awaiting confirmation")
{
    FlashController c;
    c.confirm("MAIN", CpuIdentity{});
    CHECK(c.state() == FlashState::Idle);
}

TEST_CASE("confirm refuses to resume a result that is not resumable")
{
    // AwaitingConfirmation is only ever entered from NeedsConfirmation, which
    // always sets resumable -- but the state alone must never be enough. Every
    // other outcome can leave a volume mounted, and resuming across one is the
    // wrong-CPU write FlashResult::resumable exists to prevent.
    FlashController c;
    FlashResult r;
    r.outcome   = FlashOutcome::NeedsConfirmation;
    r.resumable = false;   // the invariant deliberately violated
    c.onFinished(r);
    REQUIRE(c.state() == FlashState::AwaitingConfirmation);

    CpuIdentity id;
    id.mainPort = "COM71";
    c.confirm("MAIN", id);

    CHECK(c.state() == FlashState::AwaitingConfirmation);   // no worker started
    CHECK_FALSE(c.identity().mainPort.has_value());          // and nothing adopted
}

// ---------------------------------------------------------------------------
// Final review (Fix 1, Critical): the identity a RESUMED flash runs against.
//
// The gap this closes: begin() captured a CpuIdentity, then the plan stopped
// on NeedsConfirmation and sat there across an UNBOUNDED HUMAN PAUSE while the
// user read a sentence and typed a CPU name. confirm() rebuilt the worker's
// FlashIo from that ORIGINAL snapshot. Unplug board A during the pause and plug
// board B into the same USB port -- a realistic reaction to being told
// something unexpected is mounted -- and the engine would touch board A's
// "COM24", a number Windows reassigns from the freed pool and may well have
// handed to board B's DISPLAY console. That reboots B's DISPLAY CPU (no BOOTSEL
// button, no recovery path) into the bootloader and copies a MAIN image onto
// it. confirm() therefore takes the identity, and FlashDialog re-reads it under
// the same selectionUnchanged() gate the Idle path already ran.
//
// These drive the real state machine, including the real worker thread. They
// touch no hardware: the plan is empty, so runFlashPlan() returns immediately
// without calling findVolumes, touchPort or copyToVolume at all.
// ---------------------------------------------------------------------------

namespace {

/// Drain the worker queue the way the UI thread does -- poll() once per frame
/// -- until the final result has landed. Bounded so a defect cannot hang the
/// suite rather than fail it.
void pumpUntilSettled(FlashController& c)
{
    for (int i = 0; i < 5000 && c.state() == FlashState::Running; ++i) {
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

TEST_CASE("makeProductionFlashIo binds identify() to the snapshot it was handed")
{
    // The far end of the chain confirm()'s new parameter feeds: whatever
    // identity reaches the controller becomes io.identify(), which is what
    // decides the port runFlashPlan() hands to io.touchPort().
    CpuIdentity id;
    id.mainPort    = "COM71";
    id.displayPort = "COM72";

    FlashIo io = makeProductionFlashIo(id);
    REQUIRE(io.identify);
    const CpuIdentity seen = io.identify();
    REQUIRE(seen.mainPort.has_value());
    CHECK(*seen.mainPort == "COM71");
    REQUIRE(seen.displayPort.has_value());
    CHECK(*seen.displayPort == "COM72");
}

TEST_CASE("a resume runs against the identity given to confirm(), not begin()'s snapshot")
{
    FlashController c;

    // Board A, as it was when Start Flashing was clicked.
    CpuIdentity boardA;
    boardA.mainPort    = "COM24";
    boardA.displayPort = "COM59";

    // No uf2 assets -> buildFlashPlan() produces an empty plan -> the worker
    // finishes immediately having performed no I/O whatsoever. This test is
    // about which identity the controller carries, not about the engine.
    CatalogEntry entry;
    entry.slug = "empty-plan";
    entry.name = "Empty plan";

    c.begin(entry, boardA);
    pumpUntilSettled(c);
    REQUIRE(c.identity().mainPort.has_value());
    REQUIRE(*c.identity().mainPort == "COM24");

    // The plan hits an already-mounted RPI-RP2 volume and pauses for a typed
    // confirmation. During that pause the user swaps boards on the same port.
    FlashResult paused;
    paused.outcome         = FlashOutcome::NeedsConfirmation;
    paused.confirmationCpu = TargetCpu::Main;
    paused.stepsCompleted  = 0;
    paused.resumable       = true;
    c.onFinished(paused);
    REQUIRE(c.state() == FlashState::AwaitingConfirmation);

    // Board B, freshly re-read by the dialog under selectionUnchanged().
    CpuIdentity boardB;
    boardB.mainPort    = "COM71";
    boardB.displayPort = "COM72";

    c.confirm("MAIN", boardB);
    pumpUntilSettled(c);

    // Before this fix, both of these still read COM24/COM59 -- board A's
    // ports, on a board that is no longer plugged in.
    REQUIRE(c.identity().mainPort.has_value());
    CHECK(*c.identity().mainPort == "COM71");
    REQUIRE(c.identity().displayPort.has_value());
    CHECK(*c.identity().displayPort == "COM72");

    // And the resume genuinely ran, rather than the controller having sat
    // still -- otherwise the assertions above would pass vacuously.
    CHECK(c.state() == FlashState::Succeeded);
}

TEST_CASE("reset returns to idle and clears the log and result")
{
    FlashController c;
    c.onProgress(FlashProgress{ FlashPhase::StepStarted, 0, 1, TargetCpu::Main, "starting" });
    FlashResult r;
    r.outcome = FlashOutcome::CopyFailed;
    r.message = "could not write the MAIN image";
    c.onFinished(r);
    REQUIRE(c.state() == FlashState::Failed);

    c.reset();

    CHECK(c.state() == FlashState::Idle);
    CHECK(c.progressLog().empty());
    CHECK(c.result().message.empty());
    CHECK_FALSE(c.lastProgress().has_value());
    CHECK(c.progressFraction() == 0.0f);
}

// ---------------------------------------------------------------------------
// The progress bar the dialog draws. The mapping itself lives in the engine
// and is tested there; these are about what the CONTROLLER adds on top of it:
// a monotonic high-water mark, and keeping a wait's 120 periodic refreshes out
// of the scrolling log while still letting them drive the live readout.
// ---------------------------------------------------------------------------

TEST_CASE("a wait's periodic refreshes drive the live readout without touching the log") {
    // A wait that runs to its thirty-second timeout polls 120 times. Those
    // updates are exactly what stops the dialog freezing on one line -- and
    // exactly what would bury the log's record of what happened if they were
    // written to it. So: lastProgress follows them, progressLog does not, and
    // the log therefore grows with EVENTS rather than with elapsed time.
    FlashController c;

    FlashProgress announcement{ FlashPhase::WaitingForVolume, 1, 2, TargetCpu::Display,
                                "waiting for the RPI-RP2 volume" };
    announcement.waitTotalMs = kVolumeWaitMs;
    c.onProgress(announcement);
    REQUIRE(c.progressLog().size() == 1);

    int lastMs = 0;
    for (int ms = kVolumePollMs; ms <= kVolumeWaitMs; ms += kVolumePollMs) {
        FlashProgress tick = announcement;
        tick.isRefresh     = true;
        tick.waitElapsedMs = ms;
        c.onProgress(tick);
        lastMs = ms;
    }
    REQUIRE(lastMs == kVolumeWaitMs);   // a full timeout's worth really was fed in

    CHECK(c.progressLog().size() == 1);   // still just the announcement
    REQUIRE(c.lastProgress().has_value());
    CHECK(c.lastProgress()->isRefresh);
    CHECK(c.lastProgress()->waitElapsedMs == kVolumeWaitMs);
    CHECK(c.lastProgress()->phase == FlashPhase::WaitingForVolume);
}

TEST_CASE("the overall fraction does not slide backwards when a resumed step repeats itself") {
    // confirm() restarts the engine at the step that stopped on
    // NeedsConfirmation, and that step re-reports StepStarted/Loading/
    // Verifying -- work it had already shown before it paused. The PLAN has
    // not regressed just because one step is being re-evaluated, so the bar
    // must not say it has.
    FlashController c;
    c.onProgress(FlashProgress{ FlashPhase::StepStarted, 0, 2, TargetCpu::Main, "starting" });
    c.onProgress(FlashProgress{ FlashPhase::Loading,     0, 2, TargetCpu::Main, "loading" });
    c.onProgress(FlashProgress{ FlashPhase::Verifying,   0, 2, TargetCpu::Main, "checking" });

    const float paused = c.progressFraction();
    CHECK(paused == doctest::Approx(0.125));   // (0 + 0.25) / 2

    // The resume, replaying step 0 from the top.
    c.onProgress(FlashProgress{ FlashPhase::StepStarted, 0, 2, TargetCpu::Main, "starting" });
    CHECK(c.progressFraction() == doctest::Approx(paused));

    // ...and it still advances once the step gets further than it did before.
    c.onProgress(FlashProgress{ FlashPhase::Copying, 0, 2, TargetCpu::Main, "writing" });
    CHECK(c.progressFraction() > paused);
}

TEST_CASE("only a Success completes the bar; a failure leaves it where the flash stopped") {
    FlashController failed;
    failed.onProgress(FlashProgress{ FlashPhase::WaitingForVolume, 1, 2, TargetCpu::Display,
                                     "waiting for the RPI-RP2 volume" });
    const float stopped = failed.progressFraction();
    CHECK(stopped == doctest::Approx(0.725));   // (1 + 0.45) / 2

    FlashResult timeout;
    timeout.outcome = FlashOutcome::Timeout;
    timeout.stepsCompleted = 1;
    failed.onFinished(timeout);

    // Not snapped to full (which would be a lie) and not reset to empty
    // (which would throw away the only honest thing left to say).
    CHECK(failed.progressFraction() == doctest::Approx(stopped));
    CHECK(failed.progressFraction() < 1.0f);

    FlashController ok;
    ok.onProgress(FlashProgress{ FlashPhase::StepFinished, 0, 1, TargetCpu::Main, "done" });
    CHECK(ok.progressFraction() < 1.0f);   // StepFinished alone is not the whole plan
    FlashResult success;
    success.outcome = FlashOutcome::Success;
    success.stepsCompleted = 1;
    ok.onFinished(success);
    CHECK(ok.progressFraction() == doctest::Approx(1.0));
}

TEST_CASE("a fresh flash starts the bar over from zero") {
    // begin() is a genuine start-over, unlike confirm(). It zeroes the bar
    // synchronously, before the worker exists; the worker's result is only
    // ever applied by poll(), which has not run at the point of the check.
    FlashController c;
    c.onProgress(FlashProgress{ FlashPhase::Copying, 0, 1, TargetCpu::Main, "writing" });
    REQUIRE(c.progressFraction() > 0.0f);

    // No uf2 assets -> an empty plan -> the worker performs no I/O at all.
    CatalogEntry entry;
    entry.slug = "empty-plan";
    entry.name = "Empty plan";

    c.begin(entry, CpuIdentity{});
    CHECK(c.progressFraction() == 0.0f);

    pumpUntilSettled(c);
    CHECK(c.state() == FlashState::Succeeded);
    CHECK(c.progressFraction() == doctest::Approx(1.0));
}

TEST_CASE("lastProgress is nullopt until the first event, then tracks the latest")
{
    FlashController c;
    CHECK_FALSE(c.lastProgress().has_value());

    c.onProgress(FlashProgress{ FlashPhase::StepStarted, 0, 2, TargetCpu::Main, "first" });
    REQUIRE(c.lastProgress().has_value());
    CHECK(c.lastProgress()->phase == FlashPhase::StepStarted);

    c.onProgress(FlashProgress{ FlashPhase::Copying, 1, 2, TargetCpu::Display, "second" });
    REQUIRE(c.lastProgress().has_value());
    CHECK(c.lastProgress()->phase == FlashPhase::Copying);
    CHECK(c.lastProgress()->cpu == TargetCpu::Display);
}

// ---------------------------------------------------------------------------
// selectionUnchanged -- the device-replug guard a FlashDialog re-checks
// immediately before Start Flashing can be clicked (see fwFlashDialog.cpp's
// Idle-state handling). Pure and DeviceModel-free, so the mismatch refusal
// this backs is directly testable without a worker thread or real hardware.
//
// Fix round: uniqueID alone was found to be insufficient -- it is
// fwfinder's topological port-chain ID (see the header comment on
// selectionUnchanged in fwFlashController.h), so a DIFFERENT board plugged
// into the SAME port comes back with the SAME uniqueID. selectionUnchanged
// now also compares `serial`, and returns a SelectionCheck rather than a
// bool so the four cases below (matches / nothing selected / same port,
// different board / different port) are each individually testable and each
// produce a distinguishable refusal message via selectionCheckMessage().
// ---------------------------------------------------------------------------

TEST_CASE("selectionUnchanged is Unchanged only when uniqueID AND serial both match")
{
    DeviceView device;
    device.uniqueID = 42;
    device.serial   = "FW4788";

    CHECK(selectionUnchanged(std::optional<DeviceView>(device), 42, "FW4788") == SelectionCheck::Unchanged);
}

TEST_CASE("selectionUnchanged is NothingSelected when nothing is selected any more")
{
    CHECK(selectionUnchanged(std::nullopt, 42, "FW4788") == SelectionCheck::NothingSelected);
}

TEST_CASE("selectionUnchanged is DifferentBoard when the SAME port now holds a DIFFERENT board")
{
    // The exact hazard the previous uniqueID-only check missed: a replug can
    // put an entirely different board on the same USB port, and fwfinder's
    // uniqueID (packed purely from the port chain) comes back identical for
    // it. Only comparing serial as well catches this.
    DeviceView device;
    device.uniqueID = 42;   // SAME port as opened
    device.serial   = "FW9999"; // a DIFFERENT physical board

    CHECK(selectionUnchanged(std::optional<DeviceView>(device), 42, "FW4788") == SelectionCheck::DifferentBoard);
}

TEST_CASE("selectionUnchanged is DifferentPort when the SAME board moved to a DIFFERENT port")
{
    // The reciprocal case: the serial still matches (genuinely the same
    // physical unit), but uniqueID does not -- COM ports have almost
    // certainly been renumbered by the move, so the captured identity is
    // stale regardless of whose board it is.
    DeviceView device;
    device.uniqueID = 77;      // DIFFERENT port from the one opened on
    device.serial   = "FW4788"; // the SAME physical board

    CHECK(selectionUnchanged(std::optional<DeviceView>(device), 42, "FW4788") == SelectionCheck::DifferentPort);
}

TEST_CASE("selectionUnchanged treats an empty serial on either side as a mismatch, never a match")
{
    // UnidentifiedSerial, not DifferentBoard: still a refusal (that is what
    // matters, and the "never Unchanged" assertions below pin it), but a
    // separate one so the message can say "cannot be confirmed" instead of
    // asserting a swap nobody has observed. See the enum comment.
    DeviceView bothEmpty;
    bothEmpty.uniqueID = 42;
    bothEmpty.serial   = "";
    CHECK(selectionUnchanged(std::optional<DeviceView>(bothEmpty), 42, "") == SelectionCheck::UnidentifiedSerial);
    CHECK(selectionUnchanged(std::optional<DeviceView>(bothEmpty), 42, "") != SelectionCheck::Unchanged);

    DeviceView currentEmpty;
    currentEmpty.uniqueID = 42;
    currentEmpty.serial   = "";
    CHECK(selectionUnchanged(std::optional<DeviceView>(currentEmpty), 42, "FW4788") == SelectionCheck::UnidentifiedSerial);

    DeviceView openedEmptyCurrent;
    openedEmptyCurrent.uniqueID = 42;
    openedEmptyCurrent.serial   = "FW4788";
    CHECK(selectionUnchanged(std::optional<DeviceView>(openedEmptyCurrent), 42, "") == SelectionCheck::UnidentifiedSerial);
}

TEST_CASE("selectionUnchanged treats fwfinder's \"Unknown\" serial sentinel as a mismatch, not a match")
{
    // Fix round 3: "Unknown" is fwfinder's own literal (fwfinder.cpp, the
    // FreeWili branch taken when no FTDI child device is found), not an
    // empty string -- observed on real hardware in a degraded
    // identification state. Two DIFFERENT boards on the same port that both
    // happen to be in that degraded state must still refuse: comparing
    // "Unknown" == "Unknown" as a genuine match would reopen exactly the
    // hazard this guard exists to close.
    DeviceView device;
    device.uniqueID = 42;   // SAME port as opened
    device.serial   = "Unknown";

    CHECK(selectionUnchanged(std::optional<DeviceView>(device), 42, "Unknown") == SelectionCheck::UnidentifiedSerial);
    // The property that actually protects the board: whatever this is called,
    // it is never the one answer that permits a flash.
    CHECK(selectionUnchanged(std::optional<DeviceView>(device), 42, "Unknown") != SelectionCheck::Unchanged);
}

TEST_CASE("a board that goes unidentified mid-dialog refuses, but is not accused of being a different board")
{
    // The mirror of the DeviceModel adoption fix: the dialog opened on a
    // CONFIRMED serial and the board then dropped to fwfinder's "Unknown"
    // part-way through re-enumeration. That must keep refusing -- from this
    // data it is indistinguishable from a swap in progress -- but the refusal
    // must not TELL the user a different board is plugged in, because nothing
    // observed that.
    DeviceView reenumerating;
    reenumerating.uniqueID = 42;          // same port the dialog opened on
    reenumerating.serial   = "Unknown";   // ...but momentarily saying nothing

    const SelectionCheck check = selectionUnchanged(std::optional<DeviceView>(reenumerating), 42, "FW6548");
    CHECK(check == SelectionCheck::UnidentifiedSerial);
    CHECK(check != SelectionCheck::Unchanged);              // refuses, which is the point
    // Freshness cannot rescue it either -- a refusal is passed through as-is.
    CHECK(selectionUnchangedFresh(std::optional<DeviceView>(reenumerating), 42, "FW6548",
                                  std::chrono::milliseconds(0)) == SelectionCheck::UnidentifiedSerial);

    const auto msg = selectionCheckMessage(check);
    CHECK_FALSE(msg.empty());
    CHECK(msg != selectionCheckMessage(SelectionCheck::DifferentBoard));
    CHECK(msg.find("different device") == std::string::npos);   // no swap is being asserted
    CHECK(msg.find("not reporting a serial") != std::string::npos);

    // And a REAL substitution is still called what it is, so splitting the two
    // has not blunted the message that matters most.
    DeviceView swapped;
    swapped.uniqueID = 42;
    swapped.serial   = "FW2222";
    CHECK(selectionUnchanged(std::optional<DeviceView>(swapped), 42, "FW6548") == SelectionCheck::DifferentBoard);
}

TEST_CASE("selectionCheckMessage is empty only for Unchanged, and distinguishes board-swap from port-move")
{
    CHECK(selectionCheckMessage(SelectionCheck::Unchanged).empty());
    CHECK_FALSE(selectionCheckMessage(SelectionCheck::NothingSelected).empty());

    const auto boardMsg = selectionCheckMessage(SelectionCheck::DifferentBoard);
    const auto portMsg  = selectionCheckMessage(SelectionCheck::DifferentPort);
    CHECK_FALSE(boardMsg.empty());
    CHECK_FALSE(portMsg.empty());
    // Someone who swapped boards must see something different from someone
    // who moved the same board to another port.
    CHECK(boardMsg != portMsg);

    // And someone whose scan data is merely too old must not be told either of
    // those happened -- nothing is known to have changed.
    const auto staleMsg = selectionCheckMessage(SelectionCheck::StaleSnapshot);
    CHECK_FALSE(staleMsg.empty());
    CHECK(staleMsg != boardMsg);
    CHECK(staleMsg != portMsg);

    // ...and neither must someone whose board is merely mid-reconnect. Every
    // refusal reason gets its own sentence; only Unchanged is silent.
    const auto unidMsg = selectionCheckMessage(SelectionCheck::UnidentifiedSerial);
    CHECK_FALSE(unidMsg.empty());
    CHECK(unidMsg != boardMsg);
    CHECK(unidMsg != portMsg);
    CHECK(unidMsg != staleMsg);
}

// ---------------------------------------------------------------------------
// selectionUnchangedFresh -- the freshness precondition on top of the identity
// comparison above.
//
// Why it exists: selectionUnchanged() reads BOTH sides of its comparison out of
// the same cached scan result. If nothing has re-scanned since before the user
// swapped board A for board B in the same USB port, that cache still describes
// A on both sides and the comparison reports Unchanged -- correctly, about
// stale data. The flash dialog is modal, so the Rescan button is unreachable
// while it is open, and the pause in the AwaitingConfirmation branch is
// unbounded by construction. Age is the only signal that can catch this.
// ---------------------------------------------------------------------------

namespace {
DeviceView sameBoard()
{
    DeviceView d;
    d.uniqueID = 42;
    d.serial   = "FW4788";
    return d;
}
} // namespace

TEST_CASE("selectionUnchangedFresh approves an identity match backed by a recent scan")
{
    // The ordinary case: the dialog is holding the scanner in its fast-poll
    // window, so the snapshot is a few hundred ms old at worst.
    CHECK(selectionUnchangedFresh(std::optional<DeviceView>(sameBoard()), 42, "FW4788",
                                  std::chrono::milliseconds(200)) == SelectionCheck::Unchanged);
}

TEST_CASE("selectionUnchangedFresh refuses an identity match backed by a stale scan")
{
    // Exactly the frozen-scanner case: the data says "same board" because
    // nobody has looked since before the swap.
    CHECK(selectionUnchangedFresh(std::optional<DeviceView>(sameBoard()), 42, "FW4788",
                                  std::chrono::milliseconds(30000)) == SelectionCheck::StaleSnapshot);
}

TEST_CASE("selectionUnchangedFresh fails CLOSED when the snapshot's age is unknown")
{
    // nullopt means no scan has ever completed (or the platform cannot say).
    // That is the least verified state there is and must refuse, not default
    // to "fresh enough".
    CHECK(selectionUnchangedFresh(std::optional<DeviceView>(sameBoard()), 42, "FW4788",
                                  std::nullopt) == SelectionCheck::StaleSnapshot);
}

TEST_CASE("selectionUnchangedFresh's freshness boundary is inclusive at maxAge and refuses beyond it")
{
    const auto maxAge = std::chrono::milliseconds(1000);
    CHECK(selectionUnchangedFresh(std::optional<DeviceView>(sameBoard()), 42, "FW4788",
                                  std::chrono::milliseconds(1000), maxAge) == SelectionCheck::Unchanged);
    CHECK(selectionUnchangedFresh(std::optional<DeviceView>(sameBoard()), 42, "FW4788",
                                  std::chrono::milliseconds(1001), maxAge) == SelectionCheck::StaleSnapshot);
}

TEST_CASE("selectionUnchangedFresh's default maxAge is the one the flash dialog relies on")
{
    // Guards the constant itself: a value the fast-poll window cannot keep up
    // with would make the Proceed button permanently unreachable, and one much
    // larger would let an approval drift further behind reality than intended.
    CHECK(selectionUnchangedFresh(std::optional<DeviceView>(sameBoard()), 42, "FW4788",
                                  kMaxSnapshotAgeForFlash) == SelectionCheck::Unchanged);
    CHECK(selectionUnchangedFresh(std::optional<DeviceView>(sameBoard()), 42, "FW4788",
                                  kMaxSnapshotAgeForFlash + std::chrono::milliseconds(1))
          == SelectionCheck::StaleSnapshot);
}

TEST_CASE("selectionUnchangedFresh keeps the specific refusal rather than masking it with staleness")
{
    // A refusal is a refusal either way, so safety is identical -- but "a
    // different device now occupies this USB port" tells the user what to do
    // and a generic staleness notice does not. Only the answer that PERMITS a
    // flash is gated on freshness.
    DeviceView swapped = sameBoard();
    swapped.serial = "FW9999";          // same port, different board
    CHECK(selectionUnchangedFresh(std::optional<DeviceView>(swapped), 42, "FW4788",
                                  std::nullopt) == SelectionCheck::DifferentBoard);

    DeviceView moved = sameBoard();
    moved.uniqueID = 77;                // same board, different port
    CHECK(selectionUnchangedFresh(std::optional<DeviceView>(moved), 42, "FW4788",
                                  std::chrono::milliseconds(30000)) == SelectionCheck::DifferentPort);

    CHECK(selectionUnchangedFresh(std::nullopt, 42, "FW4788",
                                  std::nullopt) == SelectionCheck::NothingSelected);
}
