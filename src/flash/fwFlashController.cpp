#include "flash/fwFlashController.h"

#include "catalog/fwCatalogEmbedded.h"
#include "platform/fwHttp.h"
#include "platform/fwPaths.h"
#include "platform/fwSerialTouch.h"
#include "platform/fwVolume.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>

namespace fwog {
namespace {

const char* cpuLabel(TargetCpu cpu) { return cpu == TargetCpu::Main ? "MAIN" : "DISPLAY"; }

/// "[step/count] CPU phase: message" -- includes p.message verbatim, which
/// is what lets the dialog's scrolling log double as the same text a user
/// would see reading fwFlashEngine.cpp's report() calls directly.
std::string formatProgress(const FlashProgress& p)
{
    std::string s = "[" + std::to_string(p.stepIndex + 1) + "/" + std::to_string(p.stepCount) + "] ";
    s += cpuLabel(p.cpu);
    s += " ";
    s += flashPhaseLabel(p.phase);
    if (!p.message.empty()) {
        s += ": ";
        s += p.message;
    }
    return s;
}

std::expected<std::vector<uint8_t>, std::string> readLocalFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::unexpected("could not open " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
        return std::unexpected("error reading " + path);
    return bytes;
}

/// Dispatches on which of ImageRef's three mutually-exclusive fields is set
/// -- see ImageRef's comment in fwTypes.h.
std::expected<std::vector<uint8_t>, std::string> loadImageProd(const ImageRef& ref)
{
    if (!ref.embeddedId.empty())
        return loadEmbeddedImage(ref.embeddedId);
    if (!ref.localPath.empty())
        return readLocalFile(ref.localPath);
    if (!ref.url.empty()) {
        auto text = httpGet(ref.url);
        if (!text)
            return std::unexpected(text.error());
        return std::vector<uint8_t>(text->begin(), text->end());
    }
    return std::unexpected("no image source was set for this step");
}

std::expected<std::filesystem::path, std::string>
stageFileProd(std::span<const uint8_t> bytes, const std::string& filename)
{
    const auto path = tempDir() / filename;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return std::unexpected("could not create " + path.string());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
        return std::unexpected("could not write " + path.string());
    return path;
}

} // namespace

FlashIo makeProductionFlashIo(const CpuIdentity& identity)
{
    FlashIo io;
    io.identify    = [identity] { return identity; };
    io.findVolumes = findRpiRp2Volumes;
    io.touchPort   = touchPort1200;
    io.loadImage   = loadImageProd;
    io.stageFile   = stageFileProd;
    // Wraps the real copyToVolume rather than binding it directly: the file
    // stageFileProd wrote into tempDir() is single-use (a fresh name per
    // step, from stagedName() in fwFlashEngine.cpp) and nothing else ever
    // deletes it. copy_file() inside copyToVolume() has already produced an
    // independent copy at the destination by the time it returns (success
    // or failure), so removing the source here is safe either way -- this
    // is "after a step completes or fails" without needing runFlashPlan
    // itself to know anything about staging or cleanup.
    io.copyToVolume = [](const std::filesystem::path& src, const std::string& volume)
        -> std::expected<void, std::string> {
            auto result = copyToVolume(src, volume);
            std::error_code ec;
            std::filesystem::remove(src, ec); // best-effort; result is what matters
            return result;
        };
    io.waitTick = [](int intervalMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        return true;
    };
    return io;
}

SelectionCheck selectionUnchanged(const std::optional<DeviceView>& current,
                                   uint64_t openedUniqueID,
                                   const std::string& openedSerial)
{
    if (!current.has_value()) return SelectionCheck::NothingSelected;
    if (current->uniqueID != openedUniqueID) return SelectionCheck::DifferentPort;
    // Ordered before the equality test, not folded into it: two unidentified
    // serials must never compare equal (that is the whole point of
    // serialIsUnidentified()), and separating the two answers costs nothing
    // in safety -- both refuse -- while letting the message say what is
    // actually known. See the SelectionCheck enum's comment.
    if (serialIsUnidentified(current->serial) || serialIsUnidentified(openedSerial))
        return SelectionCheck::UnidentifiedSerial;
    return current->serial == openedSerial ? SelectionCheck::Unchanged : SelectionCheck::DifferentBoard;
}

SelectionCheck selectionUnchangedFresh(const std::optional<DeviceView>& current,
                                        uint64_t openedUniqueID,
                                        const std::string& openedSerial,
                                        std::optional<std::chrono::milliseconds> snapshotAge,
                                        std::chrono::milliseconds maxAge)
{
    const SelectionCheck base = selectionUnchanged(current, openedUniqueID, openedSerial);
    // Only Unchanged is gated -- see this function's header comment for why a
    // refusal is passed through with its own specific wording instead.
    if (base != SelectionCheck::Unchanged) return base;
    // nullopt first, and deliberately not folded into the comparison below: an
    // unknown age is the LEAST verified state there is, and must refuse, not
    // default to fresh.
    if (!snapshotAge.has_value()) return SelectionCheck::StaleSnapshot;
    if (*snapshotAge > maxAge) return SelectionCheck::StaleSnapshot;
    return SelectionCheck::Unchanged;
}

std::string selectionCheckMessage(SelectionCheck check)
{
    switch (check) {
    case SelectionCheck::Unchanged:
        return {};
    case SelectionCheck::NothingSelected:
        return "No device is selected any more. Close and reopen to flash the current selection.";
    case SelectionCheck::DifferentBoard:
        return "A different device now occupies this USB port than when this dialog opened. Close and reopen to flash the current selection.";
    case SelectionCheck::UnidentifiedSerial:
        // Deliberately NOT the DifferentBoard wording. This state is reached
        // most often by ONE board part-way through re-enumeration, which
        // briefly reports no serial at all; asserting "a different device now
        // occupies this port" would state as fact something nobody knows and
        // send the user looking for a board swap that never happened. What IS
        // known is only that the board cannot be confirmed right now -- so say
        // that, and say that waiting is what resolves it. It refuses just as
        // hard either way: this is indistinguishable from a swap in progress,
        // and is treated as one.
        return "The board on this USB port is not reporting a serial number right now, so it cannot be confirmed as the same one this dialog opened on -- it may still be reconnecting. Wait for it to come back, or close and reopen to flash the current selection.";
    case SelectionCheck::DifferentPort:
        return "The selected device is no longer on the same USB port -- it may have moved or been unplugged. Close and reopen to flash the current selection.";
    case SelectionCheck::StaleSnapshot:
        // Worded as "we have not looked recently enough", not as "something
        // changed": nothing is known to have changed, and saying so would send
        // the user hunting for a problem that may not exist. It does not offer
        // "close and reopen" like the three above, because reopening would
        // re-read the same stale snapshot and change nothing -- waiting is the
        // action that actually resolves it.
        return "Waiting for a fresh scan of the connected devices -- the last one is too old to confirm this is still the same board.";
    }
    return {};
}

const char* flashPhaseLabel(FlashPhase phase)
{
    switch (phase) {
    case FlashPhase::StepStarted:       return "starting";
    case FlashPhase::Loading:           return "loading";
    case FlashPhase::Verifying:         return "verifying";
    case FlashPhase::Touching:          return "touching";
    case FlashPhase::WaitingForVolume:  return "waiting for volume";
    case FlashPhase::Copying:           return "copying";
    case FlashPhase::WaitingForRelease: return "waiting for release";
    case FlashPhase::StepFinished:      return "finished";
    }
    return "?";
}

std::optional<RecoveryAnchor> recoveryAnchorFor(FlashOutcome outcome, TargetCpu cpu,
                                                 std::size_t stepsCompleted,
                                                 std::size_t stepCount)
{
    // A plan that stopped part-way through is answered FIRST, ahead of the
    // per-outcome mapping below: both CPUs now hold firmware from two
    // different installs, and that is what the user has to deal with before
    // whatever reason the failing step gave. See recoveryAnchorFor's header
    // comment for the LegacyDirect path this is written for.
    //
    // A second exhaustive switch rather than `if (partway) return ...` with a
    // list of exempt outcomes, so this classification is ALSO a compile error
    // to leave un-answered when a FlashOutcome enumerator is added -- the same
    // property the mapping switch below has, and the reason neither has a
    // `default:`.
    const bool partway = stepsCompleted > 0 && stepsCompleted < stepCount;
    switch (outcome) {
    case FlashOutcome::Success:
        // Not a stop at all: stepsCompleted == stepCount, so `partway` is
        // already false. Listed for exhaustiveness.
    case FlashOutcome::NeedsConfirmation:
        // Not a failure either -- the plan is paused mid-flight and the dialog
        // is prompting for what it needs. Sending the user to Recovery here
        // would talk them out of the resume that is the intended next step.
    case FlashOutcome::InvalidArgument:
        // A caller bug caught before any I/O; stepsCompleted is always 0.
        break;
    case FlashOutcome::RefusedUnidentified:
    case FlashOutcome::RefusedAmbiguous:
    case FlashOutcome::RefusedWrongCpu:
    case FlashOutcome::BadImage:
    case FlashOutcome::VerifyFailed:
    case FlashOutcome::CopyFailed:
    case FlashOutcome::Timeout:
    case FlashOutcome::Aborted:
        // Aborted included deliberately: a cancel between the MAIN and DISPLAY
        // steps leaves exactly the same mixed board as any other mid-plan stop
        // (fwApp.cpp's window-close guard says so in as many words). A cancel
        // before anything was written leaves `partway` false and still gets no
        // Recovery link, which is what the flash dialog's "Aborted is the
        // user's own choice" styling depends on.
        if (partway) return RecoveryAnchor::PartialLegacyFlash;
        break;
    }

    switch (outcome) {
    case FlashOutcome::RefusedUnidentified:
    case FlashOutcome::Timeout:
        return cpu == TargetCpu::Main ? RecoveryAnchor::MainNotIdentified
                                       : RecoveryAnchor::DisplayNotIdentified;
    case FlashOutcome::BadImage:
    case FlashOutcome::VerifyFailed:
        return RecoveryAnchor::ImageRejected;
    case FlashOutcome::CopyFailed:
        return RecoveryAnchor::WriteInterrupted;
    case FlashOutcome::RefusedAmbiguous:
        return RecoveryAnchor::TwoVolumes;
    case FlashOutcome::RefusedWrongCpu:
        // NOT TwoVolumes: exactly one volume is mounted and there is nothing
        // ambiguous about it, so a section headed "Two RPI-RP2 drives are
        // mounted" would describe a state the user is not in. What they are
        // one click away from is a main image on the DISPLAY CPU -- the hazard
        // this refusal just stopped -- and that section says what it is and why
        // it matters, which is the thing worth reading after being refused.
        return RecoveryAnchor::WrongImageOnWrongCpu;
    case FlashOutcome::Success:
    case FlashOutcome::Aborted:
    case FlashOutcome::NeedsConfirmation:
    case FlashOutcome::InvalidArgument:
        return std::nullopt;
    }
    return std::nullopt;
}

FlashController::FlashController() = default;

FlashController::~FlashController()
{
    // Best-effort cooperative stop, then wait it out: a worker mid-copy or
    // mid-download does not stop instantly, but the alternative -- detaching
    // and letting it keep running against a destroyed `this` (the thread
    // lambda captures it by pointer for the progress/result callbacks) --
    // is a use-after-free. This can block app shutdown for up to one poll
    // interval plus whatever blocking call is in flight; RemoteCatalog's
    // destructor (fwCatalogRemote.h) makes the same trade for the same
    // reason.
    m_cancelRequested.store(true, std::memory_order_relaxed);
    if (m_worker.joinable()) m_worker.join();
}

void FlashController::begin(const CatalogEntry& entry, const CpuIdentity& identity)
{
    if (m_state == FlashState::Running) return; // one flash at a time

    // Only reached when NOT Running, so any previous worker has already
    // pushed its final result (state became AwaitingConfirmation/Succeeded/
    // Failed via a prior poll()) and is at most microseconds from returning
    // -- this join is not a wait on the engine, only on that thread's own
    // teardown.
    if (m_worker.joinable()) m_worker.join();

    m_identity = identity;
    // dropRedundantErases() applied HERE, at the one place a plan becomes the
    // thing that actually runs, against the SAME identity snapshot the engine
    // will use -- so the steps executed and the steps the dialog previewed
    // cannot disagree. See dropRedundantErases()'s comment (fwFlashPlan.h) for
    // the single, narrow rule it applies; with a default-constructed identity
    // it removes nothing at all, which is every call before a CPU probe has
    // run.
    m_plan = dropRedundantErases(buildFlashPlan(entry), identity);
    m_log.clear();
    m_result = FlashResult{};
    m_lastProgress.reset();
    // A genuinely fresh flash from step 0, so the bar starts over. confirm()
    // deliberately does not do this -- see progressFraction()'s comment.
    m_progressFraction = 0.0f;
    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_state = FlashState::Running;

    startWorker(std::string{}, 0);
}

void FlashController::confirm(std::string typed, const CpuIdentity& identity)
{
    if (m_state != FlashState::AwaitingConfirmation) return;
    // Defensive: AwaitingConfirmation is only ever entered from a
    // NeedsConfirmation result, and runFlashPlan() always sets resumable
    // true for that outcome (see FlashResult's comment in
    // fwFlashEngine.h) -- but never resume on the strength of the state
    // alone if that invariant were ever violated.
    if (!m_result.resumable) return;

    if (m_worker.joinable()) m_worker.join(); // see begin()'s comment

    // Overwrite begin()'s snapshot with the identity the caller just re-read.
    // startWorker() below builds makeProductionFlashIo(m_identity), so this
    // assignment is the entire mechanism by which the resumed steps touch the
    // board that is on the port RIGHT NOW rather than the one that was there
    // before the user was asked to type a CPU name. See confirm()'s header
    // comment for the wrong-CPU write this closes.
    m_identity = identity;

    const std::size_t startIndex = m_result.stepsCompleted;
    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_state = FlashState::Running;
    startWorker(std::move(typed), startIndex);
}

void FlashController::startWorker(std::string typedConfirmation, std::size_t startIndex)
{
    FlashIo io = makeProductionFlashIo(m_identity);
    // Overrides makeProductionFlashIo's plain sleep-and-never-cancel
    // waitTick with one bound to this controller's own flag.
    io.waitTick = [this](int intervalMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        return !m_cancelRequested.load(std::memory_order_relaxed);
    };

    m_worker = std::thread([this, io = std::move(io), plan = m_plan,
                            typedConfirmation = std::move(typedConfirmation), startIndex]() mutable {
        auto result = runFlashPlan(io, plan, typedConfirmation,
            [this](const FlashProgress& p) { enqueueProgress(p); }, startIndex);
        enqueueResult(result);
    });
}

void FlashController::cancel()
{
    if (m_state == FlashState::Running) {
        m_cancelRequested.store(true, std::memory_order_relaxed);
        return;
    }
    if (m_state == FlashState::AwaitingConfirmation) {
        // No worker is running to signal -- the plan is simply not resumed.
        // Routed through onFinished(), same as a worker-produced result,
        // so this state transition happens in exactly one place.
        FlashResult r;
        r.outcome = FlashOutcome::Aborted;
        r.message = "cancelled";
        r.stepsCompleted = m_result.stepsCompleted;
        r.resumable = false;
        onFinished(r);
    }
}

void FlashController::reset()
{
    if (m_state == FlashState::Running) return;
    if (m_worker.joinable()) m_worker.join();
    m_state = FlashState::Idle;
    m_result = FlashResult{};
    m_log.clear();
    m_lastProgress.reset();
    m_progressFraction = 0.0f;
}

void FlashController::enqueueProgress(const FlashProgress& p)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_queue.push_back(QueueEntry{ false, p, FlashResult{} });
}

void FlashController::enqueueResult(const FlashResult& r)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_queue.push_back(QueueEntry{ true, FlashProgress{}, r });
}

void FlashController::poll()
{
    std::vector<QueueEntry> drained;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        drained.swap(m_queue);
    }
    for (const auto& entry : drained) {
        if (entry.isFinal) onFinished(entry.result);
        else               onProgress(entry.progress);
    }
}

void FlashController::onProgress(const FlashProgress& progress)
{
    // A refresh repeats a phase the log has ALREADY recorded and adds nothing
    // to the record of what happened -- see FlashProgress::isRefresh. Keeping
    // them out is not an optimisation: a wait that runs to its thirty-second
    // timeout polls 120 times, and 120 lines of "waiting for the RPI-RP2
    // volume" would bury the announcement and the failure that bracket them,
    // which are the two lines a user reads the log for. It also means the log
    // grows with EVENTS, not with elapsed time, so nothing accumulates without
    // bound while a wait runs.
    if (!progress.isRefresh)
        m_log.push_back(formatProgress(progress));

    // The live readout and the bar want the opposite: refreshes are exactly
    // what keeps them moving through a wait.
    m_lastProgress = progress;
    m_progressFraction = std::max(m_progressFraction, flashProgressFraction(progress));
}

void FlashController::onFinished(const FlashResult& result)
{
    m_result = result;
    switch (result.outcome) {
    case FlashOutcome::Success:           m_state = FlashState::Succeeded; break;
    case FlashOutcome::NeedsConfirmation: m_state = FlashState::AwaitingConfirmation; break;
    default:                              m_state = FlashState::Failed; break;
    }

    // Only a Success completes the bar. flashPhaseFraction() stops short of
    // 1.0 on purpose (the last thing a final step reports is StepFinished, at
    // 0.85 of that step), so this is the one place that can honestly say the
    // whole plan is done -- and it says it only once runFlashPlan has actually
    // returned Success. Every other outcome leaves the bar exactly where the
    // flash stopped, which is the true statement about it; a failure that
    // snapped the bar to 100% would be this dialog telling the user something
    // untrue for the second time.
    if (result.outcome == FlashOutcome::Success)
        m_progressFraction = 1.0f;
}

std::optional<RecoveryAnchor> FlashController::recoveryAnchor() const
{
    const TargetCpu cpu = m_lastProgress ? m_lastProgress->cpu : TargetCpu::Main;
    // The plan size as the ENGINE saw it, which is what stepsCompleted is
    // relative to. m_plan is the same plan, so the fallback for "no progress
    // event has arrived yet" agrees with it; that case only arises before the
    // worker has reported anything, where stepsCompleted is 0 and the step
    // count cannot change the answer.
    const std::size_t stepCount = m_lastProgress ? m_lastProgress->stepCount : m_plan.size();
    return recoveryAnchorFor(m_result.outcome, cpu, m_result.stepsCompleted, stepCount);
}

} // namespace fwog
