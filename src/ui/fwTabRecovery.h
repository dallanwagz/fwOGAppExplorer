#pragma once

#include "core/fwTypes.h"
#include "flash/fwCpuProbeController.h"
#include "ui/fwBoardImage.h"
#include "ui/fwRecoveryContent.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fwog {

class DeviceModel;

/// The Recovery tab: what the app can currently see, the two ways to put a
/// FreeWili CPU into its bootloader by hand, a photograph of the board showing
/// where the pads are, and the way back from a CPU left running the CPU prober.
///
/// It previously rendered every section of kRecoverySections
/// (fwRecoveryContent.h) as a collapsible header, keyed by RecoveryAnchor so a
/// flash refusal could scrollTo() the explanation of what had just happened, and
/// it hosted the Identify CPUs action inside the TwoVolumes section.
///
/// CONSEQUENCE, so nobody rediscovers it as a bug: scrollTo() still records an
/// anchor and the flash dialog's "Open Recovery" still brings the user here,
/// but there are no anchored sections left to land on -- they arrive at the top
/// of the page. recoveryAnchorFor() and its deliberately exhaustive switches
/// are untouched and still compile; what changed is only what this tab does
/// with the answer. The identification machinery below is likewise still live
/// and still driven by the Default Firmware tab's auto-identify (ProbeAccess),
/// which is where it is actually reached from now that hub position names a
/// bootrom drive without any probe at all.
///
/// THE PANEL IS GONE FROM THE CODE TOO, and that is a deliberate second step. Up
/// to now the "just those two procedures" state above was true only at RUNTIME:
/// drawIdentifyPanel() and drawWhatIsNowPossible() were still here, still
/// compiled, and had ZERO CALL SITES -- private members nothing invoked. A
/// reader (and a reviewer, and the author of the unmounted-BOOTSEL notice, which
/// was added to that panel and therefore never rendered once) had every reason
/// to take them for live UI. Dead private code that looks live is not neutral;
/// it is a false map. The escape hatch that panel contained was moved into
/// draw() -- it is the only part with no other home in the app and a live source
/// of the state it recovers from -- and the rest was deleted. Nothing the user
/// could reach was removed, because none of it was reachable.
class TabRecovery {
public:
    /// Draws the tab body for one frame. `deviceModel` is read, never mutated:
    /// the identification needs a CpuIdentity SNAPSHOT (its worker thread must
    /// never touch DeviceModel) and needs to know how many boards are
    /// connected, since the by-elimination step is only sound for one board.
    /// `onGoToTab` switches the app's visible tab to the given index -- the
    /// caller owns the tab bar, exactly as it owns the FlashDialog's "Open
    /// Recovery" jump in the other direction. It is what closes the loop after
    /// a successful identification: knowing which drive is which is of no use
    /// on the page that told you, and leaving the user to find their own way to
    /// a Flash button is how a finished feature still reads as a dead end.
    /// `board` is the photograph with the MAIN and DISPLAY BOOTSEL pads and a
    /// ground point marked on it, created once by the caller (it needs the
    /// SDL_Renderer, which this tab has no other reason to know about). A
    /// BoardImage with a null texture renders as a stated absence, not a gap.
    void draw(DeviceModel& deviceModel, const std::function<void(int)>& onGoToTab,
              const BoardImage& board);

    /// Drains the identification worker's queue AND re-reads the mounted
    /// volumes and serial ports. Call once per frame from the app's main loop,
    /// OUTSIDE the tab bar.
    ///
    /// draw() only runs while this tab is the active one, and two things must
    /// keep happening whatever tab the user is looking at: an identification
    /// that is writing to a board has to keep making progress and stay
    /// cancellable, and a mapping that has stopped applying has to be noticed.
    /// The second is why the environment poll lives here rather than in draw():
    /// verifiedVolume() below authorises writes from OTHER tabs, so it must go
    /// stale on its own schedule, not on the schedule of the user happening to
    /// look at this page.
    void poll();

    /// The one fact a still-valid identification contributes to the rest of the
    /// app, or a default-constructed (`known() == false`) VerifiedVolume when
    /// there is none. Re-derived from the CURRENT volumes on every call -- see
    /// verifiedVolumeFrom() (fwCpuProbe.h), which applies the freshness rule.
    /// Nothing is cached, so nothing can outlive its evidence.
    VerifiedVolume verifiedVolume() const;

    /// One line about the state of the identification, for the always-visible
    /// device bar: what it established while it holds, and -- the case this
    /// exists for -- that it has been DISCARDED and why, the moment it stops
    /// applying. Empty only when no identification has ever completed.
    ///
    /// A mapping silently evaporating is what the device bar must never let
    /// happen: the user would watch a Flash button they had just earned go back
    /// to refusing, with the explanation confined to a tab they are no longer
    /// on.
    std::string identificationNote() const;

    /// True when identificationNote() is reporting a DISCARDED identification
    /// rather than a live one. Exposed as its own answer so the device bar can
    /// colour it without inferring anything from the sentence's words.
    bool identificationDiscarded() const;

    /// True while an identification is actually executing. The caller uses it
    /// the same way it uses FlashDialog::isFlashRunning(): to veto an OS
    /// window-close request, because closing the app mid-flow leaves a CPU
    /// running the prober with the app that knows how to get it back gone.
    bool isIdentifyRunning() const;

    /// Starts an identification against the currently selected board. Exactly
    /// what this tab's own Identify CPUs button does -- it calls this too, so
    /// there is one snapshot rule and one begin() call site rather than two
    /// that can drift.
    ///
    /// Exposed because the Default Firmware tab's install buttons run an
    /// identification THEMSELVES when a click cannot proceed without one (see
    /// autoIdentifyDecision(), fwTabDefaultFirmwareLogic.h). That tab does not
    /// own a prober and must not acquire one: there is exactly one
    /// CpuProbeController in this app, and exactly one place its result is fed
    /// back into DeviceModel (fwApp.cpp's setVerifiedVolume call). A second
    /// controller would mean two mappings with two lifetimes.
    ///
    /// No-op while one is already running (CpuProbeController::begin()).
    /// `deviceModel` is read, never mutated: the CpuIdentity handed to the
    /// worker is a SNAPSHOT taken here, on the UI thread.
    void beginIdentify(DeviceModel& deviceModel);

    /// How many RPI-RP2 volumes were mounted at the last poll(). The count is
    /// what decides whether an identification is needed at all, and it is
    /// re-read every frame by poll() regardless of which tab is visible --
    /// which is exactly why the Default Firmware tab can ask for it.
    std::size_t mountedVolumeCount() const { return m_volumes.size(); }

    /// Why the most recent identification did not produce an answer, or empty
    /// when there is nothing to report (none has run, one is running, or the
    /// last one succeeded). Fail-closed by construction: it reports a FAILURE,
    /// never an authorisation, so a caller cannot mistake an empty string here
    /// for permission to do anything.
    std::string identifyFailure() const;

    /// Requests that the section for `anchor` be opened and scrolled into view
    /// on the next draw() call. Consumed the first time draw() acts on it.
    void scrollTo(RecoveryAnchor anchor);

private:
    /// The "a CPU is stuck running the prober" escape hatch, at the foot of the
    /// body. Drawn from draw(); see its definition for why it is the one part of
    /// the retired identification panel that was kept.
    void drawStuckProber();

    /// Re-reads the mounted RPI-RP2 volumes, at most every kPollIntervalMs.
    /// findRpiRp2Volumes() walks the logical drives and queries each one's
    /// label, so doing it at frame rate would be pointless work. Called from
    /// poll(), i.e. every frame whatever tab is visible: verifiedVolume()'s
    /// freshness rule is only as good as the reading it is applied to, and it
    /// authorises writes from the OTHER tabs.
    void refreshVolumes();

    /// Re-reads the serial ports, at most every kPollIntervalMs. Called from
    /// draw() only: nothing outside this tab reads m_ports, and
    /// listSerialPorts() is a full SetupDi device-class enumeration -- far more
    /// expensive than the volume walk and with no reader when this tab is not
    /// on screen.
    void refreshPorts();

    static constexpr int kPollIntervalMs = 500;

    std::optional<RecoveryAnchor> m_pendingScroll;

    CpuProbeController m_probe;

    std::vector<std::string> m_volumes;
    /// How many CPUs are in BOOTSEL, read alongside m_volumes and from the same
    /// poll. Read together or not at all: the only thing this number is used
    /// for is the DIFFERENCE between it and m_volumes.size(), and comparing two
    /// readings taken at different moments would invent a mismatch (or hide
    /// one) every time a drive mounted between them.
    int m_bootselDevices = 0;
    /// listSerialPorts(): PRESENTLY ATTACHED ports, each named once. That it
    /// is presently-attached matters here rather than being a detail -- this
    /// list is what the manual picker below offers to reboot into BOOTSEL, and
    /// it used to be read from a registry key that retains names for devices
    /// that are gone (and reuses them). Offering a user a COM number that no
    /// longer belongs to anything, or belongs to something else now, is not a
    /// list to act on. See src/platform/fwSerialPorts.h.
    std::vector<std::string> m_ports;
    /// Separate clocks: the two reads happen on different schedules (every
    /// frame vs. only while this tab draws), so one timestamp would let a spell
    /// of drawing this tab suppress the volume poll the rest of the app depends
    /// on -- or the reverse.
    std::optional<std::chrono::steady_clock::time_point> m_volumesPolledAt;
    std::optional<std::chrono::steady_clock::time_point> m_portsPolledAt;

    /// Index into m_ports for the manual "return this port's CPU to BOOTSEL"
    /// control. Clamped every frame -- the port list changes underneath it.
    int m_selectedPort = 0;
};

} // namespace fwog
