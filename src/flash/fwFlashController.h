#pragma once

#include "core/fwTypes.h"
#include "device/fwDeviceModel.h"   // DeviceView
#include "flash/fwFlashEngine.h"
#include "flash/fwFlashPlan.h"
#include "ui/fwRecoveryContent.h"   // RecoveryAnchor

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fwog {

/// Binds FlashIo to the real platform: fwog::findRpiRp2Volumes,
/// fwog::touchPort1200, fwog::copyToVolume, fwog::tempDir, and an image
/// loader that dispatches on which ImageRef field is set (embedded id ->
/// loadEmbeddedImage, localPath -> a plain file read, url -> httpGet).
///
/// `identify` is LIVE when `uniqueID` is non-zero: every call is a fresh
/// identifyBoardNow(uniqueID) (fwBoardIdentify.h) -- one Fw::find_all() round
/// trip for the board at that hub position -- with `identity` serving as the
/// starting point and as the answer whenever the board cannot be found for a
/// moment (mid-re-enumeration). The verified-probe volumes of `identity`, if
/// any, are carried onto every live answer (withProbeVolumesFrom()).
///
/// Live rather than a snapshot because a plan CHANGES the board as it runs.
/// The step that erased MAIN leaves a MAIN drive where a MAIN port used to be;
/// the DISPLAY bootloader's console appears ten seconds after MAIN goes quiet;
/// the preparation that quiets DISPLAY (fwFlashPrep.h) turns its port into a
/// drive. A snapshot taken before any of that names ports that no longer
/// exist and misses drives that now do -- and in one real shape (bootloader
/// install: ERASE MAIN, then WRITE DISPLAY with no display port in the
/// snapshot) it left the engine unable to tell that the one mounted drive
/// was MAIN's, and asking the user to type DISPLAY over it. The live answer
/// knows the drive is at MAIN's hub port and refuses.
///
/// It does NOT read DeviceModel, which is not thread-safe and belongs to the
/// UI thread; identifyBoardNow() goes to fwfinder directly.
///
/// With `uniqueID == 0` (tests, and callers with no board handle) `identify`
/// returns `identity` unchanged every time, as it always did.
///
/// `waitTick` sleeps for the requested interval and always returns true (no
/// cancellation support). FlashController replaces it with a
/// cancellation-aware version bound to its own flag before ever running this
/// FlashIo -- see startWorker() in the .cpp. A caller that uses this
/// function directly, without going through FlashController, gets a FlashIo
/// with no way to cancel an in-progress wait.
FlashIo makeProductionFlashIo(const CpuIdentity& identity, uint64_t uniqueID = 0);

/// Result of comparing the device selected THIS frame against the one a
/// FlashDialog captured when it opened. See selectionUnchanged().
enum class SelectionCheck {
    Unchanged,       ///< same uniqueID, and nothing the board says about itself
                     ///< contradicts what was recorded -- safe to proceed
    NothingSelected, ///< no device is selected any more
    DifferentBoard,  ///< uniqueID (USB port) matches but the board there now
                     ///< positively contradicts the recorded fingerprint -- an
                     ///< FTDI serial or an RP2040 chip id both sides know, and
                     ///< disagree on. A DIFFERENT physical board occupies the
                     ///< port the dialog opened on
    DifferentPort,   ///< uniqueID no longer matches. Whether or not this is still
                     ///< the same physical board, its COM ports have almost
                     ///< certainly been renumbered by the move -- the identity
                     ///< captured at open() is stale either way
    StaleSnapshot,   ///< the comparison itself cannot be trusted: the device list
                     ///< it was made against is too old (or no scan has ever
                     ///< completed) to rule out a swap that happened since. NOT a
                     ///< statement that anything changed -- a statement that
                     ///< nobody has looked recently enough to know
};

/// Compares `current` (the device selected THIS frame) against the device a
/// FlashDialog captured at open() time (`openedUniqueID` from
/// `DeviceView::uniqueID`, `opened` from fingerprintOf(DeviceView)). Returns
/// SelectionCheck::Unchanged only when it is safe to proceed with a flash;
/// every other value is a refusal reason.
///
/// `uniqueID` ALONE IS NOT ENOUGH. It is fwfinder's own
/// Fw::FreeWiliDevice::uniqueID, packed purely from the USB port chain
/// (`_generateUniqueIDFromUSBPortChain`, fwfinder.cpp) -- it identifies a
/// SOCKET, not a board. Unplug board A from a port and plug board B into
/// that same port, and `uniqueID` comes back identical for B. The board's
/// FINGERPRINT (BoardFingerprint, fwDeviceModel.h: FTDI serial plus the two
/// RP2040 chip ids) is what tells physical units apart, and this refuses --
/// DifferentBoard -- when the board now on the port CONTRADICTS the recorded
/// fingerprint: some identifier both know, with different values.
///
/// It does NOT refuse a board that says nothing about itself. A FreeWili OG
/// running OG firmware never enumerates its FTDI, so its serial is fwfinder's
/// "Unknown" for its whole working life; a board with both CPUs in the
/// bootrom reports no chip id either. The rule this replaces refused every
/// such board, which is to say every board this app exists to flash, in the
/// states it most needs flashing in. What actually prevents a wrong-CPU write
/// on a swapped board is hub-position identification of the CPUs, which the
/// engine applies to whatever board is there -- the fingerprint check is a
/// second line, and a second line that refuses everything protects nothing.
///
/// A caller must re-run this (and re-read a fresh `CpuIdentity` from
/// `current`) immediately before calling begin(), rather than trusting
/// whatever was true when the dialog opened -- see FlashDialog::draw()'s
/// Idle-state handling.
SelectionCheck selectionUnchanged(const std::optional<DeviceView>& current,
                                   uint64_t openedUniqueID,
                                   const BoardFingerprint& opened);

/// The oldest a device snapshot may be for selectionUnchangedFresh() to
/// approve a flash against it.
///
/// Why any limit at all: selectionUnchanged() compares a uniqueID and a serial
/// read out of the SAME cached scan result the dialog opened on. If nothing has
/// re-scanned since before the user swapped board A for board B in the same USB
/// port, that cache still describes A on both sides, and the comparison reports
/// Unchanged with total confidence -- it is comparing a copy of stale data with
/// itself. No amount of care about WHAT is compared can detect that; only the
/// data's AGE can. The flash dialog is modal, so the user cannot reach the
/// Rescan button to disprove it either, which is why this cannot be left to
/// them.
///
/// Why 1500 ms: a dialog whose gate is on screen holds fwFinderManager in its
/// kFastPollMs window (FlashDialog::draw() calls requestRescan() every frame
/// while the gate is live), where a scan lands roughly every 260 ms on the
/// measured hardware. 1500 ms is about five of those -- loose enough that one
/// slow enumeration does not flicker the Proceed button off, tight enough that
/// an approval is never more than ~1.5 s behind physical reality. If scanning
/// stops entirely, the gate closes within that window and stays closed.
inline constexpr std::chrono::milliseconds kMaxSnapshotAgeForFlash{1500};

/// selectionUnchanged() plus a freshness precondition. `snapshotAge` is
/// DeviceModel::snapshotAge() -- how long ago the scan behind `current`
/// actually completed, or nullopt if none ever has.
///
/// Returns StaleSnapshot in place of Unchanged when the snapshot is older than
/// `maxAge`, or when its age is unknown. FAILS CLOSED: an unknown or excessive
/// age is never treated as fresh, so a caller gating a Proceed button on this
/// refuses rather than enabling it on unverified data.
///
/// A NON-Unchanged answer from selectionUnchanged() is passed through as-is
/// rather than being overwritten with StaleSnapshot. Both refuse, so the safety
/// outcome is identical either way, and the specific refusal ("a different
/// device now occupies this USB port") tells the user more about what to do
/// next than a generic staleness notice would. Only the one answer that PERMITS
/// a flash is gated on freshness, because it is the only one where being wrong
/// damages a board.
SelectionCheck selectionUnchangedFresh(const std::optional<DeviceView>& current,
                                        uint64_t openedUniqueID,
                                        const BoardFingerprint& opened,
                                        std::optional<std::chrono::milliseconds> snapshotAge,
                                        std::chrono::milliseconds maxAge = kMaxSnapshotAgeForFlash);

/// Human-readable explanation of a non-Unchanged SelectionCheck (empty for
/// Unchanged). Deliberately worded differently for DifferentBoard ("a
/// different device now occupies this port") than for DifferentPort ("this
/// device moved") so someone who swapped boards sees something different
/// from someone who moved the same board to another port.
std::string selectionCheckMessage(SelectionCheck check);

/// Human-readable phase name ("loading", "verifying", ...). Used both by
/// the controller's own progress-log formatting and by FlashDialog's
/// one-line "step N of M" readout, so the two never describe the same
/// phase two different ways.
const char* flashPhaseLabel(FlashPhase phase);

/// Maps a flash outcome to the Recovery tab section that explains it, or
/// nullopt when there is nothing to send the user to: Success and
/// NeedsConfirmation aren't failures (the dialog itself already prompts for
/// what NeedsConfirmation needs), and InvalidArgument is a caller bug, not a
/// board state.
///
/// `stepsCompleted`/`stepCount` come straight from FlashResult::stepsCompleted
/// and the plan's size, and decide ONE thing, ahead of everything else: a plan
/// that stopped PART-WAY through (0 < stepsCompleted < stepCount) left the two
/// CPUs holding firmware from two different installs, and that mixed state is
/// what the user has to deal with first -- ahead of whatever reason step
/// `stepsCompleted` itself failed for. Such a failure therefore maps to
/// RecoveryAnchor::PartialLegacyFlash whatever the outcome was.
///
/// This is the realistic Original FreeWili (LegacyDirect) path, not a corner
/// case: step 0 succeeds -- which is precisely what destroys the display
/// bootloader -- and step 1 then refuses because the DISPLAY port could not be
/// identified, since the firmware just installed publishes no FWOG_* product
/// string and a running MAIN CPU suppresses the display console entirely.
/// Mapping that to DisplayNotIdentified sends the user to a section telling
/// them to install the bootloader from the Default Firmware tab without ever
/// mentioning that the one they had is gone. Before this, PartialLegacyFlash
/// was returned for NO outcome at all: the Recovery section written for exactly
/// this state was reachable only by browsing to it.
///
/// It is also why Aborted is no longer unconditionally nullopt. Cancelling
/// between the MAIN and DISPLAY steps of a LegacyDirect plan produces the same
/// mixed board as any other mid-plan stop -- fwApp.cpp's window-close guard
/// says so in as many words. Cancelling BEFORE anything was written is still
/// the user's own choice with nothing to explain, and still returns nullopt.
///
/// `cpu` selects between the two CPU-specific anchors for
/// RefusedUnidentified and Timeout -- see RecoveryAnchor's header comment
/// (fwRecoveryContent.h) for why attaching DisplayNotIdentified to a
/// MAIN-side failure would tell the user things that are not true of their
/// situation (no BOOTSEL button, 1200-baud reboot, FWOG_DIAG -- the MAIN CPU
/// has a reachable BOOTSEL button instead). `cpu` is ignored for every other
/// outcome; pass anything for those. Callers should pass the CPU whose step
/// actually produced the outcome -- FlashController::recoveryAnchor() does
/// this using the CPU of the most recent progress event it saw, which is
/// always that step's CPU (see its own comment). This free function takes
/// the CPU explicitly, rather than tracking it itself, so the mapping stays
/// testable without spinning up a controller or a worker thread. The step
/// counts are passed the same way, for the same reason -- and as PARAMETERS
/// rather than as a `default:` arm bolted onto the switch: the switch over
/// FlashOutcome is exhaustive with no `default:` on purpose, so that adding an
/// enumerator is a compile error here instead of a silent fallthrough to "no
/// Recovery section". That property is preserved.
std::optional<RecoveryAnchor> recoveryAnchorFor(FlashOutcome outcome, TargetCpu cpu,
                                                 std::size_t stepsCompleted,
                                                 std::size_t stepCount);

/// Lifecycle of one FlashController. A controller is reused across many
/// flashes, one at a time -- see begin().
enum class FlashState {
    Idle,                  ///< nothing running, nothing to report
    Running,                ///< a worker thread is executing runFlashPlan()
    AwaitingConfirmation,   ///< paused on FlashOutcome::NeedsConfirmation
    Succeeded,
    Failed,
};

/// Owns the worker thread that runs one flash at a time, and the
/// mutex-guarded queue the UI drains via poll(). Nothing here touches
/// ImGui; fwFlashDialog.h is the widget that reads this.
///
/// Only one flash may run at a time: begin() and confirm() are no-ops
/// outside the states that make sense for them (see each one's comment).
/// Concurrent flashes to one board are meaningless, and concurrent volume
/// polling would race against itself.
class FlashController {
public:
    FlashController();
    ~FlashController();

    FlashController(const FlashController&) = delete;
    FlashController& operator=(const FlashController&) = delete;

    /// Starts flashing `entry` against `identity`. `identity` MUST be a
    /// snapshot -- e.g. DeviceModel::selected()->identity, copied out by the
    /// caller BEFORE calling this -- never a live reference into
    /// DeviceModel: a flash can run for tens of seconds across many UI
    /// frames, DeviceModel is rebuilt every one of them, and the worker
    /// thread this spawns must never touch it.
    ///
    /// `uniqueID` is the board's DeviceView::uniqueID. When non-zero the
    /// worker re-identifies the board LIVE at every step, by that handle,
    /// through makeProductionFlashIo(identity, uniqueID) -- see its comment
    /// for why a snapshot is not enough for a plan that changes the board as
    /// it runs. `identity` is then the starting point and the fallback for a
    /// moment when the board cannot be found. Zero (the default, and what the
    /// tests pass) keeps `identity` fixed for the whole run.
    ///
    /// Before step 0 the worker runs quietDisplayBeforeMainWrite()
    /// (fwFlashPrep.h) against the same FlashIo: for a plan that installs
    /// MAIN firmware while the DISPLAY is running, the DISPLAY is rebooted
    /// into BOOTSEL first so the MAIN write is stable. Reported into the log
    /// as FlashPhase::Preparing.
    ///
    /// No-op while state() == Running: only one flash may run at a time.
    /// Safe to call from every other state -- Idle, Succeeded,
    /// AwaitingConfirmation or Failed -- each call starts a fresh run from
    /// step 0 and discards whatever the previous one reported.
    void begin(const CatalogEntry& entry, const CpuIdentity& identity, uint64_t uniqueID = 0);

    /// Supplies the CPU name the user typed and resumes a plan that
    /// stopped at NeedsConfirmation. No-op unless state() ==
    /// AwaitingConfirmation. Resumes at result().stepsCompleted against the
    /// SAME original plan built in begin() -- never a re-sliced one. This is
    /// safe here specifically because NeedsConfirmation is the one outcome
    /// whose result().resumable is true; see FlashResult's comment in
    /// fwFlashEngine.h for why every other outcome must never be resumed.
    ///
    /// `identity` REPLACES the one begin() captured, and must be read fresh
    /// from the current DeviceModel by the caller immediately before this call
    /// -- exactly as begin()'s own caller does, and under the same
    /// selectionUnchanged() gate. This is not a convenience: the pause between
    /// begin() and confirm() is an UNBOUNDED HUMAN PAUSE. The user is being
    /// asked to read a sentence and type a CPU name, and in that window they
    /// can unplug the board and plug a different one into the same USB port --
    /// which is a realistic reaction to being told something is unexpected
    /// about the board. begin()'s snapshot would then still name board A's COM
    /// ports; Windows reassigns COM numbers from the freed pool, so
    /// io.touchPort() on a stale "COM24" can reboot a DIFFERENT CPU of a
    /// DIFFERENT board into BOOTSEL and receive the wrong image. uniqueID
    /// cannot catch that on its own -- it is topological, so the same socket
    /// yields the same value for the substituted board (see
    /// selectionUnchanged() above); the caller's serial comparison is what
    /// does, and this parameter is what makes the engine actually USE the
    /// board that comparison approved.
    void confirm(std::string typed, const CpuIdentity& identity);

    /// Requests cancellation. While Running, sets the flag the worker's
    /// waitTick polls -- cooperative, not immediate: the current step's
    /// blocking call (a copy already in flight, a download) still runs to
    /// completion before the next waitTick is reached. While
    /// AwaitingConfirmation, there is no worker thread to signal, so this
    /// finishes the operation synchronously as FlashOutcome::Aborted
    /// instead. A no-op in every other state.
    void cancel();

    /// Returns to Idle, discarding any previous result and log. No-op while
    /// Running -- there is nothing safe to discard mid-flight. Intended for
    /// a caller (FlashDialog::open()) that reopens the same controller for a
    /// new entry and must not show the previous flash's leftover outcome
    /// before Start is clicked again.
    void reset();

    /// Drains whatever the worker thread queued since the last call and
    /// applies it (via onProgress()/onFinished()). Never blocks: it only
    /// holds the mutex for the swap, not for the duration of any I/O. Call
    /// once per UI frame regardless of state().
    void poll();

    FlashState state() const { return m_state; }
    const std::vector<std::string>& progressLog() const { return m_log; }
    const FlashResult& result() const { return m_result; }

    /// The most recent progress event, or nullopt before any has arrived
    /// (Idle, or Running but the worker hasn't reported yet). Exposed
    /// alongside progressLog() so a caller (FlashDialog) can render a
    /// one-line "step N of M -- phase" readout above the scrolling log,
    /// distinct from the log's per-event history.
    ///
    /// Unlike progressLog(), this DOES track the periodic refreshes a bounded
    /// wait emits (FlashProgress::isRefresh) -- they are the whole reason the
    /// readout can keep moving through a thirty-second wait instead of
    /// freezing on one line. Each refresh is self-describing, so rendering
    /// from this alone is always correct.
    std::optional<FlashProgress> lastProgress() const { return m_lastProgress; }

    /// Overall completion of the current flash, 0..1, for a determinate
    /// progress bar. 0 before anything has been reported; exactly 1 only once
    /// a Success result has landed.
    ///
    /// MONOTONIC for the whole life of one flash: it is the high-water mark of
    /// flashProgressFraction() over every event seen, so it can only ever go
    /// up. Two things need that, and neither is hypothetical:
    ///
    ///  - A resume. confirm() restarts the engine at the step that stopped on
    ///    NeedsConfirmation, and that step re-reports StepStarted/Loading/
    ///    Verifying -- work it had already done and already shown before it
    ///    paused. The plan has not regressed just because one step is being
    ///    re-evaluated, and a bar that slid backwards would say it had. Hence
    ///    confirm() deliberately does NOT reset this; begin() and reset(), the
    ///    two entry points that genuinely start over, do.
    ///
    ///  - A failure. The bar stops where the flash stopped and stays there,
    ///    rather than snapping to full or to empty. Where it stopped is true.
    float progressFraction() const { return m_progressFraction; }

    /// The CpuIdentity the next (or current) worker runs against -- whatever
    /// begin() or confirm() was last given. Exposed so a test can assert the
    /// resume path actually adopts confirm()'s freshly-read identity instead of
    /// silently reusing begin()'s snapshot, which is the whole point of
    /// confirm() taking one; makeProductionFlashIo(identity) is what carries it
    /// into the engine as io.identify(). Read-only, and not a substitute for
    /// the caller's own selectionUnchanged() check.
    const CpuIdentity& identity() const { return m_identity; }

    /// recoveryAnchorFor(result().outcome, <the CPU whose step actually
    /// produced it>). The CPU comes from lastProgress(): every failure path
    /// in runFlashPlan() reports at least that step's StepStarted before it
    /// can fail, and a release-wait Timeout's last progress event
    /// (WaitingForRelease) still carries the just-finished step's CPU -- the
    /// one whose volume did not release -- so this is correct for both
    /// causes of FlashOutcome::Timeout, not only the "volume never
    /// appeared" one.
    ///
    /// The step counts come from result().stepsCompleted and from
    /// lastProgress()->stepCount -- the plan size as the ENGINE saw it, which
    /// is what stepsCompleted is relative to -- falling back to the plan this
    /// controller holds when no progress event has arrived at all.
    std::optional<RecoveryAnchor> recoveryAnchor() const;

    // --- Applied directly by poll(); exercised directly by tests so the
    // state machine is verifiable without a real worker thread. ---
    void onProgress(const FlashProgress& progress);
    void onFinished(const FlashResult& result);

private:
    struct QueueEntry {
        bool          isFinal = false;
        FlashProgress progress;
        FlashResult   result;
    };

    /// `prepare` runs quietDisplayBeforeMainWrite() ahead of the plan; true
    /// from begin(), false from confirm() -- see confirm()'s body for why.
    void startWorker(std::string typedConfirmation, std::size_t startIndex, bool prepare);
    void enqueueProgress(const FlashProgress& p);
    void enqueueResult(const FlashResult& r);

    FlashState  m_state = FlashState::Idle;
    FlashResult m_result;
    std::vector<std::string> m_log;
    /// The most recent progress event -- see lastProgress()/recoveryAnchor().
    std::optional<FlashProgress> m_lastProgress;
    /// High-water mark of flashProgressFraction() -- see progressFraction().
    float m_progressFraction = 0.0f;

    CpuIdentity            m_identity;
    uint64_t               m_uniqueID = 0;   ///< begin()'s board handle; 0 = static identity
    std::vector<FlashStep> m_plan;   // the ORIGINAL, un-sliced plan -- see confirm()

    std::atomic<bool> m_cancelRequested{ false };
    std::thread       m_worker;

    std::mutex              m_queueMutex;
    std::vector<QueueEntry> m_queue;
};

} // namespace fwog
