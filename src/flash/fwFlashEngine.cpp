#include "flash/fwFlashEngine.h"

#include "catalog/fwUf2Header.h"
#include "core/fwSha256.h"
#include "flash/fwVolumeState.h"

#include <algorithm>

namespace fwog {
namespace {

const char* cpuName(TargetCpu cpu)
{
    return cpu == TargetCpu::Main ? "MAIN" : "DISPLAY";
}

std::string stagedName(const FlashStep& step, size_t index)
{
    if (!step.image.localPath.empty())
        return std::filesystem::path(step.image.localPath).filename().string();
    if (!step.image.embeddedId.empty())
        return step.image.embeddedId + ".uf2";
    return "image" + std::to_string(index) + ".uf2";
}

FlashResult fail(FlashOutcome outcome, std::string message, size_t done)
{
    FlashResult r;
    r.outcome = outcome;
    r.message = std::move(message);
    r.stepsCompleted = done;
    return r;
}

/// The sentence appended to every failure message when the plan had already
/// written something before it stopped. Empty when `done == 0`, which is the
/// case the per-outcome messages below word for themselves ("... so nothing was
/// written").
///
/// This exists because those messages used to be written as though the failure
/// could only ever happen on step 0, and were then reused verbatim for step 1.
/// The real path: an Original FreeWili (LegacyDirect) install is
/// [MAIN, DISPLAY]; step 0 succeeds, which is exactly what DESTROYS the display
/// bootloader; step 1 then refuses because the DISPLAY port was not identified
/// -- highly likely right there, since the firmware just installed publishes no
/// FWOG_* product string and a running MAIN CPU suppresses the display console
/// outright. The user was told "nothing was written". That is false, and the
/// board is in a mixed state they have been given no reason to suspect.
///
/// The design spec's requirement is that "the failure message names exactly
/// which steps completed", so this names the CPUs rather than only counting
/// steps: which CPU was overwritten is the thing that decides what the user
/// does next.
std::string completedStepsNote(std::span<const FlashStep> plan, size_t done)
{
    if (done == 0) return {};

    std::string cpus;
    std::string erased;
    size_t eraseCount = 0;
    for (size_t k = 0; k < done && k < plan.size(); ++k) {
        if (k > 0) cpus += (k + 1 == done) ? " and the " : ", the ";
        cpus += cpuName(plan[k].cpu);
        if (plan[k].action == StepAction::Erase) {
            erased += (eraseCount == 0) ? " Step " : " and step ";
            erased += std::to_string(k + 1);
            ++eraseCount;
        }
    }

    std::string s = " Note that ";
    s += (done == 1) ? "step 1 of " : ("steps 1-" + std::to_string(done) + " of ");
    s += std::to_string(plan.size());
    s += " had already completed, so this is not an untouched board: the ";
    s += cpus;
    s += (done == 1) ? " CPU has" : " CPUs have";
    s += " already been written.";
    // An erase step DID write something (flash_nuke.uf2), so the sentence
    // above stays true -- but "written" on its own would let a user picture
    // firmware sitting there. Said separately rather than by rewording the
    // sentence above, which several failure messages are worded against.
    if (eraseCount > 0) {
        s += erased;
        s += (eraseCount == 1) ? " of those ERASED its CPU" : " of those ERASED their CPUs";
        s += " rather than installing firmware: blank flash, sitting in the RP2040's "
             "own bootloader, running nothing.";
    }
    s += " See the Recovery tab before retrying.";
    return s;
}

void report(const ProgressFn& progress, FlashPhase phase, size_t i, size_t n,
            TargetCpu cpu, std::string message)
{
    if (progress) progress(FlashProgress{ phase, i, n, cpu, std::move(message) });
}

enum class WaitOutcome { Ready, TimedOut, Cancelled, Ambiguous };

/// Everything a wait needs in order to describe itself WHILE it runs, rather
/// than only once before it starts.
///
/// This exists because the two waits are the longest-running part of a flash
/// and used to be the least informed: the single `report(...WaitingForVolume)`
/// fired before the loop and then nothing moved for up to thirty seconds. A
/// real LegacyDirect run failed exactly there -- the DISPLAY volume never
/// appeared -- and the user had one frozen line of text to work out from
/// whether the app was progressing, hung, or nearly done. It was none of the
/// three: it was counting down to a timeout, and nothing on screen said so.
///
/// The wait therefore emits its own elapsed/total as it polls. That is a fact
/// about a bounded thing, which is why it is safe to draw as a determinate bar
/// -- unlike the overall progress bar, which stays put through the wait
/// because how close a flash is to FINISHING is not something a wait clock
/// knows anything about (see flashProgressFraction's comment).
///
/// Every field is fixed for the life of one wait; only the elapsed count
/// changes. `message` is repeated verbatim on every refresh so each report
/// stands on its own -- FlashProgress documents that guarantee, and a consumer
/// showing "where things are right now" from lastProgress() alone depends on
/// it.
struct WaitReporter {
    const ProgressFn& progress;
    FlashPhase        phase;
    size_t            stepIndex;
    size_t            stepCount;
    TargetCpu         cpu;
    std::string       message;

    /// The phase transition itself: emitted once, before the first poll, and
    /// the only one of these reports that reaches the scrolling log.
    void announce() const { emit(0, /*refresh=*/false); }

    /// A periodic update from inside the loop. Marked isRefresh so the log
    /// skips it -- 120 of these per timed-out wait would drown the record the
    /// log exists to keep.
    void refresh(int elapsedMs) const { emit(elapsedMs, /*refresh=*/true); }

    void emit(int elapsedMs, bool refresh) const
    {
        if (!progress) return;
        FlashProgress p;
        p.phase         = phase;
        p.stepIndex     = stepIndex;
        p.stepCount     = stepCount;
        p.cpu           = cpu;
        p.message       = message;
        p.isRefresh     = refresh;
        p.waitElapsedMs = elapsedMs;
        p.waitTotalMs   = kVolumeWaitMs;
        progress(p);
    }
};

/// Poll `io.findVolumes()` every `kVolumePollMs`, bounded by `kVolumeWaitMs`,
/// until a volume is present (`wantPresent = true`) or absent
/// (`wantPresent = false`). Two or more volumes ever being seen aborts the
/// wait immediately as Ambiguous, matching `classifyVolumes`: neither the
/// tools nor the operator can tell RP2040 bootrom volumes apart, so waiting
/// any longer only delays that refusal. `volumes` holds the latest poll
/// result on every return path, including Ambiguous/TimedOut/Cancelled.
///
/// Used both to wait for a just-touched CPU's volume to appear, and -- after
/// a copy -- to wait for that same volume to disappear again before the next
/// step looks at the mounts. Do not duplicate this loop; the two call sites
/// exist specifically so both waits share one cancellation- and ambiguity-
/// aware polling implementation. That is also why `reporter` is a parameter
/// rather than the announcement staying at the call sites: both waits now get
/// live progress from the one implementation, and neither can drift from the
/// other.
///
/// `reporter.announce()` happens here, before the first poll, so the "we are
/// now waiting" report and the wait itself cannot get out of order. The
/// refresh is emitted AFTER the cancellation check and the elapsed increment,
/// which keeps two things true: a cancel still returns on the same tick it is
/// noticed, and a wait that is satisfied by its very first poll emits no
/// refresh at all.
/// Wait for a volume that was NOT mounted at `before` to appear, and return it.
///
/// This is the whole of how a touched CPU's drive is identified, and it is
/// identified by CAUSATION: we recorded the mounted set, we touched one known
/// port, and the drive that then appeared is the CPU on the other end of that
/// port. Nothing about the resulting drive's own contents, letter or label is
/// consulted, because none of that distinguishes one RP2040 bootrom from
/// another -- the timing and the cause do.
///
/// What this deliberately does NOT do is count pre-existing drives as an
/// ambiguity. The predecessor waited for the mounted set to be exactly one and
/// refused at two, which meant a board with its other CPU already sitting in
/// BOOTSEL could never be flashed at all: the drive that was already there,
/// which this step is not going to write to and does not care about, made every
/// touch look ambiguous. Only two drives arriving from ONE touch is a real
/// ambiguity, and that is what is refused.
struct NewVolumeWait {
    WaitOutcome outcome = WaitOutcome::TimedOut;
    std::string volume;   ///< set only when outcome == Ready
};

NewVolumeWait waitForNewVolume(const FlashIo& io, const std::vector<std::string>& before,
                               const WaitReporter& reporter)
{
    reporter.announce();

    const auto arrivals = [&before](const std::vector<std::string>& now) {
        std::vector<std::string> fresh;
        for (const auto& v : now)
            if (std::find(before.begin(), before.end(), v) == before.end())
                fresh.push_back(v);
        return fresh;
    };

    int waited = 0;
    for (;;) {
        auto fresh = arrivals(io.findVolumes());
        if (fresh.size() >= 2) return { WaitOutcome::Ambiguous, {} };
        if (fresh.size() == 1) return { WaitOutcome::Ready, std::move(fresh.front()) };
        if (waited >= kVolumeWaitMs) return { WaitOutcome::TimedOut, {} };
        if (!io.waitTick(kVolumePollMs)) return { WaitOutcome::Cancelled, {} };
        waited += kVolumePollMs;
        reporter.refresh(waited);
    }
}

/// Wait for ONE named volume to go away, ignoring every other mount.
///
/// Also a correctness fix rather than a tidy-up: the predecessor waited for the
/// mounted set to become EMPTY, so any unrelated drive -- the other CPU in
/// BOOTSEL, a second board, an SD card reader that happens to present RPI-RP2
/// -- made this wait run its full thirty seconds and then fail a step whose
/// write had already succeeded. The only thing this step needs to know is that
/// ITS drive has released.
/// `remaining` is the mounted set OBSERVED at the moment the volume was seen to
/// be gone, and it is not a diagnostic -- it is what the next step uses as the
/// `before` of its own delta wait when this step was an erase. It has to be
/// captured HERE rather than re-read by that step: an erased RP2040
/// re-enumerates on its own, and if it wins the race back the next step's
/// top-of-loop findVolumes() would already include it, so a delta taken there
/// would see no arrival at all and wait out the full timeout for a drive that
/// was sitting in front of it.
struct VolumeGoneWait {
    WaitOutcome              outcome = WaitOutcome::TimedOut;
    std::vector<std::string> remaining;
};

VolumeGoneWait waitForVolumeGone(const FlashIo& io, const std::string& volume,
                                 const WaitReporter& reporter)
{
    reporter.announce();

    int waited = 0;
    for (;;) {
        auto now = io.findVolumes();
        if (std::find(now.begin(), now.end(), volume) == now.end())
            return { WaitOutcome::Ready, std::move(now) };
        if (waited >= kVolumeWaitMs) return { WaitOutcome::TimedOut, {} };
        if (!io.waitTick(kVolumePollMs)) return { WaitOutcome::Cancelled, {} };
        waited += kVolumePollMs;
        reporter.refresh(waited);
    }
}

} // namespace

float flashPhaseFraction(FlashPhase phase)
{
    // In REPORTING order, which is not FlashPhase's declaration order:
    // StepFinished is reported the instant the copy succeeds and
    // WaitingForRelease only after it. Using the enumerator as an ordinal
    // instead would run a bar backwards on every step but the last.
    //
    // No `default:`, deliberately, so that adding a phase is a compile error
    // here rather than a silent 0.0f that would make the bar jump to the start
    // of the step. Same reason the guard and recovery switches elsewhere in
    // this project have none.
    switch (phase) {
    case FlashPhase::StepStarted:       return 0.00f;
    case FlashPhase::Loading:           return 0.10f;
    case FlashPhase::Verifying:         return 0.25f;
    case FlashPhase::Touching:          return 0.35f;
    case FlashPhase::WaitingForVolume:  return 0.45f;
    case FlashPhase::Copying:           return 0.60f;
    // The write landed. Everything up to here can still be undone by a refusal
    // that leaves the board untouched; from here on it cannot.
    case FlashPhase::StepFinished:      return 0.85f;
    // Reported only for a non-final step, and only after StepFinished. The
    // step's image is already on the board; what is left is the board letting
    // go of the volume so the next step can look at the mounts safely.
    case FlashPhase::WaitingForRelease: return 0.90f;
    }
    return 0.0f;
}

float flashProgressFraction(const FlashProgress& progress)
{
    if (progress.stepCount == 0) return 0.0f;
    // Clamp rather than trust: a stepIndex at or past stepCount cannot come
    // from runFlashPlan (the loop condition forbids it), but this is drawn
    // straight into a progress bar and a fraction above 1 is not something a
    // caller should have to defend against.
    const size_t index = (progress.stepIndex < progress.stepCount)
                       ? progress.stepIndex
                       : progress.stepCount - 1;
    return (static_cast<float>(index) + flashPhaseFraction(progress.phase))
         / static_cast<float>(progress.stepCount);
}

FlashResult runFlashPlan(const FlashIo& io,
                         std::span<const FlashStep> plan,
                         std::string_view typedConfirmation,
                         const ProgressFn& progress,
                         std::size_t startIndex)
{
    const size_t n = plan.size();
    if (startIndex > n)
        return fail(FlashOutcome::InvalidArgument,
                    "startIndex (" + std::to_string(startIndex) + ") is greater "
                    "than the plan size (" + std::to_string(n) + "). This is a "
                    "caller bug: nothing was touched or written.", 0);

    // Every in-loop failure goes through this rather than calling fail()
    // directly, so no message can be worded as though the plan stopped on step
    // 0 when it did not. `done` is the same stepsCompleted the result carries
    // (i for a failure before this step's copy, i + 1 for one after it), which
    // is exactly what completedStepsNote() needs -- see its comment.
    //
    // NeedsConfirmation is deliberately NOT routed through here: it is a prompt
    // in the middle of a plan that is still going, not a report on a plan that
    // stopped. It never claims anything was or was not written, and appending
    // "See the Recovery tab before retrying" to a message whose entire purpose
    // is "type MAIN and press Proceed" would send the user the wrong way.
    auto failStep = [&plan](FlashOutcome outcome, std::string message, size_t done) {
        return fail(outcome, std::move(message) + completedStepsNote(plan, done), done);
    };

    // Set at the very end of an iteration whose step ERASED its CPU and whose
    // release wait then reported Ready -- that is, we watched every RPI-RP2
    // volume go away after the erase image landed. Consumed by the NEXT
    // iteration and by nothing else: it is read once, at the top, and cleared
    // there whether or not it applied.
    //
    // Why this exists (item 6 of the DISPLAY-first change). An RP2040 whose
    // flash has just been erased re-enumerates RPI-RP2 ON ITS OWN, with no
    // button and no 1200-baud touch. In the deprecated-firmware plan --
    // [ERASE DISPLAY, WRITE DISPLAY, WRITE MAIN] -- step 2 therefore arrives
    // to find either no volume yet (the erase is still running) or a volume
    // nobody touched. Without this, the first case refuses
    // (RefusedUnidentified: an erased CPU has no serial port to identify) and
    // the second demands a typed DISPLAY confirmation, in the middle of a plan
    // that is behaving exactly as designed. Both are wrong, and the second is
    // worse than merely annoying: a confirmation prompt that fires on the
    // happy path is how users learn to type confirmations without reading
    // them, which is the one habit every guard in this file depends on them
    // NOT having.
    //
    // What makes it safe to skip the confirmation here, stated plainly:
    //  - The release wait proved the previous step's volume was GONE. Anything
    //    mounted now appeared after that moment.
    //  - The only thing this plan knows to be rebooting in that window is the
    //    CPU it just erased.
    //  - Nothing else on this board enters BOOTSEL unprompted. The other CPU
    //    is running whatever it was running; reaching its bootloader takes a
    //    physical button press.
    //  - Two volumes still refuse, in classifyVolumes() and again mid-wait. A
    //    hand-pressed BOOTSEL landing inside this window shows up as the
    //    second volume, and is caught there.
    // That is precisely the same strength of evidence the engine already
    // accepts for OursAfterTouch ("we touched MAIN, one volume appeared,
    // therefore it is MAIN"), applied to a reboot this plan caused a different
    // way. ForeignMounted is untouched for every other case, which is the one
    // it was written for: a board the user left in BOOTSEL by hand.
    std::optional<TargetCpu> eraseRebootExpectedOn;

    /// The mounted set as it stood the instant the previous step's volume was
    /// observed to release. Paired with eraseRebootExpectedOn and only read
    /// when it is set: it is the `before` an erase-reboot's delta wait measures
    /// arrivals against. See VolumeGoneWait::remaining for why it cannot be
    /// re-derived at the top of the next iteration.
    std::vector<std::string> volumesAtRelease;

    for (size_t i = startIndex; i < n; ++i) {
        const FlashStep& step = plan[i];
        const char* cpu = cpuName(step.cpu);

        // Read once and cleared unconditionally: it is a statement about the
        // step that just ran, and it expires the moment this one begins. Note
        // startIndex -- a resumed call starts with it empty, so a resume can
        // never inherit a claim about a step it did not itself observe.
        const bool expectedFromPriorErase =
            eraseRebootExpectedOn.has_value() && *eraseRebootExpectedOn == step.cpu;
        eraseRebootExpectedOn.reset();
        // Moved out and cleared with it, so the two can never be read apart:
        // a stale release snapshot applied to an unrelated step would measure
        // arrivals against the wrong baseline.
        const std::vector<std::string> priorReleaseVolumes = std::move(volumesAtRelease);
        volumesAtRelease.clear();

        report(progress, FlashPhase::StepStarted, i, n, step.cpu, step.description);

        // --- Load the bytes -------------------------------------------------
        report(progress, FlashPhase::Loading, i, n, step.cpu, "loading the image");
        const auto bytes = io.loadImage(step.image);
        if (!bytes)
            return failStep(FlashOutcome::BadImage,
                            "could not load the image: " + bytes.error(), i);

        // --- Validate before anything irreversible --------------------------
        report(progress, FlashPhase::Verifying, i, n, step.cpu, "checking the image");
        const auto info = parseUf2(*bytes);
        if (!info)
            return failStep(FlashOutcome::BadImage, uf2ErrorMessage(info.error()), i);

        if (!step.sha256.empty()) {
            const std::string actual = sha256Hex(*bytes);
            if (actual != step.sha256)
                // "was not written" is scoped to THIS step's image, which is
                // true whatever i is; completedStepsNote() supplies the rest of
                // the picture when earlier steps did write something.
                return failStep(FlashOutcome::VerifyFailed,
                                "the image does not match its published checksum, so it "
                                "was not written; the download may be corrupt", i);
        }

        // --- Decide what the mounted volumes mean ---------------------------
        auto volumes = io.findVolumes();
        const auto identity = io.identify();
        const auto port = (step.cpu == TargetCpu::Main) ? identity.mainPort
                                                        : identity.displayPort;
        // The OTHER CPU's port is evidence too -- see decideAction(). A CPU
        // publishing a serial port is running firmware and so is none of the
        // mounted bootrom drives, which is what lets a single drive be named by
        // elimination instead of by asking the user.
        const auto otherPort = (step.cpu == TargetCpu::Main) ? identity.displayPort
                                                             : identity.mainPort;

        const VolumeState state = classifyVolumes(volumes, /*weTouched=*/false,
                                                  expectedFromPriorErase, identity, step.cpu);
        const GuardAction action = decideAction(state, port.has_value(),
                                                otherPort.has_value());

        // The one drive this step will write to, established by whichever arm
        // of the switch below runs. Every arm must set it, and nothing after
        // the switch may fall back to "the only volume mounted" -- that
        // assumption is exactly what this change exists to remove, since there
        // is now routinely more than one drive mounted and only one of them is
        // ours.
        std::string targetVolume;

        // Set only by GuardAction::WriteMappedVolume below, and read only by
        // the Copying report: a write that skipped both the touch and the typed
        // confirmation must say in the log WHY it was allowed to, or the record
        // the user reads afterwards shows an image appearing on a drive with no
        // stated reason.
        bool viaVerifiedProbe = false;

        switch (action) {
        case GuardAction::RefuseAmbiguous:
            return failStep(FlashOutcome::RefusedAmbiguous,
                            "two RPI-RP2 volumes are mounted. Both RP2040s present the "
                            "same bootrom serial, so they cannot be told apart, and this "
                            "step was not attempted. Unmount one and try again.", i);

        case GuardAction::RefuseUnidentified:
            // Deliberately "this step wrote nothing" rather than the flat
            // "nothing was written" this used to say: on a LegacyDirect plan
            // this is overwhelmingly the step-1 refusal that follows a
            // SUCCESSFUL step 0, and completedStepsNote() spells out what that
            // means. See its comment.
            return failStep(FlashOutcome::RefusedUnidentified,
                            std::string("the ") + cpu + " CPU could not be identified, so "
                            "this step wrote nothing. See the Recovery tab, or put that "
                            "CPU into BOOTSEL by hand and try again.", i);

        case GuardAction::RefuseWrongCpu: {
            const char* mine = cpuName(step.cpu);
            const char* theirs = cpuName(otherCpu(step.cpu));
            const IdentitySource src = otherCpu(step.cpu) == TargetCpu::Main
                                         ? identity.mainSource : identity.displaySource;
            const std::string how = src == IdentitySource::VerifiedProbe
                ? "the Recovery tab's CPU probe measured that, it is not a guess"
                : "it is on that CPU's port of the board's internal USB hub, which is "
                  "structural and does not depend on what firmware is loaded";
            return failStep(FlashOutcome::RefusedWrongCpu,
                            "the one RPI-RP2 volume mounted (" + volumes.front() + ") is the " +
                            theirs + " CPU -- " + how + " -- and this step writes to the " +
                            mine + " CPU, which is not answering on a port either. "
                            "Writing here would put this image on the wrong CPU, so nothing was "
                            "written. Flash the " + theirs + " CPU first, or unmount that volume.",
                            i);
        }

        case GuardAction::WriteMappedVolume:
            // Nothing to do before the copy, and that is the entire point.
            //
            // No touch: there is no firmware running on a CPU sitting in the
            // RP2040's bootrom, so there is no port to open at 1200 baud -- and
            // no reason to want one, since a touch's only job is to put the CPU
            // exactly where this one already is.
            //
            // No typed confirmation: the prompt asks the user to state which
            // CPU an unidentifiable mount belongs to. Hub position, or a
            // verified probe, has established that already -- both strictly
            // better evidence than the user's recollection -- and re-asking a
            // question that has been answered is how a confirmation becomes a
            // reflex.
            //
            // The volume comes from the IDENTITY, not from volumes.front(), and
            // that distinction is now load-bearing: MappedToTarget no longer
            // implies exactly one mount. A structurally located drive stays
            // valid with a second drive mounted beside it (both CPUs in BOOTSEL
            // is the ordinary case), and front() would then be a coin flip
            // between the two CPUs. classifyVolumes() only answers
            // MappedToTarget when this volume is among those mounted, so there
            // is nothing to wait for either.
            viaVerifiedProbe = true;
            if (const auto& mapped = volumeForCpu(identity, step.cpu); mapped)
                targetVolume = *mapped;
            break;

        case GuardAction::RequireTypedConfirmation: {
            if (!confirmationMatches(typedConfirmation, step.cpu)) {
                FlashResult r = fail(FlashOutcome::NeedsConfirmation,
                        std::string("an RPI-RP2 volume was already mounted. Which CPU "
                        "that is cannot be determined from the mount, so type ") + cpu +
                        " to confirm this is the right one.", i);
                r.confirmationCpu = step.cpu;
                // Step i never touched or copied anything on this path (the
                // guard refused before either), and every step before it
                // completed in full, including its release wait. Resuming at
                // startIndex = i (== stepsCompleted here) re-evaluates step i
                // fresh against the same still-mounted volume, this time with
                // the confirmation supplied -- safe, and the intended flow.
                r.resumable = true;
                return r;
            }
            targetVolume = volumes.front();
            break;   // confirmed: fall through to the copy, no touch needed
        }

        case GuardAction::WaitForEraseReboot: {
            // No touch, deliberately: the CPU whose flash the previous step
            // erased has no firmware and therefore no serial port to open at
            // 1200 baud. It does not need one -- blank flash IS the reason it
            // comes back as RPI-RP2. Same wait, same thirty-second budget,
            // same ambiguity check as every other wait; only the touch is
            // absent. If the volume is already there, the wait's first poll
            // sees it and returns immediately.
            const WaitReporter waiting{ progress, FlashPhase::WaitingForVolume, i, n,
                                        step.cpu,
                                        std::string("waiting for the erased ") + cpu +
                                        " CPU to return as an RPI-RP2 volume" };
            // Delta-based like the touch wait, and for the same reason: the
            // drive we are waiting for is the one that ARRIVES, and any drive
            // already sitting there belongs to something else.
            //
            // Measured against the set captured when the erase step's own
            // volume released, NOT against `volumes` read at the top of this
            // iteration: the erased CPU re-enumerates on its own and may well
            // have beaten us here, in which case it is already in `volumes` and
            // a delta taken from there would see no arrival at all.
            const auto arrived = waitForNewVolume(io, priorReleaseVolumes, waiting);
            switch (arrived.outcome) {
            case WaitOutcome::Ready:
                targetVolume = arrived.volume;
                break;
            case WaitOutcome::Ambiguous:
                return failStep(FlashOutcome::RefusedAmbiguous,
                                "two RPI-RP2 volumes appeared at once; they cannot be told "
                                "apart, so this step's image was not written.", i);
            case WaitOutcome::TimedOut:
                return failStep(FlashOutcome::Timeout,
                                std::string("the ") + cpu + " CPU was erased but never "
                                "came back as an RPI-RP2 volume. See the Recovery tab.", i);
            case WaitOutcome::Cancelled:
                return failStep(FlashOutcome::Aborted, "cancelled.", i);
            }
            break;
        }

        case GuardAction::TouchThenWait: {
            // Snapshotted BEFORE the touch, and this is the load-bearing line
            // of the whole one-click install: whatever is mounted right now is
            // by definition not the CPU we are about to reboot, so the drive
            // that appears after the touch is unambiguously ours no matter how
            // many others are sitting there.
            const std::vector<std::string> before = volumes;

            report(progress, FlashPhase::Touching, i, n, step.cpu,
                   std::string("rebooting the ") + cpu + " CPU into BOOTSEL");
            io.touchPort(*port);

            // The announcement that used to be a report() call right here now
            // belongs to the wait itself, which repeats it as it polls -- see
            // WaitReporter. This is the wait that a real LegacyDirect run sat
            // in, silently, for the full thirty seconds before failing.
            const WaitReporter waiting{ progress, FlashPhase::WaitingForVolume, i, n,
                                        step.cpu, "waiting for the RPI-RP2 volume" };
            const auto arrived = waitForNewVolume(io, before, waiting);
            switch (arrived.outcome) {
            case WaitOutcome::Ready:
                targetVolume = arrived.volume;
                break;
            case WaitOutcome::Ambiguous:
                return failStep(FlashOutcome::RefusedAmbiguous,
                                std::string("two RPI-RP2 volumes appeared at once after "
                                "rebooting the ") + cpu + " CPU, so which one is that CPU "
                                "cannot be told; this step's image was not written.", i);
            case WaitOutcome::TimedOut:
                return failStep(FlashOutcome::Timeout,
                                std::string("no new RPI-RP2 volume appeared after rebooting "
                                "the ") + cpu + " CPU. See the Recovery tab.", i);
            case WaitOutcome::Cancelled:
                // Terminating period like every other message here: failStep()
                // may append completedStepsNote(), and "cancelled Note that
                // step 1 of 2 had already completed..." runs two sentences
                // together.
                return failStep(FlashOutcome::Aborted, "cancelled.", i);
            }
            break;
        }

        // Unreachable given the single call to classifyVolumes() above: it is
        // always passed weTouched=false, so it can never produce
        // VolumeState::OursAfterTouch, the only state decideAction() maps to
        // Proceed. The "we created this volume, so proceed" case is instead
        // handled inline by the Ready outcome of the TouchThenWait wait
        // above. Kept, rather than deleted, so the switch stays exhaustive
        // over every GuardAction and a future enumerator addition fails to
        // compile here instead of silently falling through. Do not delete
        // this arm to "simplify" the switch, and do not treat its being
        // untested as a coverage gap to fill -- there is no way to reach it
        // through runFlashPlan's public API today.
        case GuardAction::Proceed:
            targetVolume = volumes.empty() ? std::string{} : volumes.front();
            break;
        }

        // Unreachable: every arm that breaks out of the switch above sets
        // targetVolume -- the two write-immediately arms and the confirmed
        // typed confirmation from the one volume ForeignMounted/MappedToTarget
        // guarantee, and the two waits from the drive they watched arrive.
        // Kept as a defensive backstop so that an arm added later which forgets
        // to name its volume fails loudly here instead of writing somewhere
        // unintended, not as a reachable branch to write a test for.
        if (targetVolume.empty())
            return failStep(FlashOutcome::Timeout, "the RPI-RP2 volume disappeared", i);

        // --- Copy -----------------------------------------------------------
        auto staged = io.stageFile(*bytes, stagedName(step, i));
        if (!staged)
            return failStep(FlashOutcome::CopyFailed,
                            "could not prepare the image: " + staged.error(), i);

        report(progress, FlashPhase::Copying, i, n, step.cpu,
               viaVerifiedProbe
                 ? ("writing to " + targetVolume + ", identified as the " + cpu + " CPU " +
                    ((step.cpu == TargetCpu::Main ? identity.mainSource : identity.displaySource)
                        == IdentitySource::VerifiedProbe
                       ? "by the Recovery tab's CPU probe"
                       : "by its port on the board's internal USB hub"))
                 : ("writing to " + targetVolume));
        if (auto copied = io.copyToVolume(*staged, targetVolume); !copied)
            return failStep(FlashOutcome::CopyFailed,
                            std::string("could not write the ") + cpu + " image: " +
                            copied.error() + ". A partial write may be on the board; "
                            "see the Recovery tab.", i);

        // The copy succeeding is what completes this step -- report it done
        // now, before the release wait below. The release wait is
        // preparation for the NEXT step, not part of finishing this one; a
        // step whose copy succeeded but whose release wait then failed still
        // reports StepFinished here, matching stepsCompleted crediting the
        // write itself (see the i + 1 passed to fail() below).
        report(progress, FlashPhase::StepFinished, i, n, step.cpu, "done");

        // --- Wait for this step's volume to release before the next step ----
        // copyToVolume() returning success means the bytes were written to
        // the mass-storage volume, NOT that the RP2040 has finished consuming
        // the file: on real hardware it takes hundreds of milliseconds to
        // several seconds to parse the UF2, reboot, and drop its
        // mass-storage endpoint. Without this wait, the NEXT step's
        // findVolumes() call would still see THIS step's volume, classify it
        // ForeignMounted, and -- in a LegacyDirect plan called with a
        // confirmation supplied for the whole plan up front -- silently
        // accept it and copy the next step's image onto the CPU this step
        // just finished flashing. That is a wrong-CPU write: the single
        // worst outcome this project can produce. Only the final step is
        // exempt; there is no following step to protect.
        //
        // A failure here (Timeout/Ambiguous/Aborted) still credits step i as
        // completed via stepsCompleted = i + 1 (the write did succeed), but
        // deliberately leaves FlashResult::resumable false: step i's volume
        // is, by definition of reaching this failure, still mounted, so
        // resuming at i + 1 would let the next step meet it and copy to the
        // wrong CPU -- the exact defect this wait exists to prevent, only
        // moved across the call boundary instead of closed.
        if (i + 1 < n) {
            const WaitReporter waiting{ progress, FlashPhase::WaitingForRelease, i, n, step.cpu,
                                        std::string("waiting for the ") + cpu +
                                        " volume to release" };
            // Watches THIS step's drive specifically, not "every drive is
            // gone". Another CPU sitting in BOOTSEL is none of this wait's
            // business, and waiting for it to leave would time out a step whose
            // write already succeeded.
            const auto released = waitForVolumeGone(io, targetVolume, waiting);
            switch (released.outcome) {
            case WaitOutcome::Ready:
                volumesAtRelease = released.remaining;
                break;
            case WaitOutcome::Ambiguous:
                // waitForVolumeGone() watches one named drive and has no
                // ambiguity to report; kept only so the switch stays exhaustive
                // over WaitOutcome.
                return failStep(FlashOutcome::RefusedAmbiguous,
                                std::string("the ") + cpu + " CPU's volume could not be "
                                "tracked while waiting for it to release, so the next step "
                                "was not attempted.", i + 1);
            case WaitOutcome::TimedOut:
                return failStep(FlashOutcome::Timeout,
                                std::string("the ") + cpu + " CPU's RPI-RP2 volume did not "
                                "disappear after being flashed, so the next step was not "
                                "attempted. See the Recovery tab.", i + 1);
            case WaitOutcome::Cancelled:
                return failStep(FlashOutcome::Aborted, "cancelled.", i + 1);   // period: see the other cancel path above
            }

            // Reached only on WaitOutcome::Ready -- every other outcome
            // returned above -- so this is set exactly when the erased CPU's
            // volume has been OBSERVED to go away, which is the precondition
            // the next step's confirmation-skip rests on. See
            // eraseRebootExpectedOn's comment. It is not set for the final
            // step, which has no release wait and no next step to inform.
            if (step.action == StepAction::Erase)
                eraseRebootExpectedOn = step.cpu;
        }
    }

    FlashResult r;
    r.outcome = FlashOutcome::Success;
    r.stepsCompleted = n;
    r.message = startIndex >= n ? "nothing to do" : "flashing finished";
    r.resumable = false;   // nothing left to resume
    return r;
}

} // namespace fwog
