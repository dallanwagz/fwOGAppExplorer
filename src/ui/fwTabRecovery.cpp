#include "ui/fwTabRecovery.h"

#include "device/fwDeviceModel.h"
#include "flash/fwCpuProbe.h"
#include "platform/fwSerialPorts.h"
#include "platform/fwSerialTouch.h"
#include "platform/fwVolume.h"

#include <imgui.h>
#include <IconsMaterialDesign.h>

#include <algorithm>
#include <string>

namespace fwog {

// ---------------------------------------------------------------------------
// Recovery content. Written for someone whose board has just stopped
// working: what state you are in, why the app refused or the board failed,
// and what to actually do about it. Plain and specific -- no cheerfulness,
// no blame, nothing hedged with "simply". Ten sections. Nine are each keyed to
// a real refusal the flash engine can produce (see fwRecoveryContent.h and
// fwFlashEngine.h's FlashOutcome, which every anchor here is meant to cover);
// the tenth, BoardSerialUnidentified (Task 22), documents a state caught
// earlier -- by flashDisabledReason() disabling the Flash button before a
// flash is ever attempted -- so it has no FlashOutcome of its own.
//
// Kept here rather than in a separate fwRecoveryContent.cpp: this tab is the
// only place that ever needs the actual prose, and defining it next to the
// widget that renders it means there is exactly one file to touch when the
// wording needs a correction.
// ---------------------------------------------------------------------------

const RecoverySection kRecoverySections[] = {
    {
        RecoveryAnchor::TwoVolumes,
        "Two RPI-RP2 drives are mounted",
        R"TXT(Two RPI-RP2 volumes are mounted at the same time. The FreeWili OG has two RP2040 microcontrollers -- a MAIN CPU and a DISPLAY CPU -- and each one presents an identical RPI-RP2 volume once it is in BOOTSEL mode. With both mounted, nothing can tell which drive belongs to which CPU from the mount alone: not this app, not picotool, not you. Writing the wrong image to the wrong CPU can damage the board (see "A main-CPU image landed on the DISPLAY CPU" below), so the app refuses to guess.

If one of the two is a CPU you did not mean to put into bootloader mode, the simple fix is still the right one: eject or unmount that drive, leaving only the CPU you intend to flash, and retry.

If BOTH CPUs are erased, there is no drive to unmount -- an RP2040 with blank flash presents RPI-RP2 all by itself, with no button and nothing running. That state is reachable using this app alone (erase the MAIN CPU here, then start the Original FreeWili install, whose first step erases the DISPLAY CPU), and until now it was a dead end: every path refused, including the ones that would have fixed it.

The Identify CPUs action below is the way out of that state, and only that state. The Default Firmware tab's two install buttons now run this same identification THEMSELVES when a click needs it, so getting out no longer requires finding this page first; the button below stays for running it deliberately and for reading what it found. It works because the drives are not actually identical -- only their labels are. The CC1101 sub-GHz radio is wired to the MAIN CPU only, so a small image that probes for the radio and reports what it found can tell you which CPU it is running on. Writing it to one of the two drives makes that drive's CPU reboot into the prober, which both answers the question and removes that drive from the picture; the drive still mounted is the other CPU, by elimination.

That image is the one thing this app will write to a CPU it has not identified, and it is safe to do so for one specific reason: it never configures or drives GPIO 29. GPIO 29 is the PDM microphone's output on the DISPLAY CPU and FPGA_RESET on the MAIN CPU, and a main image driving it on the DISPLAY CPU is precisely the damage every other refusal here exists to prevent. Nothing else in this app is permitted to reach an unidentified CPU, and clicking Flash with two drives mounted still refuses, exactly as it did before -- the answer this produces is only ever about ONE drive, so a second one appearing beside it discards the answer rather than extending it.

Once the answer is in, that drive is no longer unidentified, and the app treats it accordingly. Firmware for the CPU it belongs to is written straight to it -- no 1200-baud reboot (there is nothing running on a CPU in its bootloader to reboot, and it is already where a reboot would put it) and no typed CPU name. That is not the confirmation being waived: the confirmation exists because a mount carries no evidence of which CPU it is, and a probe result is exactly that evidence, measured off the radio rather than read off a USB string. The same knowledge refuses in the other direction too -- firmware for the OTHER CPU is now rejected outright instead of being offered a confirmation box, which is a case that used to be accepted and should not have been.

The answer is discarded the moment the mounted drives are not exactly the one it named -- another drive appearing, that drive going away, a replug -- and after two minutes regardless, because a drive letter is a reusable name and a board that has been replugged can hand the same letter to the other CPU. When that happens the device bar says so; it does not quietly go back to refusing.)TXT"
    },
    {
        RecoveryAnchor::DisplayNotIdentified,
        "The DISPLAY CPU could not be identified",
        R"TXT(The DISPLAY CPU has no reachable BOOTSEL button. To get it into its bootloader, the app instead opens its USB serial port and drops the connection at 1200 baud -- a trick that only works if the display is already running firmware with its USB port up.

Two things stop that port from existing:
- Firmware built with FWOG_DIAG off has no USB serial at all -- there is nothing to open.
- The display bootloader's own console only enumerates after the MAIN CPU has gone quiet on the inter-CPU link for 10 seconds. A MAIN CPU that is still running application firmware keeps talking indefinitely, so the DISPLAY console never appears no matter how long you wait -- this is not a "wait a little longer" problem.

If the MAIN CPU is currently running its own firmware (or you are not sure), waiting will not help: go to the Default Firmware tab and erase the MAIN CPU first (Danger zone -> Erase MAIN CPU). That silences it, and the DISPLAY console should enumerate within about 10 seconds afterward. Only do this when the FreeWili 1-OG bootloader install actually will not start -- it is a destructive, one-way step for the MAIN CPU's own firmware, not something to reach for by default.

If MAIN is already silent -- freshly erased, or the board has never been flashed at all -- and the DISPLAY port still does not appear, unplug and replug the board, wait about 15 seconds without touching it, then refresh the device list before concluding the port is genuinely gone.

This 10-second rule is also why the Original FreeWili Firmware install erases and writes the DISPLAY CPU BEFORE it writes the MAIN CPU. Once the original main firmware is running, this port stops appearing at all, so a DISPLAY step scheduled after it can never succeed. One exception worth knowing: within that install, the step that writes the display firmware does not need this port -- the erase immediately before it leaves the DISPLAY CPU presenting an RPI-RP2 volume on its own.)TXT"
    },
    {
        RecoveryAnchor::MainNotIdentified,
        "The MAIN CPU could not be identified",
        R"TXT(This is different from the DISPLAY CPU's identification problem, and the fix is different too: the MAIN CPU has a reachable BOOTSEL button.

Unplug the board. Hold the MAIN CPU's BOOTSEL button down, plug the USB cable back in while still holding it, and keep holding until an RPI-RP2 volume appears -- then release it and retry.

If no volume appears even with BOOTSEL held, check the cable next: a charge-only USB cable carries power but no data and is the single most common reason nothing enumerates. See "Nothing enumerates over USB" below.)TXT"
    },
    {
        RecoveryAnchor::BootloaderMissing,
        "The display bootloader is missing",
        R"TXT(Symptoms: the display stays dark, and the main CPU's application firmware is otherwise running normally. This is what a FreeWili OG looks like before the wiliOGBsp display bootloader has ever been installed, or after an Original FreeWili firmware install removed it.

Fix: Default Firmware tab -> FreeWili 1-OG -> install. That writes bl_display.uf2 to the DISPLAY CPU. Afterward, app firmware for the DISPLAY CPU is embedded in the MAIN CPU's UF2 and arrives automatically over the inter-CPU link, so an ordinary app install flashes both CPUs from one file.

That install is only for a board that does not already have the display bootloader. Running it again on a board that does is unnecessary, though not harmful -- it is simply not the normal case.

The install needs the DISPLAY CPU's own USB console, and that console only enumerates after about 10 seconds of MAIN-CPU silence -- so on a board whose MAIN CPU is running application firmware, the install may never even start. Erase the MAIN CPU first (Default Firmware -> Danger zone -> Erase MAIN CPU); once it is quiet, the install can proceed. The app now runs the CPU identification for you when it needs it, but it cannot make a chattering MAIN CPU stop talking -- that is what the erase is for.

If the bootloader is not missing but BROKEN -- installed, yet the DISPLAY CPU will not come up and will not present a console to reinstall through -- Danger zone -> Erase DISPLAY CPU is the way back. It writes the standard Pico flash-erase image to the DISPLAY CPU, and a blank RP2040 presents an RPI-RP2 volume by itself, with no button, which is exactly the state the bootloader install can then be written into. It is destructive and it is recoverable, in that order.

One case this app cannot drive through on its own: if the DISPLAY CPU hangs before its bootloader's USB console comes up AND it cannot be reached to erase either, there is no port to open at 1200 baud and no drive to copy to. The display bootloader is the one component whose replacement can require physical BOOTSEL access on the DISPLAY CPU.)TXT"
    },
    {
        RecoveryAnchor::WrongImageOnWrongCpu,
        "A main-CPU image landed on the DISPLAY CPU",
        R"TXT(A main-CPU image running on the DISPLAY CPU drives GPIO 29 as an output, directly against the PDM microphone's own output on that same pin. Two outputs fighting over one pin is a hazard to the microphone and to whatever is driving it. This is why the app demands you type the CPU's name before it will write to a volume it did not create itself -- an unidentified volume carries no evidence of which CPU it belongs to.

Unplug the board. Reflash the correct image for that CPU: the display bootloader (or a properly targeted display image) for the DISPLAY CPU, the app image for the MAIN CPU. Do not leave the board powered with a main image running on DISPLAY any longer than it takes to notice.)TXT"
    },
    {
        RecoveryAnchor::ImageRejected,
        "The image was rejected before anything was written",
        R"TXT(The app refused to write this image. That check runs before the board is ever touched -- no port was rebooted into BOOTSEL and no bytes were copied -- so whatever firmware is already on the board is untouched and still there.

Two distinct reasons this happens:
- A downloaded file that is truncated or corrupted fails basic UF2 validation and is rejected outright.
- A file that parses as a valid UF2 but does not match its published checksum is rejected too -- most often because the download is corrupt, occasionally because it is an image built for a different chip or the wrong CPU, checked and stopped before it could reach the board.

Re-download the file and try again, or, if what you are after is one of the two curated bundles, use the Default Firmware tab instead -- that firmware is embedded in this executable and needs no network.)TXT"
    },
    {
        RecoveryAnchor::WriteInterrupted,
        "The write did not finish",
        R"TXT(The write to the board's RPI-RP2 volume did not complete. The CPU that was being written to may now hold a partial image and may not run anything meaningful until it is reflashed.

This is not the same as a bricked board: the RP2040's own bootloader lives permanently in on-chip ROM and a bad write cannot erase it. On the MAIN CPU, holding BOOTSEL while plugging in gets you back into bootloader mode regardless of whatever is currently in flash.

Re-run the same install; writing over a partial image is safe. If the CPU that failed was the DISPLAY CPU and it will not come back on its own, see "The DISPLAY CPU could not be identified" above for how to reach its bootloader without a BOOTSEL button.)TXT"
    },
    {
        RecoveryAnchor::PartialLegacyFlash,
        "Only part of the Original FreeWili install completed",
        R"TXT(The Original FreeWili Firmware install writes three images in sequence, and the order is not arbitrary:

1. ERASE the DISPLAY CPU (the standard Pico flash-erase image)
2. WRITE the display firmware to the DISPLAY CPU
3. WRITE the main firmware to the MAIN CPU

The MAIN CPU goes LAST on purpose. The DISPLAY CPU's bootloader console only enumerates after about 10 seconds of MAIN-CPU silence, and the original main firmware talks on the inter-CPU link continuously -- so once step 3 has run, the DISPLAY CPU can no longer be reached at all. Doing MAIN first, which is what this app used to do, did not merely risk the display step: it made it unreachable, and it stranded the board in a mixed state every time. If you are reading this because a MAIN-first install left your board half-done, re-running the install now does the DISPLAY CPU first and finishes the job.

Where it stopped decides what you are looking at:
- Stopped after step 1. The DISPLAY CPU is blank and sitting in its own bootloader. This is the most recoverable state on the list, not the worst: an RP2040 with erased flash presents an RPI-RP2 volume by itself, with no button, so the next run can write straight to it. The MAIN CPU is untouched and still running whatever it was.
- Stopped after step 2. Both display images have landed; the MAIN CPU is untouched and still running its previous firmware, which will not match the display firmware now installed. Nothing is damaged; the board is simply mismatched until step 3 runs.
- Stopped during step 3. The DISPLAY CPU is fully installed and the MAIN CPU may hold a partial image. Hold the MAIN CPU's BOOTSEL button while plugging in and reflash it; MAIN is the CPU that always has that way back.

The flash dialog's own message names exactly which steps completed, including whether one of them was the erase.

What the install does regardless of where it stopped: it REMOVES the wiliOGBsp display bootloader, from step 1 onward. That is what this install is for, not a side effect -- but it means "the display bootloader is missing" is now also true, whatever the later steps did or did not manage.

There is no partial fix to reach for here. Re-run the whole Original FreeWili Firmware install from the Default Firmware tab. Writing the same image twice is safe, and the second run finishes what the first one left undone. If step 1 or step 2 keeps failing because the DISPLAY CPU cannot be identified, read "The DISPLAY CPU could not be identified" above first -- erasing the MAIN CPU is what silences the link and lets the display console enumerate.)TXT"
    },
    {
        RecoveryAnchor::NothingEnumerates,
        "Nothing enumerates over USB",
        R"TXT(Check the cable first. A charge-only USB cable -- power only, no data lines -- is the single most common reason nothing shows up, and it looks identical to one that works. Swap it for a cable you know carries data, and try a different USB port; some hubs and front-panel headers are unreliable for a device drawing power during enumeration.

If, after that, one CPU enumerates and the other does not, that is a firmware problem on the CPU that stayed silent, not a cable problem -- see "The DISPLAY CPU could not be identified" or "The display bootloader is missing" above, depending on which CPU it is and what state it is in.)TXT"
    },
    {
        RecoveryAnchor::BoardSerialUnidentified,
        kBoardSerialUnidentifiedTitle,
        R"TXT(This is different from either CPU's identification problem above, and it does not stop the MAIN or DISPLAY port from being found. The FreeWili OG's own identifying serial number is read from a separate FTDI USB-to-serial chip on the board, not from either RP2040's own serial port. If that chip's USB device entry is not found during a scan, the board's serial shows as "Unknown" instead of its real number -- the device still appears in the device bar, with both CPUs identified normally, but its own identity has not.

This app refuses to flash a device it cannot re-identify by serial, on purpose: the serial is what proves this is still the SAME physical board a moment from now as it is right now, and that is exactly the check that stops a flash from silently landing on a different board that gets swapped into the same USB port. An "Unknown" serial cannot make that promise, so the refusal holds even though the board is clearly connected and its CPUs are clearly identified.

Unplug and replug the board, wait a few seconds for USB enumeration to finish, then rescan. If it keeps coming back "Unknown" on the same board, try a different USB port or a cable you know carries data -- see "Nothing enumerates over USB" above for why that matters even when most things ARE enumerating.)TXT"
    },
};

const int kRecoverySectionCount = static_cast<int>(sizeof(kRecoverySections) / sizeof(kRecoverySections[0]));

namespace {

const ImVec4 kMutedColor { 0.65f, 0.65f, 0.65f, 1.00f };
const ImVec4 kErrorColor { 0.95f, 0.35f, 0.35f, 1.00f };
const ImVec4 kOkColor    { 0.45f, 0.85f, 0.45f, 1.00f };
const ImVec4 kWarnColor  { 0.90f, 0.65f, 0.15f, 1.00f };

void wrappedColored(const ImVec4& color, const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

/// Why the Identify CPUs button is not available, or empty when it is.
///
/// Every one of these is a precondition of the by-elimination step being SOUND,
/// not a convenience check:
///  - Exactly two volumes, because the conclusion is "the one that is left".
///  - At most one FreeWili connected, because two boards with one erased CPU
///    each also produce two RPI-RP2 volumes, and writing the prober to one
///    board's volume says nothing whatsoever about the other board's. This is
///    the one precondition the flow itself cannot check -- identifyCpus() sees
///    volumes and ports, not boards -- so it is checked here, where the device
///    list is.
std::string identifyDisabledReason(const std::vector<std::string>& volumes,
                                    const DeviceModel& deviceModel)
{
    if (!kDeviceSupportAvailable)
        return std::string(platformLimitationNotice());
    if (deviceModel.devices().size() > 1)
        return "More than one FreeWili is connected. Two boards can present two RPI-RP2 "
               "volumes between them, and the prober's answer would then say nothing about "
               "the other board's drive. Unplug all but the board you are recovering.";
    if (volumes.size() != 2)
        return "This needs exactly two RPI-RP2 volumes mounted; " +
               std::to_string(volumes.size()) + " " + (volumes.size() == 1 ? "is" : "are") +
               " mounted right now. With one, the flash dialog already asks you to type the "
               "CPU name instead.";
    return {};
}

} // namespace

void TabRecovery::poll()
{
    m_probe.poll();
    // Every frame (self-throttled), not only while this tab is visible: see
    // poll()'s header comment. verifiedVolume() below is read by the device
    // bar and, through it, by the flash path, so its evidence has to be
    // refreshed on the app's schedule rather than the user's.
    //
    // VOLUMES ONLY. The port list is not promoted to always-on with it: it
    // feeds nothing but this tab's manual picker, and listSerialPorts() is a
    // full SetupDi device-class enumeration -- paying for that twice a second
    // for the app's whole life, in every session where nobody opens this tab,
    // would be a cost with no reader.
    refreshVolumes();
}

VerifiedVolume TabRecovery::verifiedVolume() const
{
    if (m_probe.state() != ProbeState::Finished) return {};
    // The whole freshness rule lives in verifiedVolumeFrom(); nothing is
    // re-derived here, and nothing is cached, so a mapping cannot survive one
    // frame past the volumes it describes.
    return verifiedVolumeFrom(m_probe.result(), m_volumes, m_probe.identificationAge());
}

std::string TabRecovery::identificationNote() const
{
    if (m_probe.state() != ProbeState::Finished) return {};
    const IdentifyResult& r = m_probe.result();
    if (r.outcome != IdentifyOutcome::Success) return {};

    const MappingCheck check = mappingStillFresh(r, m_volumes, m_probe.identificationAge());
    if (check == MappingCheck::Fresh)
        return std::string(r.remainingVolume) + " is the " + probeCpuName(r.remainingCpu) +
               " CPU (measured by the Recovery tab's CPU probe). Firmware for that CPU can be "
               "flashed straight to it.";

    // The case this function exists for. Say that something WAS established and
    // has now been dropped, and why -- not merely that nothing is known, which
    // is what an empty string here would amount to.
    return "The CPU identification from the Recovery tab has been discarded. " +
           mappingCheckMessage(check);
}

bool TabRecovery::identificationDiscarded() const
{
    if (m_probe.state() != ProbeState::Finished) return false;
    const IdentifyResult& r = m_probe.result();
    if (r.outcome != IdentifyOutcome::Success) return false;
    return mappingStillFresh(r, m_volumes, m_probe.identificationAge()) != MappingCheck::Fresh;
}

bool TabRecovery::isIdentifyRunning() const
{
    return m_probe.state() == ProbeState::Running;
}

void TabRecovery::beginIdentify(DeviceModel& deviceModel)
{
    // A SNAPSHOT, taken here on the UI thread, exactly as the flash dialog
    // does it: the worker must never touch DeviceModel. An unidentified or
    // absent selection yields an empty CpuIdentity, which is the honest input
    // -- the contradiction check in identifyCpus() treats "no port" as no
    // evidence, never as confirmation.
    CpuIdentity identity;
    if (const auto selected = deviceModel.selected())
        identity = selected->identity;
    else if (const auto raw = deviceModel.selectedByIdOnly())
        identity = raw->identity;
    m_probe.begin(identity);
}

std::string TabRecovery::identifyFailure() const
{
    if (m_probe.state() != ProbeState::Finished) return {};
    const IdentifyResult& r = m_probe.result();
    if (r.outcome == IdentifyOutcome::Success) return {};
    return r.message;
}

namespace {

/// True (and stamps `at`) when at least kPollIntervalMs has passed. Shared by
/// the two throttled reads below so neither can drift from the other's rate.
bool dueFor(std::optional<std::chrono::steady_clock::time_point>& at, int intervalMs)
{
    const auto now = std::chrono::steady_clock::now();
    if (at.has_value() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - *at).count() < intervalMs)
        return false;
    at = now;
    return true;
}

} // namespace

void TabRecovery::refreshVolumes()
{
    if (!dueFor(m_volumesPolledAt, kPollIntervalMs)) return;
    m_volumes = findRpiRp2Volumes();
}

void TabRecovery::refreshPorts()
{
    if (!dueFor(m_portsPolledAt, kPollIntervalMs)) return;
    m_ports = listSerialPorts();
}

void TabRecovery::draw(DeviceModel& deviceModel, const std::function<void(int)>& onGoToTab,
                       const BoardImage& board)
{
    // Volumes are NOT re-read here -- poll() owns them now and refreshes them
    // every frame regardless of which tab is visible. The port list is the
    // half that only this tab reads, so it is read only while this tab draws.
    refreshPorts();

    // onGoToTab is unused now that the tab is two procedures and a photograph:
    // the "you can flash that CPU now" jump belonged to the identification
    // panel this tab no longer draws. The parameter stays because App::run()
    // owns the tab bar and would have to be changed back the moment anything
    // here needs to send the user somewhere.
    (void)deviceModel;
    (void)onGoToTab;

    ImGui::BeginChild("##RecoveryBody", ImVec2(0, 0), ImGuiChildFlags_Border);

    ImGui::TextWrapped("There are exactly two ways to put a FreeWili CPU into its bootloader "
                       "by hand. Both make that CPU appear as an RPI-RP2 drive, which is what "
                       "the Default Firmware and App Explorer tabs write to.");
    ImGui::Spacing();

    // --- 1. MAIN --------------------------------------------------------
    ImGui::SeparatorText(ICON_MD_MEMORY " MAIN CPU -- the red button");
    ImGui::Indent();
    ImGui::TextWrapped("Disconnect the battery, then plug the board into USB with the red "
                       "button held down.");
    ImGui::TextColored(kMutedColor, "The battery has to come out: with it connected the board "
                                     "never actually powers down, so the button is not being "
                                     "read at the moment that decides this.");
    ImGui::Unindent();
    ImGui::Spacing();

    // --- 2. DISPLAY -----------------------------------------------------
    ImGui::SeparatorText(ICON_MD_MEMORY " DISPLAY CPU -- the BOOTSEL pad");
    ImGui::Indent();
    ImGui::TextWrapped("Short the DISPLAY BOOTSEL pad to ground while the board powers up. "
                       "Hold the short as power comes on, then release it. An easy way to "
                       "connect to ground is the GND on the 20 position connector.");
    ImGui::TextColored(kMutedColor, "The pad and a ground point are both marked on the board "
                                     "below.");
    ImGui::Unindent();
    ImGui::Spacing();

    // --- The board ------------------------------------------------------
    ImGui::SeparatorText(ICON_MD_IMAGE " Where the pads are");
    if (board.texture && board.w > 0 && board.h > 0) {
        // Scaled to the pane, and only ever DOWN. Enlarging a 794px photo to
        // fill a maximised window would make the pads blurrier, not easier to
        // find, which is the opposite of what this picture is for.
        const float avail = ImGui::GetContentRegionAvail().x;
        const float scale = avail < float(board.w) ? avail / float(board.w) : 1.0f;
        ImGui::Image(ImTextureRef(board.texture),
                     ImVec2(float(board.w) * scale, float(board.h) * scale));
    } else {
        // Degraded, never silent: the two procedures above are still complete
        // instructions without the picture, so say what is missing rather than
        // leaving an unexplained gap where a diagram should be.
        ImGui::TextColored(kWarnColor,
            ICON_MD_WARNING " The board diagram could not be loaded in this build.");
    }

    ImGui::EndChild();
}

void TabRecovery::drawWhatIsNowPossible(const IdentifyResult& result,
                                        const std::function<void(int)>& onGoToTab)
{
    const char* cpu = probeCpuName(result.remainingCpu);
    const bool display = (result.remainingCpu == TargetCpu::Display);

    ImGui::Spacing();
    ImGui::TextUnformatted(ICON_MD_BOLT " You can flash that CPU now");
    // One line. What this measurement then permits and refuses is set out in
    // full in this section's own prose above; repeating it here made the panel
    // longer than the answer it was reporting.
    ImGui::TextWrapped("%s is the %s CPU, measured. Firmware for the %s CPU writes straight "
                       "to it; anything aimed at the other CPU is refused.",
                       result.remainingVolume.c_str(), cpu, cpu);

    ImGui::Spacing();
    ImGui::TextWrapped(
        "%s", display
            ? "Default Firmware -> FreeWili 1-OG installs the DISPLAY CPU's bootloader. "
              "App Explorer's apps are MAIN-CPU images and are refused here."
            : "App Explorer for an application image, or Default Firmware for the Original "
              "FreeWili install. Either writes the MAIN CPU.");

    ImGui::Spacing();
    // The deep link. Two buttons rather than one guessed destination: which
    // firmware someone wants is theirs to decide, and the sentence above
    // already says which one fits. The recommended tab leads.
    if (display) {
        if (ImGui::Button(ICON_MD_OPEN_IN_NEW " Open Default Firmware")) onGoToTab(1);
        ImGui::SameLine();
        if (ImGui::Button(ICON_MD_OPEN_IN_NEW " Open App Explorer"))     onGoToTab(0);
    } else {
        if (ImGui::Button(ICON_MD_OPEN_IN_NEW " Open App Explorer"))     onGoToTab(0);
        ImGui::SameLine();
        if (ImGui::Button(ICON_MD_OPEN_IN_NEW " Open Default Firmware")) onGoToTab(1);
    }

    ImGui::Spacing();
    ImGui::TextColored(kMutedColor,
        "Do this BEFORE the button below: bringing the other CPU back first puts a second "
        "indistinguishable drive on the bus and everything refuses again.");
}

void TabRecovery::drawIdentifyPanel(DeviceModel& deviceModel, const std::function<void(int)>& onGoToTab)
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(ICON_MD_FINGERPRINT " Identify CPUs");
    ImGui::Spacing();

    std::string mounted;
    for (const auto& v : m_volumes) {
        if (!mounted.empty()) mounted += ", ";
        mounted += v;
    }
    ImGui::TextColored(kMutedColor, "Mounted RPI-RP2 volumes: %s",
                       mounted.empty() ? "(none)" : mounted.c_str());

    const std::string reason = identifyDisabledReason(m_volumes, deviceModel);
    const bool running = m_probe.state() == ProbeState::Running;

    // --- The action -------------------------------------------------------
    ImGui::BeginDisabled(!reason.empty() || running);
    // beginIdentify() rather than an inline m_probe.begin(): the Default
    // Firmware tab's install buttons start an identification too, and both
    // must take the identity snapshot by the same rule.
    if (ImGui::Button(ICON_MD_TROUBLESHOOT " Identify CPUs"))
        beginIdentify(deviceModel);
    ImGui::EndDisabled();

    if (!reason.empty()) {
        ImGui::SameLine();
        wrappedColored(kErrorColor, reason);
    }

    if (running) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_MD_CANCEL " Cancel"))
            m_probe.cancel();
    }

    // --- What it has done so far ------------------------------------------
    if (!m_probe.log().empty()) {
        ImGui::Spacing();
        ImGui::BeginChild("##IdentifyLog", ImVec2(0, 110), ImGuiChildFlags_Border);
        for (const auto& line : m_probe.log())
            ImGui::TextWrapped("%s", line.c_str());
        ImGui::EndChild();
    }

    // --- The answer, and whether it is still an answer ---------------------
    if (m_probe.state() == ProbeState::Finished) {
        const IdentifyResult& r = m_probe.result();
        if (r.outcome != IdentifyOutcome::Success) {
            wrappedColored(kErrorColor, ICON_MD_WARNING " Not identified: " + r.message);
        } else {
            // Re-checked EVERY FRAME against the volumes as they are now, not
            // recorded once when the answer arrived. A mapping is a claim about
            // drive letters, and drive letters are reusable names -- see
            // mappingStillFresh(), which this delegates to rather than
            // re-deriving the rule here.
            const MappingCheck check =
                mappingStillFresh(r, m_volumes, m_probe.identificationAge());
            if (check != MappingCheck::Fresh) {
                wrappedColored(kErrorColor,
                    std::string(ICON_MD_WARNING " This identification no longer applies. ") +
                    mappingCheckMessage(check));
            } else {
                wrappedColored(kOkColor, std::string(ICON_MD_INFO " ") + r.message);
                // Everything the answer is now GOOD FOR, including the way to
                // go and use it. Stating a fact and stopping there is what made
                // this feature unusable on the board it was written for.
                drawWhatIsNowPossible(r, onGoToTab);
            }

            // Deliberately OUTSIDE the freshness branch above, and this is not
            // a slip. The mapping and the port are two different facts with two
            // different lifetimes: "which CPU is drive E:" is only true while
            // the volumes are unchanged, whereas "the prober is on COM99" stops
            // being true only when that port goes away. Gating this button on
            // the mapping's freshness made it unreachable at exactly the moment
            // it is needed -- flashing the remaining volume unmounts it, which
            // invalidates the mapping, which is precisely the point at which
            // the user is supposed to bring the prober's CPU back.
            ImGui::Spacing();
            const bool portPresent =
                std::find(m_ports.begin(), m_ports.end(), r.proberPort) != m_ports.end();
            // Sequencing, not safety: nothing is damaged by touching early, but
            // touching while an RPI-RP2 volume is still mounted puts a second
            // one back on the bus and returns the user to the exact state they
            // are trying to leave.
            const bool volumeStillMounted = !m_volumes.empty();

            ImGui::BeginDisabled(!portPresent || volumeStillMounted);
            if (ImGui::Button(ICON_MD_RESTART_ALT " Return the prober's CPU to BOOTSEL"))
                m_probe.returnProberToBootsel();
            ImGui::EndDisabled();
            if (!portPresent) {
                ImGui::SameLine();
                wrappedColored(kMutedColor,
                    "The prober's port (" + r.proberPort + ") is no longer present. If a CPU "
                    "is still running the prober, use the port picker below.");
            } else if (volumeStillMounted) {
                ImGui::SameLine();
                wrappedColored(kMutedColor,
                    "Available once no RPI-RP2 volume is mounted -- flash " + r.remainingVolume +
                    " first.");
            }
        }
    }

    // --- The way back if any of the above was interrupted ------------------
    //
    // Without this the flow could create a NEW dead end: a CPU left running the
    // prober publishes no FWOG_* product string, so the ordinary flash path
    // cannot identify it and cannot reboot it, and a successful result is
    // otherwise the only thing that knows its port. Closing the app, or
    // cancelling, between the write and the answer would strand it.
    ImGui::Spacing();
    if (ImGui::TreeNode("A CPU is stuck running the prober")) {
        ImGui::TextWrapped(
            "The prober honours the standard 1200-baud touch, so it is never a trap -- but "
            "the app has to be told which port it is on. Pick the prober's port and reboot "
            "it into its bootloader, then flash it as normal. The prober enumerates as "
            "\"FWOG probe 002\" (FreeWili OG) in Device Manager if you need to "
            "check which port that is.");
        ImGui::TextColored(kMutedColor,
            "Only do this to a port you know is the prober. Rebooting an unrelated "
            "RP2040-based device into its bootloader is recoverable, but it is not helpful.");

        if (m_ports.empty()) {
            ImGui::TextColored(kMutedColor, "No serial ports are present.");
        } else {
            if (m_selectedPort < 0 || m_selectedPort >= int(m_ports.size()))
                m_selectedPort = 0;
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("##ProberPort", m_ports[size_t(m_selectedPort)].c_str())) {
                for (int i = 0; i < int(m_ports.size()); ++i) {
                    const bool selected = (i == m_selectedPort);
                    if (ImGui::Selectable(m_ports[size_t(i)].c_str(), selected))
                        m_selectedPort = i;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_MD_RESTART_ALT " Reboot this port into BOOTSEL"))
                touchPort1200(m_ports[size_t(m_selectedPort)]);
        }
        ImGui::TreePop();
    }
}

void TabRecovery::scrollTo(RecoveryAnchor anchor)
{
    m_pendingScroll = anchor;
}

} // namespace fwog
