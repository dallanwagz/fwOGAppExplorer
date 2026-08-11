#include "ui/fwFlashDialog.h"

#include "catalog/fwCatalogFilter.h"   // flashDisabledReason
#include "flash/fwFlashPlan.h"
#include "flash/fwVolumeState.h"       // confirmationMatches

#include <imgui.h>
#include <IconsMaterialDesign.h>

#include <algorithm>
#include <cstdio>
#include <optional>

namespace fwog {
namespace {

constexpr const char* kPopupId = "Flash Firmware##FlashDialog";

const ImVec4 kMutedColor { 0.65f, 0.65f, 0.65f, 1.00f };
const ImVec4 kWarnColor  { 0.90f, 0.65f, 0.15f, 1.00f };
const ImVec4 kErrorColor { 0.95f, 0.35f, 0.35f, 1.00f };
const ImVec4 kOkColor    { 0.35f, 0.80f, 0.35f, 1.00f };
const ImVec4 kAccentColor{ 0.30f, 0.55f, 0.95f, 1.00f };

const char* cpuLabel(TargetCpu cpu) { return cpu == TargetCpu::Main ? "MAIN" : "DISPLAY"; }

/// One width for the whole progress block -- the two bars, the muted note and
/// the scrolling log -- so the section reads as one column rather than as
/// several widgets that happen to be stacked. This is what the log child was
/// already hardcoded to; it is now shared rather than repeated.
constexpr float kProgressWidth = 420.0f;

} // namespace

void FlashDialog::open(const CatalogEntry& entry, const DeviceView& device)
{
    if (m_controller.state() == FlashState::Running) return; // one flash at a time

    m_entry          = entry;
    m_openedUniqueID = device.uniqueID;
    m_openedSerial   = device.serial;
    // Seeded from the device this was opened on, then RE-DERIVED every Idle
    // frame in draw() from a freshly re-read identity -- see the Idle branch.
    // A plan and its warnings are a promise about what the button will do, and
    // that promise has to track the board, not the moment the dialog opened.
    refreshPlan(device.identity);
    m_confirmBuf[0] = '\0';

    // Reopening this same controller for a new entry must not show the
    // previous flash's leftover Succeeded/Failed panel before Start is
    // clicked again.
    m_controller.reset();

    m_open = true;
    m_shouldOpenPopup = true;
}

bool FlashDialog::isFlashRunning() const
{
    return m_controller.state() == FlashState::Running;
}

void FlashDialog::beginImmediate(const CatalogEntry& entry, const DeviceView& device)
{
    if (m_controller.state() == FlashState::Running) return;

    m_entry          = entry;
    m_openedUniqueID = device.uniqueID;
    m_openedSerial   = device.serial;
    m_confirmBuf[0]  = '\0';

    // Built for the same reason open() builds it: if this run stops on a typed
    // confirmation or fails, draw() raises the modal, and the modal renders
    // m_plan. Without this it would raise a window describing the previous
    // flash's plan, or none at all.
    refreshPlan(device.identity);

    m_controller.reset();
    m_inlineRun = true;
    m_startedAt = std::chrono::steady_clock::now();
    // The identity is used exactly as read this frame by the caller. See the
    // header: a single click has no pause in which the board could be swapped,
    // which is what lets this start without a second confirmation.
    m_controller.begin(entry, device.identity);
}

bool FlashDialog::isInlineBusy() const
{
    if (!m_inlineRun) return false;
    const FlashState s = m_controller.state();
    return s == FlashState::Running || s == FlashState::AwaitingConfirmation;
}

float FlashDialog::progressFraction() const
{
    return m_controller.progressFraction();
}

std::string FlashDialog::progressLine() const
{
    const auto p = m_controller.lastProgress();
    if (!p) return {};

    std::string line = "step " + std::to_string(p->stepIndex + 1) + " of " +
                       std::to_string(p->stepCount) + " -- " + flashPhaseLabel(p->phase) +
                       (p->message.empty() ? std::string{} : (": " + p->message));

    // A BOUNDED WAIT gets a countdown, and this is the whole reason for it: the
    // overall bar deliberately does not advance while a wait runs (see
    // flashProgressFraction -- a wait's end is not predictable, so moving the
    // bar with its clock would claim progress that has not happened). The
    // result is a bar that can sit still for thirty seconds while the app is
    // working perfectly, which reads as a hang. The seconds remaining are the
    // honest thing that IS moving, so they are what gets shown.
    if (p->waitTotalMs > 0) {
        const int remaining = (p->waitTotalMs - p->waitElapsedMs + 999) / 1000;
        line += " (" + std::to_string(remaining > 0 ? remaining : 0) + "s left)";
    }
    return line;
}

bool FlashDialog::isWaiting() const
{
    const auto p = m_controller.lastProgress();
    return p.has_value() && p->waitTotalMs > 0;
}

bool FlashDialog::hasTimeEstimate() const
{
    if (!m_startedAt) return false;
    if (m_entry.scheme != FlashScheme::LegacyDirect) return false;
    // The FULL plan only. restrictToCpu() keeps the slug and the scheme, so
    // neither tells a one-image half apart from the three-step whole -- the step
    // count does, and a MAIN-only write is nothing like three and a half
    // minutes. No estimate is better than a wrong one.
    return m_plan.size() >= 3;
}

float FlashDialog::estimatedFraction() const
{
    if (!hasTimeEstimate()) return 0.0f;
    // Finished means finished: the bar fills when the flash actually ends, not
    // when the clock says it should have.
    if (m_controller.state() == FlashState::Succeeded) return 1.0f;

    const auto elapsed = std::chrono::steady_clock::now() - *m_startedAt;
    const float f = float(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count())
                  / float(std::chrono::duration_cast<std::chrono::milliseconds>(
                              kLegacyFlashEstimate).count());
    if (f < 0.0f) return 0.0f;
    // Held just short of full while still running. See the header: a guess that
    // reads 100% with the board still being written is the dialog claiming
    // something it does not know.
    return f > 0.99f ? 0.99f : f;
}

std::string FlashDialog::estimatedRemaining() const
{
    if (!hasTimeEstimate()) return {};
    if (m_controller.state() == FlashState::Succeeded) return "done";

    const auto elapsed = std::chrono::steady_clock::now() - *m_startedAt;
    const auto left = kLegacyFlashEstimate
                    - std::chrono::duration_cast<std::chrono::seconds>(elapsed);
    // Past the estimate and still going. Saying so is the honest reading, and
    // it is materially different from "0s left" -- which would suggest the
    // thing is about to stop when nobody knows that.
    if (left.count() <= 0) return "taking longer than expected";

    const long long total = left.count();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%lldm %02llds left", total / 60, total % 60);
    return buf;
}

void FlashDialog::refreshPlan(const CpuIdentity& identity)
{
    // The SAME two calls, in the same order, that FlashController::begin() makes
    // -- buildFlashPlan() then dropRedundantErases(), against the identity that
    // will be handed to begin(). That is what makes the numbered list on screen
    // the list that actually runs, rather than a list of what would run on a
    // board in some other state.
    const auto full = buildFlashPlan(m_entry);
    m_droppedNote   = droppedEraseNote(full, identity);
    m_plan          = dropRedundantErases(full, identity);
    m_warnings      = planWarnings(m_entry, identity);
}

void FlashDialog::close()
{
    m_startedAt.reset();
    m_open = false;
    ImGui::CloseCurrentPopup();
}

void FlashDialog::draw(DeviceModel& deviceModel, const std::function<void(RecoveryAnchor)>& onOpenRecovery)
{
    // Drained every frame, independent of m_open, so nothing the worker
    // queued while the dialog was momentarily not being drawn is lost.
    m_controller.poll();

    // An inline run (beginImmediate) has no window behind it. The two outcomes
    // that need one get it here, the frame they arrive:
    //
    //  - AwaitingConfirmation: the user has to read a sentence and type a CPU
    //    name, under the same freshly-re-checked identity gate as any other
    //    resume. That is an unbounded human pause, so it needs the modal's
    //    per-frame rescan and selectionUnchangedFresh() check -- exactly what
    //    the modal already does, rather than a second copy of it inline.
    //  - Failed: the failure text, the completed-steps note and the Open
    //    Recovery route are the whole reason a user can act on a bad flash.
    //    Reducing them to a red line beside a button would lose the route.
    //
    // Success is deliberately absent: it needs no window. The inline bar
    // reaching full IS the report, and popping a modal to say "done" after a
    // one-click flash would put the click back that this removed.
    if (m_inlineRun && !m_open) {
        const FlashState s = m_controller.state();
        if (s == FlashState::AwaitingConfirmation || s == FlashState::Failed) {
            m_open = true;
            m_shouldOpenPopup = true;
            m_inlineRun = false;   // the modal owns it from here
        }
    }

    if (!m_open) return;

    if (m_shouldOpenPopup) {
        ImGui::OpenPopup(kPopupId);
        m_shouldOpenPopup = false;
    }

    // p_open is deliberately null, always: this dialog is closed only by an
    // explicit button inside it (Close/Cancel), never by a window close
    // control. Dear ImGui's Escape-closes-popup handling
    // (NavUpdateCancelRequest, imgui.cpp) explicitly excludes modal popups
    // from that path, so a BeginPopupModal opened with OpenPopup() is not
    // closed by Escape either -- that is what makes "cannot be dismissed
    // with Escape while Running" true structurally, not merely by a state
    // check here that could be bypassed.
    if (!ImGui::BeginPopupModal(kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const FlashState state = m_controller.state();
    const bool running = (state == FlashState::Running);

    // The two states below whose Start/Proceed button is gated on "is this
    // still the same board?" -- and therefore the two states whose answer is
    // only as good as the age of the scan behind it.
    const bool identityGateIsLive = (state == FlashState::Idle ||
                                     state == FlashState::AwaitingConfirmation);

    // Hold the background scanner in its fast-poll window for as long as the
    // gate is on screen. Every frame on purpose: requestRescan() only stamps a
    // timestamp when a worker is already up (fwFinderManager::requestRefresh),
    // so this costs an atomic store per frame and never spawns a thread -- and
    // it is the only thing that makes the freshness requirement below
    // satisfiable, since this dialog is MODAL and the user cannot reach the
    // device bar's Rescan button while it is open. The elevated scan rate is
    // scoped to exactly the window where a board swap can cause a wrong-CPU
    // write, and lapses on its own once the dialog closes.
    //
    // Deliberately NOT done while Running: a flash in progress is already
    // mounting and unmounting RPI-RP2 volumes and touching serial ports, and
    // extra device enumeration during that buys nothing -- no gate is being
    // evaluated, so nothing would read the fresher data.
    if (identityGateIsLive)
        deviceModel.requestRescan();

    // Re-derive the plan from the board as it is NOW, but only while Idle:
    // once a flash has started, the plan on screen must be the plan the engine
    // is executing, and FlashController froze that at begin() time. Rewriting
    // the list under a running flash would show steps that are not the ones
    // being run.
    if (state == FlashState::Idle) {
        const auto current = deviceModel.selectedByIdOnly();
        refreshPlan(current ? current->identity : CpuIdentity{});
    }

    // --- 1. The plan ------------------------------------------------------
    ImGui::TextUnformatted(ICON_MD_INFO " Plan");
    if (m_plan.empty()) {
        ImGui::TextColored(kMutedColor, "(nothing would be written -- no UF2 assets)");
    } else {
        for (size_t i = 0; i < m_plan.size(); ++i) {
            const auto& step = m_plan[i];
            ImGui::BulletText("%zu. %s -> %s", i + 1, cpuLabel(step.cpu), step.description.c_str());
        }
    }

    // A plan that is visibly shorter than the documentation describes must say
    // why, right here, or it reads as something having quietly gone missing.
    if (!m_droppedNote.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kMutedColor);
        ImGui::TextWrapped(ICON_MD_INFO " %s", m_droppedNote.c_str());
        ImGui::PopStyleColor();
    }

    // --- 2. Warnings, in the theme's accent (warning) colour --------------
    for (const auto& w : m_warnings) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarnColor);
        ImGui::TextWrapped(ICON_MD_WARNING " %s", w.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    // --- 3. Progress: overall bar, phase, step N of M, scrolling log -------
    if (state != FlashState::Idle) {
        ImGui::TextUnformatted(ICON_MD_BOLT " Progress");
        // The same turning hourglass the App Explorer's inline bar shows, so a
        // flash looks like the same operation wherever it was started from.
        // Only while RUNNING: once the flash has finished, succeeded or failed,
        // nothing is in progress and an hourglass would be saying otherwise.
        if (running) {
            ImGui::SameLine();
            static const char* kFrames[] = {
                ICON_MD_HOURGLASS_TOP, ICON_MD_HOURGLASS_FULL, ICON_MD_HOURGLASS_BOTTOM
            };
            ImGui::TextColored(kAccentColor, "%s", kFrames[int(ImGui::GetTime() * 2.0) % 3]);
        }

        const std::optional<FlashProgress> p = m_controller.lastProgress();

        // The overall bar. Determinate and monotonic -- FlashController's
        // progressFraction() is a high-water mark, so it cannot slide
        // backwards when a resumed step re-reports work it had already done.
        //
        // It stays PUT during a wait, on purpose. The second bar below is what
        // moves then, and it is labelled for what it actually measures. A bar
        // that crept toward 100% while the app was in fact counting down to a
        // timeout would be worse than no bar at all: this dialog has already
        // told a user something untrue once (that nothing had been written
        // when a CPU had), and it will not do it again in a different colour.
        //
        // AND IT STOPS CLAIMING TO BE LIVE ONCE THE RUN ENDS. A failed run used
        // to leave this bar reading "Step 2 of 3 -- 48%" with the estimate
        // below it still counting, which is how a finished failure comes to
        // look like a hang: the only thing on screen saying otherwise was the
        // error text, and a half-filled bar with a ticking clock above it
        // argues louder than a paragraph. That is the same fault as the one
        // this block's comment already warns about, arrived at from the other
        // end -- not a bar that lies while running, but a bar that goes on
        // talking after there is nothing left to say.
        const float fraction = m_controller.progressFraction();
        char overallOverlay[96];
        if (!running && p.has_value()) {
            // Terminal. Say where it STOPPED, with no percentage: a percentage
            // is a statement about work still in flight.
            std::snprintf(overallOverlay, sizeof(overallOverlay),
                          state == FlashState::Succeeded ? "Finished -- %zu of %zu steps"
                                                         : "Stopped at step %zu of %zu",
                          p->stepIndex + 1, p->stepCount);
        } else if (p.has_value()) {
            std::snprintf(overallOverlay, sizeof(overallOverlay), "Step %zu of %zu -- %d%%",
                          p->stepIndex + 1, p->stepCount,
                          static_cast<int>(fraction * 100.0f + 0.5f));
        } else {
            // Running, but the worker has not reported anything yet.
            std::snprintf(overallOverlay, sizeof(overallOverlay), "Starting");
        }
        ImGui::ProgressBar(fraction, ImVec2(kProgressWidth, 0.0f), overallOverlay);

        // A WALL-CLOCK countdown for the deprecated-firmware install, which
        // takes about three and a half minutes. The step bar above cannot say
        // that: it advances in three jumps and then sits still for minutes at a
        // time, which on a plan this long reads as a hang rather than as work.
        //
        // Labelled as an ESTIMATE on the bar itself, because that is what it is.
        // It is not derived from anything the engine reports, it holds just
        // short of full while the flash is still running however far past the
        // estimate that goes, and it fills only when the run really ends.
        // `running` and not merely hasTimeEstimate(): an estimate of the time
        // remaining in a run that is over is not a stale number, it is a false
        // one. On a failure it kept rendering "taking longer than expected",
        // which reads as "still working, be patient" at precisely the moment
        // the user needs to read the error and go to Recovery.
        if (running && hasTimeEstimate()) {
            const std::string left = estimatedRemaining();
            char estOverlay[96];
            std::snprintf(estOverlay, sizeof(estOverlay), "Estimated -- %s", left.c_str());
            ImGui::ProgressBar(estimatedFraction(), ImVec2(kProgressWidth, 0.0f), estOverlay);
            ImGui::TextColored(kMutedColor,
                "This install takes about 3m 30s. The bar above is a time estimate, not a "
                "measurement -- do not unplug the board while it is running.");
        }

        // The brief calls for phase and step N of M as their own readout,
        // distinct from the scrolling log below (which also carries them,
        // but buried inside each line's text) -- one line, always the
        // MOST RECENT event, so it reads as "where things are right now"
        // rather than one more history line to scan for. The phase is spelled
        // out in words here because the bar above can only carry a number.
        if (p.has_value()) {
            ImGui::Text("Step %zu of %zu -- %s (%s)",
                       p->stepIndex + 1, p->stepCount, flashPhaseLabel(p->phase), cpuLabel(p->cpu));

            // The wait countdown. Only while Running: once a flash has
            // stopped, the last event may well still be a wait refresh, and
            // leaving a half-full wait bar frozen under a failure message
            // would look like something was still happening.
            //
            // This is the fix for the failure that prompted the whole feature:
            // a LegacyDirect run whose DISPLAY volume never appeared sat on one
            // motionless line for the full thirty seconds. There was no way to
            // tell "working" from "hung" from "nearly done". A wait IS bounded,
            // so its elapsed/total is a fact and a determinate bar is honest --
            // as long as it says what it is measuring, which the sentence below
            // it does.
            if (running && p->waitTotalMs > 0) {
                const float waitFraction = std::clamp(
                    static_cast<float>(p->waitElapsedMs) / static_cast<float>(p->waitTotalMs),
                    0.0f, 1.0f);
                // The overlay is kept short deliberately: ImGui clips a
                // ProgressBar's overlay to the bar's own rectangle, so anything
                // that might not fit belongs in the wrapped sentence below
                // instead, where it cannot be silently cut in half.
                char waitOverlay[32];
                std::snprintf(waitOverlay, sizeof(waitOverlay), "%d s of %d s",
                              p->waitElapsedMs / 1000, p->waitTotalMs / 1000);
                ImGui::ProgressBar(waitFraction, ImVec2(kProgressWidth, 0.0f), waitOverlay);

                // Says both what is being waited for and, in as many words,
                // what the bar above it does and does not mean. The second half
                // is not padding: a determinate bar that fills is read as
                // "nearly done" unless it is told otherwise, and here filling
                // means the opposite.
                ImGui::PushStyleColor(ImGuiCol_Text, kMutedColor);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kProgressWidth);
                ImGui::TextWrapped("Still %s. This second bar shows how much of the wait's own "
                                   "time limit has passed, not how close the flash is to "
                                   "finishing; if it fills, the step gives up and reports a "
                                   "timeout.", p->message.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
        }

        ImGui::BeginChild("##FlashProgressLog", ImVec2(kProgressWidth, 140), ImGuiChildFlags_Border);
        for (const auto& line : m_controller.progressLog())
            ImGui::TextWrapped("%s", line.c_str());
        if (running) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::Separator();
    }

    // --- AwaitingConfirmation: the CPU name to type, gating Proceed --------
    if (state == FlashState::AwaitingConfirmation) {
        const auto& result = m_controller.result();
        ImGui::TextWrapped("%s", result.message.c_str());

        const TargetCpu cpu = result.confirmationCpu.value_or(TargetCpu::Main);
        ImGui::Text("Type %s to confirm this is the right CPU:", cpuLabel(cpu));
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##FlashConfirmName", m_confirmBuf, sizeof(m_confirmBuf));

        // The SAME re-read the Idle branch below performs, for the same
        // reason and with more of it: this branch sits on screen across an
        // UNBOUNDED HUMAN PAUSE by construction -- the user is being asked to
        // read a sentence and type a CPU name -- and the most likely reaction
        // to "something unexpected is mounted" is to unplug the board and try
        // another one, in the same USB port. Everything the Idle branch says
        // about a stale identity applies here with the pause guaranteed rather
        // than merely possible.
        //
        // Concretely, without this: begin() captured board A's MAIN=COM24 /
        // DISPLAY=COM59; the user swaps in board B on the same port; the engine
        // re-evaluates the step, now finds no volume, and touches "COM24" --
        // which Windows has since reassigned out of the freed pool, possibly to
        // board B's DISPLAY console. That reboots B's DISPLAY CPU into BOOTSEL
        // and copies a MAIN image onto it. selectionUnchanged() compares BOTH
        // uniqueID and serial precisely because uniqueID is topological: the
        // same socket yields the same value for a different board (see its
        // header comment in fwFlashController.h).
        //
        // selectedByIdOnly() rather than selected(), same as the Idle branch --
        // see that branch's comment for why the raw candidate is what lets the
        // refusal be worded specifically, and why that is safe.
        // ...and selectionUnchangedFresh(), not selectionUnchanged(), for the
        // half of the problem a content comparison cannot see at all: both
        // sides of that comparison come out of ONE cached scan result, so if
        // nothing has re-scanned since before the swap, it compares a copy of
        // board A's data with itself and answers Unchanged. This branch is the
        // worst place for that -- the pause here is guaranteed, not merely
        // possible. The age check refuses instead, and the requestRescan()
        // above is what keeps that refusal from being the normal case.
        const std::optional<DeviceView> currentDevice = deviceModel.selectedByIdOnly();
        const SelectionCheck check = selectionUnchangedFresh(currentDevice, m_openedUniqueID,
                                                             m_openedSerial, deviceModel.snapshotAge());
        const std::string reason = (check == SelectionCheck::Unchanged)
                                     ? std::string{}
                                     : selectionCheckMessage(check);

        const bool matches = confirmationMatches(m_confirmBuf, cpu);
        ImGui::BeginDisabled(!matches || !reason.empty());
        // The has_value() test is redundant with reason being empty (check ==
        // Unchanged implies currentDevice holds a value), and kept for the same
        // reason the Idle branch keeps its own: the dereference below must be
        // guarded at the point of use, not only by a condition computed above
        // it.
        if (ImGui::Button(ICON_MD_BOLT " Proceed") && currentDevice.has_value()) {
            // A FRESH identity, not m_controller's begin()-time snapshot --
            // this is the argument that makes the resumed steps address the
            // board the check above just approved. Mirrors the Idle branch's
            // begin(m_entry, currentDevice->identity) exactly.
            m_controller.confirm(m_confirmBuf, currentDevice->identity);
            m_confirmBuf[0] = '\0';
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_controller.cancel();
            // Matches what the App Explorer retarget confirmation box
            // already does on context change (fwTabAppExplorer.cpp): a
            // typed confirmation must never silently carry over to
            // whatever the user does next.
            m_confirmBuf[0] = '\0';
        }
        if (!reason.empty())
            ImGui::TextColored(kErrorColor, "%s", reason.c_str());
    }

    // --- 4. Outcome, or the controls for whichever state we are in --------
    if (state == FlashState::Succeeded) {
        const auto& result = m_controller.result();
        ImGui::TextColored(kOkColor, ICON_MD_CHECK_CIRCLE " %s",
                           result.message.empty() ? "Flashing finished." : result.message.c_str());
        if (ImGui::Button("Close")) close();
    } else if (state == FlashState::Failed) {
        const auto& result = m_controller.result();
        // Aborted is the user's own choice (they clicked Cancel), not a
        // board failure -- painting it in the error colour would read as
        // "something went wrong" when nothing did. Muted text, not an error
        // colour, is the whole of what this distinction changes.
        //
        // It does NOT suppress the Recovery link below, and must not: a cancel
        // that lands mid-plan gets RecoveryAnchor::PartialLegacyFlash from
        // recoveryAnchorFor(), because a plan stopped between the MAIN and
        // DISPLAY steps leaves a mixed board whatever the reason it stopped.
        // Only a cancel before anything was written (stepsCompleted == 0)
        // yields no anchor and so no button -- which is the case this muted
        // styling was written for.
        const bool cancelled = (result.outcome == FlashOutcome::Aborted);
        if (cancelled) {
            ImGui::TextColored(kMutedColor, ICON_MD_CANCEL " %s",
                               result.message.empty() ? "Cancelled." : result.message.c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            ImGui::TextWrapped("%s", result.message.c_str());
            ImGui::PopStyleColor();
        }

        const auto anchor = m_controller.recoveryAnchor();
        if (anchor.has_value()) {
            if (ImGui::Button(ICON_MD_OPEN_IN_NEW " Open Recovery")) {
                onOpenRecovery(*anchor);
                close();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Close")) close();
    } else if (running) {
        ImGui::TextColored(kMutedColor, "Do not unplug the board.");
        if (ImGui::Button(ICON_MD_CANCEL " Cancel")) m_controller.cancel();
    } else if (state == FlashState::Idle) {
        // Re-read the selection fresh, right here, rather than trusting
        // whatever open() captured: this Idle preview can sit on screen for
        // an unbounded number of frames while the user reads the plan, and
        // in that window the board can be unplugged, or replaced by a
        // DIFFERENT board that lands on the same USB port -- a stale
        // mainPort/displayPort that now names a different CPU is exactly
        // the wrong-CPU write this app exists to prevent. selectionUnchanged
        // compares BOTH uniqueID and serial: uniqueID alone identifies a
        // port, not a board, so a same-port board swap would pass a
        // uniqueID-only check (see its header comment in
        // fwFlashController.h for the fwfinder detail behind this).
        //
        // Deliberately selectedByIdOnly(), not selected(): selected() already
        // applies its own serial check and collapses a same-port board swap
        // down to plain nullopt before selectionUnchanged() ever sees it, so
        // every mismatch would read as the same generic "nothing selected"
        // message. selectedByIdOnly() hands selectionUnchanged() the raw
        // candidate so it can tell DifferentBoard apart from DifferentPort
        // and word the refusal accordingly -- safety is unaffected either
        // way, since selectionUnchanged() re-applies the identical
        // uniqueID+serial check itself before anything can proceed.
        //
        // selectionUnchangedFresh() rather than selectionUnchanged(): the
        // comparison's two sides are read out of the same cached scan, so it
        // can only detect a swap that a scan has actually observed. See the
        // AwaitingConfirmation branch above and kMaxSnapshotAgeForFlash's
        // comment in fwFlashController.h.
        const std::optional<DeviceView> currentDevice = deviceModel.selectedByIdOnly();
        std::string reason;
        const SelectionCheck check = selectionUnchangedFresh(currentDevice, m_openedUniqueID,
                                                             m_openedSerial, deviceModel.snapshotAge());
        if (check != SelectionCheck::Unchanged)
            reason = selectionCheckMessage(check);
        else
            // 2-arg call, relying on identityConfirmed's default (true):
            // check == Unchanged already guarantees selectionUnchanged()'s
            // own serial check passed, so currentDevice's identity IS
            // confirmed here -- see flashDisabledReason's header comment
            // (fwCatalogFilter.h) for the case (Task 22) where that is not
            // true and the 3-arg form is needed instead.
            reason = flashDisabledReason(m_entry, &*currentDevice);

        ImGui::BeginDisabled(m_plan.empty() || !reason.empty());
        // BRACED, and it must stay braced. Adding the timestamp line above
        // turned a single guarded statement into two, and the unbraced `if`
        // silently released begin() from the guard: it then ran on EVERY Idle
        // frame, so the flash started the instant the dialog opened, with no
        // Start Flashing click and no selection check -- and dereferenced
        // `currentDevice` even when nothing was selected.
        if (ImGui::Button(ICON_MD_BOLT " Start Flashing") && currentDevice.has_value()) {
            m_startedAt = std::chrono::steady_clock::now();
            m_controller.begin(m_entry, currentDevice->identity);   // freshly re-read, not open()'s snapshot
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) close();
        if (!reason.empty())
            ImGui::TextColored(kErrorColor, "%s", reason.c_str());
    }

    ImGui::EndPopup();
}

} // namespace fwog
