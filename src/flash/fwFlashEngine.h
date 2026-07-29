#pragma once

#include "core/fwTypes.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fwog {

/// Every piece of I/O the engine performs. Production supplies the real
/// platform functions; tests supply fakes. Nothing else in the engine touches
/// a device, a disk or a socket.
struct FlashIo {
    std::function<CpuIdentity()>               identify;
    std::function<std::vector<std::string>()>  findVolumes;
    std::function<void(const std::string&)>    touchPort;

    std::function<std::expected<std::vector<uint8_t>, std::string>(const ImageRef&)>
        loadImage;

    /// Write bytes to a real file the OS can copy from, and return its path.
    std::function<std::expected<std::filesystem::path, std::string>(
        std::span<const uint8_t>, const std::string& filename)> stageFile;

    std::function<std::expected<void, std::string>(
        const std::filesystem::path&, const std::string& volume)> copyToVolume;

    /// Called once per poll interval while waiting for a volume. Return false
    /// to abort (user cancelled). The delay itself belongs to the caller, so
    /// tests run instantly.
    std::function<bool(int intervalMs)> waitTick;
};

enum class FlashOutcome {
    Success,
    RefusedUnidentified,   ///< the target CPU could not be found
    RefusedAmbiguous,      ///< two or more RPI-RP2 volumes
    /// The one mounted RPI-RP2 volume is KNOWN, from a verified CPU probe, to
    /// be the CPU this step does NOT target. Distinct from RefusedAmbiguous on
    /// purpose: nothing here is ambiguous. The app is refusing because it knows
    /// the answer and the answer is no.
    RefusedWrongCpu,
    NeedsConfirmation,     ///< a foreign volume; user must type the CPU name
    BadImage,              ///< could not load, or failed UF2 validation
    VerifyFailed,          ///< sha256 mismatch
    CopyFailed,
    Timeout,               ///< no volume appeared after the touch, or one that
                            ///< should have released after a copy did not
    Aborted,               ///< user cancelled
    InvalidArgument,       ///< a caller precondition was violated, e.g.
                            ///< startIndex > plan.size(); not a hardware
                            ///< failure, nothing was touched or written
};

/// NOTE: this enumeration's DECLARATION order is not the order the phases are
/// REPORTED in. StepFinished is reported the moment the copy succeeds, and
/// WaitingForRelease only afterwards -- see runFlashPlan()'s comment above its
/// StepFinished report for why the release wait is preparation for the NEXT
/// step rather than part of finishing this one. Anything that needs a
/// progression (a progress bar, most obviously) must therefore go through
/// flashPhaseFraction() below and must never use the enumerator's own value as
/// an ordinal: doing so makes a bar run backwards on every step but the last.
enum class FlashPhase {
    StepStarted, Loading, Verifying, Touching, WaitingForVolume, Copying,
    WaitingForRelease,   ///< after a successful copy, waiting for THIS step's
                         ///< volume to disappear before the next step looks
    StepFinished
};

struct FlashProgress {
    FlashPhase  phase = FlashPhase::StepStarted;
    size_t      stepIndex = 0;
    size_t      stepCount = 0;
    TargetCpu   cpu = TargetCpu::Main;
    std::string message;

    /// True only for the periodic updates emitted from INSIDE a bounded wait
    /// (WaitingForVolume, WaitingForRelease) while it polls. Such an update
    /// repeats a phase that has already been announced by a preceding report
    /// with `isRefresh == false`, and exists purely so a live readout can keep
    /// moving; it says nothing new about what happened.
    ///
    /// A consumer keeping a PERMANENT RECORD -- FlashController's scrolling
    /// log, which the user reads afterwards to work out what went wrong --
    /// must skip these. A thirty-second wait polls 120 times, and 120 lines of
    /// "waiting..." would bury the one line that matters. A consumer showing
    /// the CURRENT state (a status line, a progress bar) should use them: they
    /// are the entire point of the exercise.
    ///
    /// Every refresh carries the same phase/step/cpu/message as the
    /// announcement it repeats, so a consumer can render one from the refresh
    /// alone and never has to remember an earlier event to make sense of it.
    bool isRefresh = false;

    /// How far into a bounded wait this report was emitted, and how long the
    /// wait is allowed to run in total. Both zero for every phase that is not
    /// a wait, so `waitTotalMs > 0` is the test for "a countdown is available".
    ///
    /// `waitElapsedMs` counts the wait's OWN budget clock -- the same
    /// kVolumePollMs increments the kVolumeWaitMs timeout is measured against
    /// -- not wall-clock time. It therefore lags a stopwatch slightly (each
    /// poll also costs a findVolumes() call), which is correct for what it is
    /// used to say: how much of the allowance this wait has spent.
    int waitElapsedMs = 0;
    int waitTotalMs   = 0;
};

using ProgressFn = std::function<void(const FlashProgress&)>;

struct FlashResult {
    FlashOutcome outcome = FlashOutcome::Success;
    std::string  message;
    size_t       stepsCompleted = 0;
    /// Set only when outcome is NeedsConfirmation: the CPU the user must name.
    std::optional<TargetCpu> confirmationCpu;
    /// True only when resuming with `startIndex = stepsCompleted` is safe.
    /// False after any outcome that leaves a volume mounted or the board in
    /// an unknown state -- resuming from those would let the next step meet
    /// the previous step's still-mounted volume and copy to the wrong CPU.
    /// Only NeedsConfirmation sets this true: it is the one outcome where
    /// `stepsCompleted` steps genuinely finished, including each one's
    /// release wait, and step `stepsCompleted` itself never touched
    /// anything. Success also leaves this false, deliberately: there is
    /// nothing left to resume.
    bool resumable = false;
};

/// How long to wait for a volume after touching. A touch works in about a
/// second or it did not work, so a longer wait only delays the error.
constexpr int kVolumeWaitMs     = 30000;
constexpr int kVolumePollMs     = 250;

/// How much of ONE step is complete once `phase` has been reported, in 0..1.
///
/// Ordered by the order the phases are actually REPORTED in, which is not the
/// order FlashPhase declares them in -- see FlashPhase's comment. The values
/// are fixed anchors rather than a computed ratio so that the sequence is
/// legible here and so that a bar built on it cannot go backwards: they are
/// strictly increasing along the reporting order, and that is a property this
/// is tested for.
///
/// It deliberately stops short of 1.0. StepFinished means "this step's image
/// landed on the board", which is the last thing the FINAL step of a plan ever
/// reports; a plan is only complete when runFlashPlan() returns Success, and
/// claiming 100% before that has been established would be the same kind of
/// premature certainty this dialog has already been burned by. The remaining
/// headroom is what the release wait fills for every non-final step, since
/// until that volume has gone the next step cannot begin.
float flashPhaseFraction(FlashPhase phase);

/// Overall completion of a whole plan, in 0..1:
/// `(stepIndex + flashPhaseFraction(phase)) / stepCount`.
///
/// `stepIndex`/`stepCount` are always relative to the ORIGINAL, un-sliced plan
/// even when runFlashPlan() was resumed at a non-zero `startIndex` (see
/// runFlashPlan's comment), so a resumed flash picks up where it left off
/// instead of restarting at zero.
///
/// It deliberately IGNORES `waitElapsedMs`. A wait is bounded, but its end is
/// not predictable -- the volume can appear on the first poll or never -- so
/// advancing the overall bar as the wait's clock runs down would state that
/// the flash is getting closer to finishing when what it is actually getting
/// closer to is a timeout. The wait's own elapsed/total is reported separately
/// and is a fact; this stays put until something really happens.
///
/// Returns 0 for an empty plan (`stepCount == 0`), which never reports
/// progress at all, and clamps a `stepIndex` at or past `stepCount` to the
/// last step rather than returning a fraction above 1.
float flashProgressFraction(const FlashProgress& progress);

/// Execute a plan, step by step, aborting on the first failure.
///
/// `typedConfirmation` is what the user typed in the confirmation box, or
/// empty. It is consumed by any step that lands on a foreign mounted volume.
///
/// `startIndex` resumes a plan at step `startIndex` instead of step 0. Pass
/// `result.stepsCompleted` from a prior result here, with the SAME (full,
/// original) `plan`, once the user has typed the confirmation -- do not
/// slice `plan` down and call with `startIndex = 0`, and do not call with
/// `startIndex = 0` again on the full plan either: steps before `startIndex`
/// already ran real, irreversible I/O (a touch and a copy) and must not run
/// again. Because `plan` stays the original, un-sliced span, `stepsCompleted`
/// and the `stepIndex`/`stepCount` seen by `progress` are always relative to
/// the original plan, not to whatever step the caller resumed from.
///
/// `startIndex` MUST only be a `stepsCompleted` taken from a result whose
/// `resumable` was true -- in practice, only a `NeedsConfirmation` result.
/// Resuming from any other non-success result's `stepsCompleted` can run a
/// step against a volume the previous, failed step left mounted, which can
/// copy that step's image onto the wrong CPU. `startIndex > plan.size()`
/// fails with `InvalidArgument` rather than being clamped or silently
/// treated as nothing-to-do.
FlashResult runFlashPlan(const FlashIo& io,
                         std::span<const FlashStep> plan,
                         std::string_view typedConfirmation,
                         const ProgressFn& progress,
                         std::size_t startIndex = 0);

} // namespace fwog
