#include "ui/fwDeviceBar.h"

#include "core/fwTypes.h"          // kDeviceSupportAvailable, platformLimitationNotice
#include "device/fwCpuIdentify.h"   // ogBootloaderState
#include "device/fwDeviceModel.h"

#include <imgui.h>
#include <IconsMaterialDesign.h>

#if defined(__APPLE__) && !TARGET_OS_OSX
  #include "platform/fwVolumeGrant.h"   // the grant button below
#endif

#include <optional>
#include <string>

namespace fwog {
namespace {

constexpr float kBarHeight = 96.0f;
// Extra room for the CPU identification note (see DeviceBar::draw), added only
// when there is one. It is a full sentence that wraps to two lines at ordinary
// widths, and it has to be readable without the bar scrolling -- the case it
// exists for is the user being told something they did not go looking for. A
// permanently taller bar would instead take that space from the tab below it in
// every session where nobody runs the Recovery tab's identification, which is
// most of them.
constexpr float kIdentificationNoteHeight = 44.0f;
/// Extra room for the "no OG bootloader" banner, added only when it is shown --
/// same reasoning as the identification note above: a permanently taller bar
/// would take that space from the tab below it in every session with a board
/// that is perfectly fine.
constexpr float kBootloaderBannerHeight = 26.0f;

const ImVec4 kOgColor      { 0.35f, 0.80f, 0.35f, 1.00f }; // green: safe to flash
const ImVec4 kNotOgColor   { 0.90f, 0.65f, 0.15f, 1.00f }; // amber: shown, not flashable here
const ImVec4 kErrorColor   { 0.95f, 0.35f, 0.35f, 1.00f };
const ImVec4 kMutedColor   { 0.65f, 0.65f, 0.65f, 1.00f };

void drawRow(DeviceModel& model, size_t index, const DeviceView& device, const std::optional<DeviceView>& selected)
{
    ImGui::PushID(static_cast<int>(index));

    // Compared by uniqueID, not by address or index -- selected is a
    // snapshot returned by value (see DeviceModel::selected()'s header
    // comment), so it never aliases `device`.
    const bool isSelected = selected.has_value() && selected->uniqueID == device.uniqueID;
    const std::string label = device.name.empty() ? std::string("(unnamed device)") : device.name;

    // The whole row is the click target -- with several devices connected,
    // this is how the user picks which one every later flash targets.
    if (ImGui::Selectable(label.c_str(), isSelected))
        model.select(index);

    ImGui::SameLine();
    if (device.isOg) {
        ImGui::TextColored(kOgColor, "[OG]");
    } else {
        ImGui::TextColored(kNotOgColor, "[NOT an OG -- not flashable here]");
    }

    ImGui::SameLine();
    // The best name the board gives itself: FTDI serial when it enumerates
    // (legacy firmware), else an RP2040 chip id. "serial Unknown" told the
    // user nothing about a board the app can in fact tell apart from every
    // other -- see BoardFingerprint.
    ImGui::TextColored(kMutedColor, "%s", describeFingerprint(fingerprintOf(device)).c_str());

    // describeIdentity() never omits a CPU -- if one is missing this line
    // says so explicitly rather than staying silent about it.
    ImGui::TextUnformatted(describeIdentity(device.identity).c_str());

    ImGui::PopID();
}

} // namespace

void DeviceBar::draw(DeviceModel& model, const std::string& identificationNote,
                     bool identificationDiscarded, Theme& theme,
                     const std::function<void()>& onInstallBootloader)
{
    // Task 21: on the web build there is nothing to scan for and no scan to
    // report on, so a "(scanning...)"/"(idle)" status and a Rescan button
    // would both be theatre -- and "No FreeWili detected. Connect one over
    // USB." would actively mislead, since no cable can change the answer.
    // Say what is actually true, once, and draw nothing else.
    //
    // `if constexpr`, not `#if`: the desktop body below stays compiled on
    // every platform, so a change to it cannot silently break the web build
    // (and nothing in it goes unreferenced on the web either).
    if constexpr (!kDeviceSupportAvailable) {
        (void)model;
        (void)identificationNote;      // there is no board to identify on the web
        (void)identificationDiscarded;
        ImGui::BeginChild("##DeviceBar", ImVec2(0.0f, kBarHeight), ImGuiChildFlags_Border);
        ImGui::TextUnformatted(ICON_MD_USB " Devices");

        // No Rescan button on the web, but the theme picker still belongs
        // here: this bar is now the only chrome the app has, and dropping the
        // picker from this branch would leave the web build with no way to
        // change theme at all.
        float themeX = ImGui::GetWindowWidth() - themePickerWidth() - ImGui::GetStyle().WindowPadding.x;
        if (themeX < ImGui::GetCursorPosX()) themeX = ImGui::GetCursorPosX();
        ImGui::SameLine(themeX);
        drawThemePicker(theme);

        ImGui::PushStyleColor(ImGuiCol_Text, kMutedColor);
        // TextWrapped, not TextUnformatted: this is a full sentence and the
        // bar is only kBarHeight tall -- an unwrapped line would run off the
        // right edge with the second half unreadable.
        ImGui::TextWrapped("%s", std::string(platformLimitationNotice()).c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
        return;
    }

    // One getDevices() call per frame, done inside refresh() and cached
    // there -- this is the only place that pulls a new snapshot.
    model.refresh();

    // Decided BEFORE the child is sized, because the banner needs room in it.
    // Read from the CONFIRMED selection: this is a claim about a specific board,
    // and making it about a device whose identity could not be confirmed would
    // be telling the user something about hardware nobody has established.
    const std::optional<DeviceView> bannerDevice = model.selected();
    const bool lacksBootloader =
        bannerDevice.has_value() && bannerDevice->isOg
        && ogBootloaderState(bannerDevice->identity) == OgBootloaderState::Missing;

    float barHeight =
        kBarHeight + (identificationNote.empty() ? 0.0f : kIdentificationNoteHeight)
                   + (lacksBootloader ? kBootloaderBannerHeight : 0.0f);
#if defined(__APPLE__) && !TARGET_OS_OSX
    // The grant prompt (two wrapped lines + a button) needs more room than a
    // device row; sized here because the child cannot grow after it begins.
    if (model.devices().empty()) barHeight += 24.0f;
#endif
    ImGui::BeginChild("##DeviceBar", ImVec2(0.0f, barHeight), ImGuiChildFlags_Border);

    ImGui::TextUnformatted(ICON_MD_USB " Devices");
    ImGui::SameLine();
    // "(monitoring)", not the old "(idle)": the scanner no longer stops when
    // its request window expires (see fwFinderManager::run()), it drops to a
    // slower steady-state poll -- so "idle" would now be a lie about the one
    // thing this bar exists to report. "(scanning...)" still means what it
    // always did: a request is hot and the fast poll is running.
    ImGui::TextColored(kMutedColor, model.scanning() ? "(scanning...)" : "(monitoring)");

    // Right-align the theme picker and the rescan button, in that order, within
    // this child window. Both are placed from ONE origin computed from their
    // combined width rather than each being right-aligned independently: the
    // theme button has to land exactly one ItemSpacing left of the rescan
    // button, and aligning them separately would need the second placement to
    // read back where the first one ended up.
    //
    // Floored at the cursor's current position so an extreme (very narrow, or
    // squeezed by a long status string) window width cannot push either widget
    // to a negative SameLine offset -- ImGui treats <=0 specially there, so
    // this is not just cosmetic.
    constexpr const char* kRescanLabel = ICON_MD_REFRESH " Rescan";
    const float spacing     = ImGui::GetStyle().ItemSpacing.x;
    const float rescanWidth = ImGui::CalcTextSize(kRescanLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float groupWidth  = themePickerWidth() + spacing + rescanWidth;
    float themeX = ImGui::GetWindowWidth() - groupWidth - ImGui::GetStyle().WindowPadding.x;
    if (themeX < ImGui::GetCursorPosX()) themeX = ImGui::GetCursorPosX();

    ImGui::SameLine(themeX);
    drawThemePicker(theme);

    ImGui::SameLine();
    if (ImGui::Button(kRescanLabel))
        model.requestRescan();

    if (!model.error().empty())
        ImGui::TextColored(kErrorColor, "Scan error: %s", model.error().c_str());

    // The board has no OG bootloader: its DISPLAY CPU is blank, or is running
    // firmware that is not wiliOGBsp. Either way nothing in the App Explorer
    // will run on it, so this is said in the one place visible from every tab
    // rather than only on the tab where it bites.
    if (lacksBootloader) {
        ImGui::TextColored(kNotOgColor, ICON_MD_WARNING
            " No OG bootloader on this board -- OG apps will not run.");
        if (onInstallBootloader) {
            // Held in the scanner's fast-poll window for as long as the banner
            // is up. The button starts a flash on the device snapshot as it
            // stands, with no pause in which a board could be swapped, so what
            // is left to guarantee is that the snapshot is RECENT -- and this is
            // the only window in which the button can be pressed. Scoped to a
            // banner that only appears on a board missing its bootloader.
            //
            // WHAT IT COSTS, measured rather than reasoned about. This line
            // used to describe its own price as "an atomic store per frame
            // (fwFinderManager::requestRefresh)", which is true of the call and
            // badly understates the consequence: re-arming every frame means
            // the request window never expires, so the scanner never drops back
            // to kIdlePollMs and enumerates at kFastPollMs for as long as the
            // banner is up. On Linux, with a bootloader-less board attached,
            // that worker thread was measured at ~48-50% of one CPU core
            // continuously (~52 ms of CPU per Fw::find_all(), ~6,200 read()
            // syscalls per second against sysfs), against ~3% for the UI thread
            // doing all the rendering.
            //
            // Only that per-scan cost is platform-specific. The MECHANISM is
            // not, and that is knowable by reading rather than by measuring:
            // this re-arm, kFastPollMs and kIdlePollMs are all in shared code
            // with no platform variation in them, so Windows holds the scanner
            // in the same permanent fast poll. Saying "not measured on Windows"
            // here would invite a reader to suspect a Linux quirk, and there
            // is not one.
            //
            // Left as it is, deliberately: the freshness guarantee above is
            // what makes a one-click flash safe, and this is the only state in
            // which the button exists. But the number belongs here, because
            // "an atomic store" invites someone to widen this pattern to a
            // banner that is up all the time, and that would be a very
            // different bill.
            model.requestRescan();

            ImGui::SameLine();
            if (ImGui::SmallButton("Install it")) onInstallBootloader();
            // The erase is stated BEFORE the click, not discovered after it.
            // One click was asked for and one click is what this is, but a
            // button that wipes the MAIN CPU has to say so somewhere the user
            // can see without pressing it.
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Erases the MAIN CPU, then installs the display bootloader.\n"
                                  "Flash an app afterwards to put firmware back on MAIN.");
        }
    }

    const auto& devices = model.devices();
    if (devices.empty()) {
#if defined(__APPLE__) && !TARGET_OS_OSX
        // iPadOS cannot enumerate USB, so "no FreeWili detected" is not a
        // statement this build can ever make -- what it can say is that the
        // one thing flashing here needs, the granted RPI-RP2 folder, has not
        // been granted yet. The button opens the system folder picker; once
        // a folder is granted the scan publishes the synthetic device row
        // (fwDeviceModel.cpp) and this branch stops rendering.
        ImGui::TextWrapped("To flash from this iPad: hold the RED button on the "
                           "board while plugging it in, then grant this app "
                           "access to the RPI-RP2 drive that appears.");
        if (ImGui::Button(ICON_MD_FOLDER_OPEN " Grant access to RPI-RP2..."))
            grant::presentGrantPicker();
#else
        ImGui::TextDisabled("No FreeWili detected. Connect one over USB.");
#endif
    } else {
        // One selected() call per frame, not per row: refresh() and
        // selected() are each O(scan)/O(devices), and this keeps the row
        // loop below from repeating that work per row.
        const std::optional<DeviceView> selected = model.selected();
        for (size_t i = 0; i < devices.size(); ++i)
            drawRow(model, i, devices[i], selected);
#if defined(__APPLE__) && !TARGET_OS_OSX
        // The grant's live state, every frame: its failure modes differ in
        // remedy (plug the board in vs. re-grant) and nothing else on screen
        // can tell them apart. The Re-grant button stays available because a
        // dev-reinstalled app can hold a bookmark that resolves to nothing.
        ImGui::TextColored(kMutedColor, "Drive: %s", grant::statusDescription().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Re-grant...")) grant::presentGrantPicker();
#endif
    }

    // Below the rows, because it is a statement about the drives rather than
    // about a device row. Coloured by whether it still holds: a discarded
    // identification is not merely information, it is a capability the user had
    // a moment ago and no longer has, and it must not read as a footnote.
    if (!identificationNote.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, identificationDiscarded ? kNotOgColor : kOgColor);
        ImGui::TextWrapped(ICON_MD_FINGERPRINT " %s", identificationNote.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
}

} // namespace fwog
