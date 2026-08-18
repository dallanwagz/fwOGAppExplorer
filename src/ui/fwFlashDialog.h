#pragma once

#include "core/fwTypes.h"
#include "device/fwDeviceModel.h"   // DeviceView
#include "flash/fwFlashController.h"
#include "ui/fwRecoveryContent.h"   // RecoveryAnchor

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fwog {

/// The flash confirmation/progress modal: the one place in this app that
/// actually writes to a board. Owns a single FlashController, so opening it
/// for one catalog entry while a previous flash from either tab is still
/// running is refused (see open()) -- only one flash can ever be in flight,
/// and this dialog is the only door into starting one.
///
/// Shared by AppExplorerTab and DefaultFirmwareTab: one instance, owned by
/// App::run() and passed to both by reference, rather than one per tab --
/// two independent controllers could not enforce "only one flash at a time"
/// against each other.
class FlashDialog {
public:
    /// Opens the dialog on `entry`, showing its plan (buildFlashPlan) and
    /// warnings (planWarnings) for review. Does NOT start flashing -- that
    /// happens only once the user clicks Start Flashing inside the dialog,
    /// which is also where `device` is re-read fresh (see draw()) rather
    /// than trusted from here: `device` is captured only to remember WHICH
    /// device this dialog is about (its `uniqueID` AND `serial` -- see
    /// selectionUnchanged()'s comment on why uniqueID alone cannot tell a
    /// replugged board apart from a substituted one), not to freeze the
    /// `CpuIdentity` that will actually be used to flash it.
    ///
    /// No-op while a flash is Running (belt and braces: the modal already
    /// blocks input to whatever is behind it, so this should be
    /// unreachable through the UI, but FlashController::begin() enforces
    /// "one flash at a time" on its own too).
    void open(const CatalogEntry& entry, const DeviceView& device);

    /// Starts flashing `entry` against `device` IMMEDIATELY, with no modal and
    /// no review step, for a caller that has already put the review on screen.
    ///
    /// The App Explorer tab is that caller: its detail pane already renders the
    /// plan, the dropped-erase note and planWarnings() beside the Flash button,
    /// so the modal's first page was showing the user a second copy of what they
    /// had just read, and "click Flash, then click Start Flashing" was two
    /// clicks for one decision.
    ///
    /// WHAT THIS DOES NOT SKIP is the safety gate. The reason the modal defers
    /// to a Start button is the UNBOUNDED HUMAN PAUSE it opens: the board can be
    /// swapped between opening the dialog and clicking Start, so the identity is
    /// re-read and re-checked at Start rather than trusted from open(). A single
    /// click has no such pause -- `device` is read in the very frame the button
    /// is pressed -- which removes the swap window rather than ignoring it. The
    /// remaining half of the gate, that the scan behind `device` is recent
    /// enough to describe the board as it is NOW, cannot be removed that way and
    /// is the caller's to satisfy; see AppExplorerTab's pending-start handling,
    /// which requests a rescan and waits for a fresh snapshot rather than
    /// flashing on stale data.
    ///
    /// `device.identity` is used as-is, so the caller must pass a device read
    /// this frame from DeviceModel::selected() -- the CONFIRMED selection, never
    /// selectedByIdOnly().
    ///
    /// No-op while a flash is already running. AwaitingConfirmation and Failed
    /// outcomes are NOT handled inline: draw() raises the modal for those (see
    /// its comment), so the rich prompt/failure UI is reached without every
    /// caller reimplementing it.
    void beginImmediate(const CatalogEntry& entry, const DeviceView& device);

    /// True while a flash started by beginImmediate() is executing or waiting on
    /// the user, so a caller can render progress beside its own button and keep
    /// other Flash buttons disabled. False once the run is finished and its
    /// outcome has been handed to the modal.
    bool isInlineBusy() const;

    /// True once a flash started by beginImmediate() has finished with
    /// Success and nothing has been started since. Success deliberately raises
    /// no modal (see draw()), so this is how the caller says "done" in place --
    /// a bar that quietly vanished at 100% read as nothing having happened.
    bool inlineSucceeded() const;

    /// Live progress of the current run, for a caller drawing its own bar:
    /// 0..1, monotonic within one flash (see FlashController::progressFraction).
    float progressFraction() const;

    /// One-line "step N of M -- phase" readout, or empty before any progress
    /// has arrived. Shares flashPhaseLabel() with the modal so the two can
    /// never describe the same phase two different ways.
    std::string progressLine() const;

    /// True while the current step is inside a BOUNDED WAIT (waiting for a
    /// volume to appear or release). Exists so a caller can show something that
    /// is visibly alive: the overall progress bar deliberately does not move
    /// during a wait, which for up to thirty seconds is indistinguishable from
    /// a hang.
    bool isWaiting() const;

    /// How long the WHOLE deprecated-firmware install is expected to take.
    ///
    /// It is an ESTIMATE and is treated as one everywhere it is used: it is a
    /// measured wall-clock duration for that one plan, not something the engine
    /// reports. See estimatedFraction() for what happens when reality disagrees.
    static constexpr std::chrono::seconds kLegacyFlashEstimate{ 210 };   // 3m 30s

    /// True while a run with a time estimate is in flight.
    ///
    /// Only the FULL deprecated-firmware plan has one -- erase DISPLAY, write
    /// DISPLAY, write MAIN. Its per-CPU halves (restrictToCpu) write a single
    /// image and are nothing like 3m30s, so they deliberately get no countdown
    /// rather than a wrong one.
    bool hasTimeEstimate() const;

    /// Elapsed fraction of kLegacyFlashEstimate, 0..1.
    ///
    /// CLAMPED BELOW 1.0 while the flash is still running, however long it
    /// overruns. A time bar is a guess, and a guess that reaches 100% while the
    /// board is still being written would be this dialog telling the user
    /// something untrue -- which is exactly what the overall bar's own comment
    /// refuses to do. It reaches full only when the flash actually finishes.
    float estimatedFraction() const;

    /// "2m 05s left", or "taking longer than expected" once the estimate has
    /// been passed. Empty when there is no estimate in play.
    std::string estimatedRemaining() const;

    /// True from open() until the user closes the dialog. Tabs use this to
    /// keep their own Flash button disabled for the duration, on top of
    /// the modal's own input block.
    bool isOpen() const { return m_open; }

    /// True only while a flash is actually executing (not merely while the
    /// dialog is open reviewing a plan, and not while paused on a typed
    /// confirmation). Used by the caller to veto an OS window-close request
    /// -- see fwApp.cpp -- since the modal already makes a running flash
    /// un-abortable from inside the UI (no Close button, Escape excluded;
    /// see draw()'s comment) and the title bar's own close control must not
    /// be a back door around that.
    bool isFlashRunning() const;

    /// Draws the modal for one frame if open() was called; otherwise only
    /// polls the controller (cheap; keeps "poll every frame" unconditional
    /// rather than dependent on m_open). `deviceModel` is read (never
    /// mutated) to re-validate the selection immediately before Start
    /// Flashing can be clicked -- see the Idle-state handling in the .cpp
    /// for why a snapshot taken at open() cannot be trusted an unbounded
    /// number of frames later. `onOpenRecovery` is invoked with the mapped
    /// anchor when the user clicks "Open Recovery" on a failure -- the
    /// caller owns the tab bar and TabRecovery, so it alone can both
    /// scrollTo() and actually switch the visible tab; FlashDialog has no
    /// notion of either.
    void draw(DeviceModel& deviceModel, const std::function<void(RecoveryAnchor)>& onOpenRecovery);

private:
    void close();

    /// Rebuild m_plan/m_droppedNote/m_warnings for `identity` -- the same
    /// buildFlashPlan() + dropRedundantErases() pair FlashController::begin()
    /// applies, so what is previewed is what runs. Called from open() and again
    /// from every Idle frame of draw(), never once a flash is under way.
    void refreshPlan(const CpuIdentity& identity);

    FlashController m_controller;
    bool m_open = false;
    bool m_shouldOpenPopup = false;

    CatalogEntry m_entry;
    /// The DeviceView::uniqueID and BoardFingerprint of the device selected
    /// when open() was called -- NOT a CpuIdentity snapshot. See open()'s
    /// comment: the identity actually used to flash is re-read fresh, from
    /// the current DeviceModel, only once Start Flashing is clicked and only
    /// after confirming the board there does not contradict this one
    /// (selectionUnchanged()): uniqueID alone matches a SOCKET, not a board --
    /// a different board substituted into the same port would pass a
    /// uniqueID-only check.
    ///
    /// The fingerprint is RE-CAPTURED when a run pauses on
    /// AwaitingConfirmation (see draw()): the plan may already have rewritten
    /// a CPU by then, and the pause is what the resume gate must measure a
    /// swap against -- not the moment the dialog opened.
    uint64_t         m_openedUniqueID = 0;
    BoardFingerprint m_openedPrint;
    bool             m_printCapturedForPause = false;

    std::vector<FlashStep>   m_plan;
    std::vector<std::string> m_warnings;
    /// droppedEraseNote(): why the plan above is one step shorter than the
    /// entry's own, or empty. See refreshPlan().
    std::string              m_droppedNote;
    /// The DISPLAY-quieting preparation, when the plan and the board call for
    /// it (quietDisplayBeforeMainWrite, fwFlashPrep.h), or empty.
    std::string              m_prepNote;

    char m_confirmBuf[64] = {};

    /// When the current run started, for the countdown above. Set at every
    /// begin() call site and cleared by close(); nullopt means no run has
    /// started in this dialog's lifetime, which is what suppresses the bar.
    ///
    /// Kept HERE rather than in FlashController deliberately: it is a
    /// presentation estimate, and the controller has no clock and no business
    /// acquiring one to serve a progress bar.
    std::optional<std::chrono::steady_clock::time_point> m_startedAt;

    /// Set by beginImmediate(), cleared once draw() has raised the modal for a
    /// run that needs the user (AwaitingConfirmation) or has something to report
    /// (Failed). It is what tells draw() that a run in flight has no window
    /// behind it yet, and it is deliberately NOT "the controller is busy": a
    /// modal-started run must not be adopted by the inline path.
    bool m_inlineRun = false;
};

} // namespace fwog
