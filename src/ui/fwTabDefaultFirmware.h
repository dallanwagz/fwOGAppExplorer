#pragma once

#include "core/fwTypes.h"
#include "ui/fwRecoveryContent.h"   // RecoveryAnchor

#include <cstddef>
#include <functional>
#include <string>

namespace fwog {

class DeviceModel;
class FlashDialog;

/// The app's single CPU prober, as the Default Firmware tab needs it.
///
/// This tab runs an identification ITSELF when a click cannot proceed without
/// one (see autoIdentifyDecision(), fwTabDefaultFirmwareLogic.h), rather than
/// sending the user to the Recovery tab to run one by hand and come back. It
/// does not own a prober to do that with, deliberately: there is exactly one
/// CpuProbeController in this app and exactly one place its answer is fed back
/// into DeviceModel (fwApp.cpp's setVerifiedVolume call, every frame). A second
/// controller would mean two mappings with two lifetimes and two freshness
/// clocks, which is precisely the shape of bug the freshness rules exist to
/// prevent. So the tab is handed four small questions/answers about the one
/// prober that already exists, wired up by App::run() from TabRecovery.
///
/// Note what is NOT here: any way to read the probe's RESULT. The result
/// reaches this tab the same way it reaches everything else -- folded into the
/// selected device's CpuIdentity -- so this tab cannot act on a mapping that
/// has not been through verifiedVolumeFrom()'s freshness gate.
struct ProbeAccess {
    /// RPI-RP2 volumes mounted right now (TabRecovery::mountedVolumeCount(),
    /// re-read every frame whatever tab is visible).
    std::function<std::size_t()> mountedVolumes;
    /// True while an identification is executing.
    std::function<bool()>        running;
    /// Why the most recent identification produced no answer, or empty. Never
    /// an authorisation -- see TabRecovery::identifyFailure().
    std::function<std::string()> lastFailure;
    /// Start one. No-op while one is already running.
    std::function<void()>        begin;
};

/// The Default Firmware tab: two big install buttons and a Danger zone.
///
///   1. FreeWili 1-OG -- installs the wiliOGBsp display bootloader
///      (FlashScheme::DisplayBootloader).
///   2. Original FreeWili firmware (deprecated) -- the LegacyDirect plan.
///   3. Danger zone -- Erase MAIN CPU and Erase DISPLAY CPU, each collapsed by
///      default and each behind its own typed confirmation ("ERASE MAIN" /
///      "ERASE DISPLAY", eraseConfirmationMatches()).
///
/// Everything else is deliberately absent from the default view. The plan, the
/// version table and the advisory warnings live behind a per-card "Details"
/// disclosure that starts CLOSED, and the reference material that used to be
/// printed inline -- the 10-second inter-CPU silence rule, why the DISPLAY CPU
/// is dealt with first, when erasing is and is not the right move -- lives in
/// the Recovery tab, which is this app's documentation. None of it was deleted;
/// it was moved to the one page that exists to hold it, and this tab links
/// there. What a card shows by default is its name, the version it installs,
/// and -- only when there is one -- a single line of status or refusal.
///
/// The cards are rebuilt from embeddedEntries() every frame, same "no stale
/// cached copy" spirit as the rest of the app: this is the tab a user reaches
/// for when things have gone wrong. When nothing was embedded in this build,
/// each card says so plainly rather than disappearing.
///
/// Fix round (Task 22, Critical), and it applies to BOTH erase cards: the
/// confirmation buffer used to be cleared ONLY inside the Erase button's own
/// click handler. ImGui's CollapsingHeader separately remembers its open/closed
/// state by ID across frames, independent of whether this tab was even drawn in
/// between -- so a user who typed the phrase, never clicked, and left (switched
/// tabs, or just stopped looking) would find the card already expanded and the
/// button already enabled on return: reachable by momentum, exactly what these
/// cards' design exists to prevent. onTabHidden() closes that hole from the
/// outside (see fwApp.cpp, which calls it the frame this tab stops being the
/// active tab); draw() itself also clears a card's buffer any frame that card's
/// header renders collapsed. Together these guarantee the invariant: an erase
/// button can only be enabled in a frame where the user typed its phrase during
/// THIS uninterrupted visit.
class DefaultFirmwareTab {
public:
    /// Draws the tab body for one frame.
    ///
    /// `flashDialog` is the same instance AppExplorerTab uses (owned by
    /// App::run()), so only one flash can ever be in flight across the whole
    /// app -- clicking an install or erase button calls flashDialog.open(), it
    /// never starts a flash directly.
    ///
    /// `probe` is the app's single prober (see ProbeAccess). `onOpenRecovery`
    /// switches to the Recovery tab and scrolls it to the named section; the
    /// caller owns the tab bar and TabRecovery, exactly as it does for the
    /// flash dialog's own "Open Recovery" button.
    void draw(DeviceModel& deviceModel, FlashDialog& flashDialog, const ProbeAccess& probe,
              const std::function<void(RecoveryAnchor)>& onOpenRecovery);

    /// Call the frame this tab STOPS being the active tab (see fwApp.cpp's
    /// tab-bar loop). Clears both erase cards' typed confirmations so neither
    /// can survive a visit boundary -- see the class comment above -- and drops
    /// any install click that was still waiting on an identification, so
    /// leaving and returning cannot open a flash dialog the user did not ask
    /// for on this visit. Safe to call when nothing is pending.
    void onTabHidden();

private:
    /// Index into m_eraseConfirm for a CPU. There are two erase cards and they
    /// must not share a buffer: typing "ERASE MAIN" would otherwise be sitting
    /// in the DISPLAY card's box too.
    static std::size_t eraseSlot(TargetCpu cpu) { return cpu == TargetCpu::Main ? 0u : 1u; }

    /// What the user has typed into each erase card's confirmation box, MAIN
    /// then DISPLAY. Cleared whenever that Erase button is actually clicked,
    /// whenever that card's header renders collapsed, and whenever this tab
    /// loses focus (onTabHidden()) -- so a stale phrase left over from a
    /// previous visit can never silently arm a later click without being typed
    /// again.
    char m_eraseConfirm[2][32] = {};

    /// The slug of the install entry whose click is waiting on the CPU
    /// identification this tab started for it, or empty. Holding the SLUG
    /// rather than a pointer or a copy of the entry is deliberate: the catalog
    /// is rebuilt every frame, so a pointer would dangle, and a copy would let
    /// this act on an entry that has since changed underneath it.
    std::string m_pendingInstallSlug;

    /// The one line the card that started an identification shows about how it
    /// went, or empty. Only ever a failure or a refusal -- a success needs no
    /// line, because the flash dialog opens.
    std::string m_autoIdentifyNote;

    /// Which card m_autoIdentifyNote belongs to. Held separately so the note
    /// appears under the button that was actually pressed: without it, "could
    /// not identify the CPUs" would print under BOTH install cards, one of
    /// which the user never touched.
    std::string m_autoIdentifyNoteSlug;
};

} // namespace fwog
