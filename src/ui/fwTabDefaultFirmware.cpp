#include "ui/fwTabDefaultFirmware.h"

#include "catalog/fwCatalogEmbedded.h"
#include "catalog/fwCatalogFilter.h"
#include "device/fwDeviceModel.h"
#include "flash/fwFlashPlan.h"
#include "ui/fwFlashDialog.h"
#include "ui/fwTabDefaultFirmwareLogic.h"

#include <imgui.h>
#include <IconsMaterialDesign.h>

#include <algorithm>
#include <functional>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace fwog {
namespace {

const ImVec4 kMutedColor { 0.65f, 0.65f, 0.65f, 1.00f };
const ImVec4 kErrorColor { 0.95f, 0.35f, 0.35f, 1.00f };
const ImVec4 kBusyColor  { 0.30f, 0.55f, 0.95f, 1.00f };

const char* cpuLabel(TargetCpu cpu)
{
    return cpu == TargetCpu::Main ? "MAIN" : "DISPLAY";
}

/// TextColored's wrapping cousin, same helper the App Explorer tab uses.
/// Every disabled-flash reason goes through this: the platform notice is a
/// full sentence, far longer than any of the other reasons, and a line that
/// merely overflows the card's right edge silently hides whatever ran off it.
void wrappedColored(const ImVec4& color, const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

/// The one entry with `scheme` that INSTALLS firmware, i.e. whose assets are
/// not all the flash-erase image.
///
/// A plain first-match-by-scheme search is no longer enough: the standalone
/// `erase-display-cpu` entry (kEraseDisplayCpuSlug) is a
/// FlashScheme::DisplayBootloader entry too -- that is what pins it to the
/// DISPLAY CPU -- so scheme alone now matches two entries and which one won
/// would depend on manifest array order. Testing the ASSETS rather than adding
/// a second slug constant keeps this true of any future erase entry as well,
/// and isEraseAsset() is a fact about the compiled-in image rather than a claim
/// in JSON (see kFlashNukeImageId, fwFlashPlan.h).
const CatalogEntry* findInstallByScheme(std::span<const CatalogEntry> entries, FlashScheme scheme)
{
    for (const auto& e : entries) {
        if (e.scheme != scheme) continue;
        for (const auto& a : e.uf2)
            if (!isEraseAsset(a)) return &e;
    }
    return nullptr;
}

/// Looked up by slug, not scheme, for the two erase entries specifically:
/// erase-MAIN shares FlashScheme::OgApp with ordinary app installs and
/// erase-DISPLAY shares FlashScheme::DisplayBootloader with the bootloader
/// install, so nothing but the slug tells either of them apart.
const CatalogEntry* findBySlug(std::span<const CatalogEntry> entries, const char* slug)
{
    for (const auto& e : entries)
        if (e.slug == slug) return &e;
    return nullptr;
}

/// The version string for a single-asset entry (the DisplayBootloader entry),
/// read via embeddedVersion(id) rather than entry.version -- the same id-keyed
/// source of truth an "installed vs embedded" comparison would have to compare
/// against.
std::string singleAssetVersion(const CatalogEntry& entry)
{
    // The first NON-ERASE asset, not uf2[0]. The bootloader entry's first asset
    // is now the MAIN erase that precedes the install (see eraseOnlyException,
    // fwFlashPlan.cpp), and uf2[0] made the card announce "Installs
    // pico-flash-nuke" -- the version of the wipe, not of the bootloader the
    // button is named after.
    //
    // Same test isEraseAsset() serves everywhere else: a fact about the compiled
    // -in image rather than a claim in JSON, so an entry cannot mislabel its own
    // erase into being read as the thing installed.
    for (const auto& a : entry.uf2)
        if (!isEraseAsset(a)) return embeddedVersion(a.ref.embeddedId);
    return {};
}

/// The card's one version line: what this button installs, and what is on the
/// board. The second half is not implemented -- reading the installed version
/// back off a board is not something this app can do yet -- and it says so in
/// a hover rather than in a sentence on the card, because the fact that it is
/// unknown is worth one word and not one paragraph.
void drawVersionLine(const std::string& embeddedVer)
{
    ImGui::TextColored(kMutedColor, "Installs %s \xE2\x80\x94 installed: unknown",
                       embeddedVer.empty() ? "(unknown)" : embeddedVer.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reading the version already on the board is not implemented yet.");
}

/// The identity this tab previews plans against. selectedByIdOnly() rather
/// than selected(): this is display only -- every actual flash click still
/// gates on the confirmed selection -- and a board whose serial reads
/// "Unknown" must still get an accurate PLAN, even while it is refused a
/// flash.
CpuIdentity previewIdentity(DeviceModel& deviceModel)
{
    const std::optional<DeviceView> raw = deviceModel.selectedByIdOnly();
    return raw ? raw->identity : CpuIdentity{};
}

/// Everything a card used to print inline, behind a disclosure that starts
/// CLOSED: the steps that will run, why one of them may be missing, and the
/// advisory warnings. Nothing here is load-bearing for the decision to click --
/// the flash dialog shows the same plan and the same warnings again, in full,
/// before a single byte is written -- so it is available rather than unavoidable.
/// A TreeNode rather than a CollapsingHeader, and that is a visual decision
/// with a reason: a CollapsingHeader draws a full-width filled bar, and two of
/// those -- one per card -- read as heavier than the two big buttons they sit
/// under, which is the exact opposite of what this tab is for. A TreeNode is a
/// small arrow and a word.
void drawDetails(const CatalogEntry& entry, const CpuIdentity& identity,
                 const std::function<void(TargetCpu)>& onFlashCpu)
{
    ImGui::PushID("details");
    if (ImGui::TreeNode("Details")) {

        // The same pair FlashController::begin() and the flash dialog apply, so
        // this card, the dialog and the engine cannot disagree about how many
        // steps there are. See dropRedundantErases() (fwFlashPlan.h).
        const auto fullPlan = buildFlashPlan(entry);
        const auto plan     = dropRedundantErases(fullPlan, identity);
        if (plan.empty()) {
            ImGui::TextColored(kMutedColor, "(nothing would be written -- no UF2 assets)");
        } else {
            for (const auto& step : plan) {
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextWrapped("%s -> %s", cpuLabel(step.cpu), step.description.c_str());
            }
        }
        if (const std::string note = droppedEraseNote(fullPlan, identity); !note.empty())
            wrappedColored(kMutedColor, note);

        for (const auto& warning : planWarnings(entry, identity))
            wrappedColored(kErrorColor, std::string(ICON_MD_WARNING " ") + warning);

        // Per-CPU halves, for repairing one CPU without touching the other.
        // Offered only where a caller asked for them (the legacy card), and
        // only for a CPU this entry actually writes -- a button that would
        // produce an empty plan is not drawn at all rather than drawn and then
        // refusing.
        //
        // Neither performs an erase, including the DISPLAY one, whose erase the
        // FULL plan does perform. That is deliberate (see restrictToCpu,
        // fwCatalogFilter.h): each button does exactly what its label says. The
        // consequence -- the two halves do not add up to the whole -- is stated
        // on screen rather than left for someone to discover.
        if (onFlashCpu) {
            ImGui::Spacing();
            ImGui::TextColored(kMutedColor, "Flash one CPU only (no erase):");
            bool drewAny = false;
            for (const TargetCpu cpu : { TargetCpu::Main, TargetCpu::Display }) {
                if (buildFlashPlan(restrictToCpu(entry, cpu)).empty()) continue;
                if (drewAny) ImGui::SameLine();
                drewAny = true;
                ImGui::PushID(cpuLabel(cpu));
                if (ImGui::Button(cpu == TargetCpu::Main ? ICON_MD_BOLT " MAIN only"
                                                          : ICON_MD_BOLT " DISPLAY only"))
                    onFlashCpu(cpu);
                ImGui::PopID();
            }
            if (drewAny)
                wrappedColored(kMutedColor, "These write one image each. Together they are not the "
                                             "same as the button above, which also erases the "
                                             "DISPLAY CPU first.");
        }

        ImGui::TreePop();
    }
    ImGui::PopID();
}

/// One install card's big button, plus whatever single line follows it.
/// Returns true on the frame it is clicked; the caller owns what happens next,
/// because that depends on whether the CPUs have to be identified first.
///
/// `blocked` is a second, non-safety reason to stay disabled -- a flash dialog
/// already open, or an identification already in flight for either card. The
/// SAFETY reason is `reason`, from flashDisabledReason(), and it is always
/// printed when present: a disabled control with no visible explanation is
/// what users file bugs about.
bool drawBigButton(const char* label, const std::string& reason, bool blocked)
{
    // Wide and tall enough to read as the one thing on the card. Capped rather
    // than stretched to the full width: a button as wide as a maximised window
    // reads as a banner, not a button.
    const float width  = std::min(ImGui::GetContentRegionAvail().x, 460.0f);
    const float height = ImGui::GetFrameHeight() * 2.0f;

    ImGui::BeginDisabled(!reason.empty() || blocked);
    const bool clicked = ImGui::Button(label, ImVec2(width, height));
    ImGui::EndDisabled();
    return clicked;
}

/// The MAIN and DISPLAY erase actions. Two cards of the same shape, one per
/// CPU, kept visually and behaviourally apart from the two install buttons
/// above: each is collapsed by default (so scrolling this tab, or clicking
/// through it quickly, never reveals a live button) and each requires its own
/// typed phrase before its button becomes clickable -- on top of, not instead
/// of, the shared FlashDialog's own plan/warning review before "Start
/// Flashing" begins the write. This reuses buildFlashPlan/planWarnings/
/// FlashDialog exactly as the install cards do; nothing here is a second flash
/// path.
///
/// The two entries are pinned to their CPUs by their SCHEMES -- OgApp permits
/// only MAIN, DisplayBootloader only DISPLAY (schemeAllows, fwFlashPlan.cpp) --
/// not by which card drew which button, so a mistake here cannot retarget
/// either one.
void drawEraseCard(const CatalogEntry* entry, TargetCpu cpu, DeviceModel& deviceModel,
                   FlashDialog& flashDialog, char* confirmBuf, std::size_t confirmBufLen)
{
    ImGui::PushID(cpuLabel(cpu));

    if (!entry) {
        ImGui::TextColored(kMutedColor, "Erase %s CPU: no firmware was embedded in this build.",
                           cpuLabel(cpu));
        ImGui::PopID();
        return;
    }

    // Closed by default. Nothing destructive is visible, let alone clickable,
    // until the user deliberately opens this.
    const std::string header = std::string("I understand this destroys the ") + cpuLabel(cpu) +
                               " CPU's firmware -- show the erase action";
    const bool headerOpen = ImGui::CollapsingHeader(header.c_str());
    if (!headerOpen) {
        // A stale confirmation must not survive the header collapsing, whether
        // the user just collapsed it themselves or it was already collapsed
        // from a prior frame -- see the class doc comment
        // (fwTabDefaultFirmware.h). Clearing here, every frame the header reads
        // closed, is what keeps the invariant holding even in cases
        // onTabHidden() (fwApp.cpp) does not reach, such as the user manually
        // re-collapsing the card within the same visit.
        confirmBuf[0] = '\0';
        ImGui::PopID();
        return;
    }

    ImGui::Indent();

    const CpuIdentity identity = previewIdentity(deviceModel);

    // planWarnings() carries this entry's own wording, keyed off its slug
    // (fwFlashPlan.cpp) -- what it destroys and, for DISPLAY, that it is
    // recoverable. Sourced from there rather than duplicated as literals here
    // so this card and the flash dialog can never say different things about
    // the same action.
    for (const auto& warning : planWarnings(*entry, identity))
        wrappedColored(kErrorColor, std::string(ICON_MD_WARNING " ") + warning);

    const char* phrase = detail::erasePhrase(cpu);
    ImGui::Text("Type %s to enable the button below:", phrase);
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("##EraseConfirm", confirmBuf, confirmBufLen);

    // headerOpen is always true at this point (the early return above handles
    // false) -- passed through explicitly rather than hardcoding `true` so the
    // pure predicate's full contract (the button cannot enable when the header
    // is not open, regardless of what `typed` holds) stays testable and stays
    // true even if this function is ever restructured to not early-return.
    const bool confirmed = detail::eraseButtonArmed(headerOpen, cpu, confirmBuf);
    if (!confirmed)
        ImGui::TextColored(kMutedColor, "Not confirmed yet.");

    const std::optional<DeviceView> selectedDevice = deviceModel.selected();
    // A device whose identity could not be CONFIRMED (most often a serial
    // reading fwfinder's "Unknown" sentinel) must not read as "nothing
    // connected". selectedByIdOnly() hands back the raw candidate for WORDING
    // the refusal only -- the click below still gates on selectedDevice.
    const std::optional<DeviceView> rawDevice =
        selectedDevice.has_value() ? selectedDevice : deviceModel.selectedByIdOnly();
    const DeviceView* devPtr = rawDevice.has_value() ? &(*rawDevice) : nullptr;
    const std::string reason = flashDisabledReason(*entry, devPtr, selectedDevice.has_value());

    const std::string button =
        std::string(ICON_MD_DELETE_FOREVER " Erase ") + cpuLabel(cpu) + " CPU";

    ImGui::PushStyleColor(ImGuiCol_Button, kErrorColor);
    ImGui::BeginDisabled(!confirmed || !reason.empty() || flashDialog.isOpen());
    if (ImGui::Button(button.c_str()) && selectedDevice.has_value()) {
        flashDialog.open(*entry, *selectedDevice);
        // A confirmation typed for this click must not silently carry over and
        // re-arm the button for whatever the user does next.
        confirmBuf[0] = '\0';
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor();

    if (!reason.empty())
        wrappedColored(kErrorColor, reason);

    ImGui::Unindent();
    ImGui::PopID();
}

} // namespace

void DefaultFirmwareTab::draw(DeviceModel& deviceModel, FlashDialog& flashDialog,
                              const ProbeAccess& probe,
                              const std::function<void(RecoveryAnchor)>& onOpenRecovery)
{
    // Rebuilt fresh every frame, same spirit as the App Explorer tab's catalog
    // merge: this is the tab a user reaches for when things have gone wrong,
    // and a cached copy from before a rebuild would be exactly the wrong thing
    // to show them.
    const auto entries = embeddedEntries();
    const CatalogEntry* bootloader   = findInstallByScheme(entries, FlashScheme::DisplayBootloader);
    const CatalogEntry* legacy       = findInstallByScheme(entries, FlashScheme::LegacyDirect);
    const CatalogEntry* eraseMain    = findBySlug(entries, kEraseMainCpuSlug);
    const CatalogEntry* eraseDisplay = findBySlug(entries, kEraseDisplayCpuSlug);

    const bool probeRunning = probe.running && probe.running();

    // --- Resolve an install click that was waiting on an identification -----
    //
    // This runs BEFORE any card draws, so the frame the answer lands is the
    // frame the operation continues. The identification the user did not have
    // to ask for is over by now: either it failed, in which case nothing is
    // opened and the reason is shown on the card, or it succeeded, in which
    // case its answer has already been folded into the selected device's
    // CpuIdentity (fwApp.cpp calls setVerifiedVolume() before the tab bar) and
    // the ordinary refusal check below decides -- exactly as it would have for
    // a click that never needed a probe at all.
    //
    // Nothing here weakens a refusal. flashDisabledReason() is re-evaluated
    // against the identity as it now stands, and a probe that has just proved
    // the one mounted drive is the CPU this entry does NOT write to produces a
    // refusal (mappedToTheWrongCpu) rather than an opened dialog. A probe makes
    // a question answerable; it never skips one.
    if (!m_pendingInstallSlug.empty() && !probeRunning) {
        const CatalogEntry* pending = findBySlug(entries, m_pendingInstallSlug.c_str());
        m_autoIdentifyNoteSlug = m_pendingInstallSlug;
        m_pendingInstallSlug.clear();

        const std::string failure = probe.lastFailure ? probe.lastFailure() : std::string{};
        if (!failure.empty()) {
            m_autoIdentifyNote = "Could not identify the CPUs: " + failure;
        } else if (pending) {
            const std::optional<DeviceView> selectedDevice = deviceModel.selected();
            const std::optional<DeviceView> rawDevice =
                selectedDevice.has_value() ? selectedDevice : deviceModel.selectedByIdOnly();
            const DeviceView* devPtr = rawDevice.has_value() ? &(*rawDevice) : nullptr;
            const std::string reason =
                flashDisabledReason(*pending, devPtr, selectedDevice.has_value());
            if (reason.empty() && selectedDevice.has_value() && !flashDialog.isOpen()) {
                m_autoIdentifyNote.clear();
                flashDialog.open(*pending, *selectedDevice);
            } else {
                m_autoIdentifyNote = reason;
            }
        }
    }

    ImGui::BeginChild("##DefaultFirmwareBody", ImVec2(0, 0), ImGuiChildFlags_Border);

    const CpuIdentity identity = previewIdentity(deviceModel);
    const std::optional<DeviceView> selectedDevice = deviceModel.selected();
    const std::optional<DeviceView> rawDevice =
        selectedDevice.has_value() ? selectedDevice : deviceModel.selectedByIdOnly();
    const DeviceView* devPtr = rawDevice.has_value() ? &(*rawDevice) : nullptr;

    // An install click is disabled while a flash dialog is open or while an
    // identification -- this tab's own, or one started from Recovery -- is in
    // flight. Neither is a safety refusal; both are "something else is already
    // happening", which is why they are kept apart from `reason` below.
    const bool busy = flashDialog.isOpen() || probeRunning || !m_pendingInstallSlug.empty();

    // Both cards' refusals, computed up front so an identical one can be said
    // ONCE instead of twice.
    //
    // Every refusal that is about the BOARD rather than about the entry -- no
    // device, not an OG, an unconfirmed serial, the web build -- is by
    // construction the same sentence on both cards, and printing it twice was
    // the single noisiest thing left on this tab: two copies of a
    // two-line-wrapping red paragraph above two buttons that are obviously both
    // disabled for the same reason. It is said once, above both, where it is no
    // less visible and no less specific.
    //
    // Nothing is hidden and nothing is weakened: the buttons are still disabled
    // by their OWN reason (see the `reason` argument to drawBigButton below,
    // which is unchanged), and a refusal that differs between the two cards --
    // an entry with no assets, or one whose CPU a probe has measured the one
    // mounted drive NOT to be -- is still printed per card, because then the
    // two cards really are saying different things.
    const std::string bootloaderReason =
        bootloader ? flashDisabledReason(*bootloader, devPtr, selectedDevice.has_value())
                   : std::string{};
    const std::string legacyReason =
        legacy ? flashDisabledReason(*legacy, devPtr, selectedDevice.has_value())
               : std::string{};
    const bool sharedReason = !bootloaderReason.empty() && bootloaderReason == legacyReason;
    if (sharedReason)
        wrappedColored(kErrorColor, bootloaderReason);

    /// Draws one install card and, when its button is clicked, either opens the
    /// flash dialog or starts the identification that clicking it needs first.
    const auto installCard = [&](const CatalogEntry* entry, const char* label,
                                 const std::string& reason, const std::string& version,
                                 const char* subtitle) {
        if (!entry) {
            ImGui::TextColored(kMutedColor, "%s: no firmware was embedded in this build.", label);
            return;
        }
        ImGui::PushID(entry->slug.c_str());

        // ONE start path, shared by the big button and by the Details section's
        // per-CPU buttons. `what` is the entry to flash -- the card's own entry,
        // or a restrictToCpu() copy of it -- so the identification decision, the
        // auto-probe and the dialog are identical whichever button was pressed.
        const auto startInstall = [&](const CatalogEntry& what) {
            m_autoIdentifyNote.clear();
            m_autoIdentifyNoteSlug.clear();
            const std::size_t volumes = probe.mountedVolumes ? probe.mountedVolumes() : 0u;
            // Asked FIRST: when the scan has already located every CPU this
            // entry writes to -- a port to touch, or a bootrom drive named by
            // its port on the board's internal hub -- there is nothing for the
            // prober to establish. Two mounted drives are the board's own two
            // CPUs on their own two hub ports, not an ambiguity, so going and
            // writing a prober image to one of them would burn a flash cycle
            // re-answering a question the USB tree already answered.
            //
            // Folded into the decision rather than returning early: the rest of
            // this lambda still has the card's version line and status to draw,
            // and skipping them for the one frame of the click would make the
            // card flicker.
            const auto decision =
                detail::locatedWithoutProbe(what, selectedDevice->identity)
                    ? detail::AutoIdentify::NotNeeded
                    : detail::autoIdentifyDecision(volumes, deviceModel.devices().size());
            switch (decision) {
            case detail::AutoIdentify::NotNeeded:
                flashDialog.open(what, *selectedDevice);
                break;
            case detail::AutoIdentify::Run:
                // "Behind the scenes the app should just figure it out": two
                // indistinguishable RPI-RP2 volumes are mounted, so this write
                // would be refused as ambiguous. Run the identification as part
                // of the operation the user already asked for, and continue
                // above once it lands.
                m_pendingInstallSlug = entry->slug;
                if (probe.begin) probe.begin();
                break;
            case detail::AutoIdentify::Blocked:
                m_autoIdentifyNote =
                    detail::autoIdentifyBlockedReason(volumes, deviceModel.devices().size());
                break;
            }
        };

        if (drawBigButton(label, reason, busy) && selectedDevice.has_value())
            startInstall(*entry);

        drawVersionLine(version);
        if (subtitle) ImGui::TextColored(kMutedColor, "%s", subtitle);

        // At most ONE status line, and only when there is something to say. A
        // refusal outranks everything below it -- and when it is the shared one
        // already printed above both cards, it outranks them SILENTLY here,
        // rather than falling through to reprint a stale note underneath a
        // button that is refusing for a different reason entirely.
        if (!reason.empty()) {
            if (!sharedReason) wrappedColored(kErrorColor, reason);
        } else if (m_pendingInstallSlug == entry->slug)
            wrappedColored(kBusyColor, "Identifying which CPU is which...");
        else if (!m_autoIdentifyNote.empty() && m_autoIdentifyNoteSlug == entry->slug)
            wrappedColored(kErrorColor, m_autoIdentifyNote);

        // Per-CPU buttons only on the LEGACY card -- it is the only entry that
        // writes both CPUs, so it is the only one where "just this half" means
        // anything. The bootloader card writes DISPLAY (and erases MAIN), and
        // splitting that would offer an erase-only button with no label saying
        // so.
        drawDetails(*entry, identity,
                    entry->scheme == FlashScheme::LegacyDirect
                        ? std::function<void(TargetCpu)>([&](TargetCpu cpu) {
                              if (selectedDevice.has_value())
                                  startInstall(restrictToCpu(*entry, cpu));
                          })
                        : std::function<void(TargetCpu)>{});
        ImGui::PopID();
    };

    ImGui::Spacing();

    installCard(bootloader, ICON_MD_MEMORY " Install FreeWili OG Bootloader", bootloaderReason,
                bootloader ? singleAssetVersion(*bootloader) : std::string{}, nullptr);

    ImGui::Spacing();
    ImGui::Spacing();

    installCard(legacy, ICON_MD_HISTORY " Original FreeWili (deprecated firmware)", legacyReason,
                legacy ? detail::legacyVersion(*legacy) : std::string{},
                "Removes the display bootloader.");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    ImGui::TextUnformatted(ICON_MD_DANGEROUS " Danger zone");
    ImGui::PopStyleColor();

    drawEraseCard(eraseMain, TargetCpu::Main, deviceModel, flashDialog,
                  m_eraseConfirm[eraseSlot(TargetCpu::Main)],
                  sizeof(m_eraseConfirm[0]));
    drawEraseCard(eraseDisplay, TargetCpu::Display, deviceModel, flashDialog,
                  m_eraseConfirm[eraseSlot(TargetCpu::Display)],
                  sizeof(m_eraseConfirm[0]));

    // The doorway to the reference material this tab used to print inline.
    // BootloaderMissing is the section that now carries it: when each install
    // applies, why a chattering MAIN CPU stops one starting, and what erasing
    // either CPU is for.
    ImGui::Spacing();
    if (ImGui::SmallButton(ICON_MD_OPEN_IN_NEW " When to use these"))
        onOpenRecovery(RecoveryAnchor::BootloaderMissing);

    ImGui::EndChild();
}

void DefaultFirmwareTab::onTabHidden()
{
    // See the class doc comment: this closes the gap each CollapsingHeader's
    // own persisted open/closed state leaves open. draw() is only ever called
    // while this tab is the active tab (fwApp.cpp's tab-bar loop), so this
    // object has no way to notice a gap in visits on its own -- fwApp.cpp calls
    // this explicitly the frame the active tab stops being this one.
    m_eraseConfirm[0][0] = '\0';
    m_eraseConfirm[1][0] = '\0';
    // A click still waiting on an identification is dropped rather than carried
    // across the boundary: an identification started for a click made on a
    // previous visit must not open a flash dialog on a later one. The
    // identification itself keeps running (it is the app's, not this tab's, and
    // the Recovery tab reports it) -- only this tab's intent to act on it goes.
    m_pendingInstallSlug.clear();
    m_autoIdentifyNote.clear();
    m_autoIdentifyNoteSlug.clear();
}

} // namespace fwog
