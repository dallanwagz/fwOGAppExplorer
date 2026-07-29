#include "ui/fwDeviceBar.h"

#include "core/fwTypes.h"          // kDeviceSupportAvailable, platformLimitationNotice
#include "device/fwCpuIdentify.h"   // ogBootloaderState
#include "device/fwDeviceModel.h"

#include <imgui.h>
#include <IconsMaterialDesign.h>

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
    ImGui::TextColored(kMutedColor, "serial %s", device.serial.empty() ? "(none)" : device.serial.c_str());

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

    const float barHeight =
        kBarHeight + (identificationNote.empty() ? 0.0f : kIdentificationNoteHeight)
                   + (lacksBootloader ? kBootloaderBannerHeight : 0.0f);
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
            // the only window in which the button can be pressed. Costs an
            // atomic store per frame (fwFinderManager::requestRefresh) and is
            // scoped to a banner that only appears on a board missing its
            // bootloader.
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
        ImGui::TextDisabled("No FreeWili detected. Connect one over USB.");
    } else {
        // One selected() call per frame, not per row: refresh() and
        // selected() are each O(scan)/O(devices), and this keeps the row
        // loop below from repeating that work per row.
        const std::optional<DeviceView> selected = model.selected();
        for (size_t i = 0; i < devices.size(); ++i)
            drawRow(model, i, devices[i], selected);
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
