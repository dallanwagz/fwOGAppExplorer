#include "ui/fwTabAppExplorer.h"
#include "ui/fwUiScale.h"

#include "catalog/fwCatalogEmbedded.h"   // loadEmbeddedImage
#include "catalog/fwCatalogFilter.h"
#include "catalog/fwCatalogStub.h"
#include "catalog/fwOgAppInfo.h"
#include "catalog/fwUf2Header.h"
#include "catalog/fwCatalogRemote.h"
#include "core/fwSettingsIo.h"
#include "device/fwDeviceModel.h"
#include "platform/fwPaths.h"   // catalogDir
#include "flash/fwFlashController.h"   // selectionUnchangedFresh, selectionCheckMessage
#include "flash/fwFlashPlan.h"
#include "ui/fwFlashDialog.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <IconsMaterialDesign.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

namespace fwog {
namespace {

const ImVec4 kMutedColor    { 0.65f, 0.65f, 0.65f, 1.00f };
const ImVec4 kWarnColor     { 0.90f, 0.65f, 0.15f, 1.00f };
const ImVec4 kErrorColor    { 0.95f, 0.35f, 0.35f, 1.00f };
const ImVec4 kOkColor       { 0.35f, 0.80f, 0.35f, 1.00f };
const ImVec4 kAccentColor   { 0.30f, 0.55f, 0.95f, 1.00f };

const char* sourceLabel(CatalogSource s)
{
    switch (s) {
    case CatalogSource::Embedded: return "Embedded";
    case CatalogSource::Local:    return "Local";
    case CatalogSource::Remote:   return "Remote";
    case CatalogSource::Unlisted: return "Unlisted";
    }
    return "?";
}

ImVec4 sourceColor(CatalogSource s)
{
    switch (s) {
    case CatalogSource::Embedded: return kOkColor;
    case CatalogSource::Local:    return kAccentColor;
    case CatalogSource::Remote:   return kMutedColor;
    case CatalogSource::Unlisted: return kWarnColor;
    }
    return kMutedColor;
}

/// Which of ImageRef's three mutually-exclusive fields is populated, in a
/// short form suitable for one line of the plan list.
std::string imageRefSummary(const ImageRef& ref)
{
    if (!ref.embeddedId.empty()) return "embedded:" + ref.embeddedId;
    if (!ref.localPath.empty())  return ref.localPath;
    if (!ref.url.empty())        return ref.url;
    return "(no image reference)";
}

/// TextColored's non-wrapping cousin: catalog text (names, authors, tags,
/// URLs, descriptions) is free text of unbounded length, and a line that
/// merely overflows past the pane's right edge -- rather than wrapping --
/// silently hides whatever ran off it. Every free-text line in this tab
/// goes through this, not ImGui::TextColored.
void wrappedColored(const ImVec4& color, const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

/// Fills a fixed ImGui text buffer from a std::string, truncating rather than
/// overrunning. strncpy_s is not merely MSVC's preference here: at /W4 plain
/// strncpy is a hard error (C4996) in this build.
void copyToTextBuffer(char* dst, size_t dstLen, const std::string& src)
{
#if defined(_MSC_VER)
    strncpy_s(dst, dstLen, src.c_str(), _TRUNCATE);
#else
    std::strncpy(dst, src.c_str(), dstLen - 1);
    dst[dstLen - 1] = '\0';
#endif
}

/// An hourglass that turns over, for "a flash is running".
///
/// Three frames from the Material Icons set -- full, then draining, then empty
/// -- so it reads as time passing rather than as a decoration that happens to
/// move. Driven by ImGui's own clock rather than a frame counter, so it turns at
/// the same human-readable rate whatever the frame rate is.
const char* flashHourglass()
{
    static const char* kFrames[] = {
        ICON_MD_HOURGLASS_TOP, ICON_MD_HOURGLASS_FULL, ICON_MD_HOURGLASS_BOTTOM
    };
    return kFrames[int(ImGui::GetTime() * 2.0) % 3];
}

const CatalogEntry* findBySlug(std::span<const CatalogEntry> entries, const std::string& slug)
{
    if (slug.empty()) return nullptr;
    for (const auto& e : entries)
        if (e.slug == slug) return &e;
    return nullptr;
}

/// Read an entry's image and parse its UF2 header, for the detail pane's
/// "UF2 image" block.
///
/// Called once per selection change, never per frame -- it reads the ENTIRE
/// image and parseUf2() walks every 512-byte block of it.
///
/// A remote entry whose image is a URL is reported, not fetched. Downloading
/// megabytes because a user clicked a row would turn browsing into traffic, and
/// the flash itself is what legitimately triggers the download.
AppExplorerTab::Uf2Summary loadUf2Summary(const CatalogEntry& entry)
{
    AppExplorerTab::Uf2Summary out;
    out.slug = entry.slug;

    if (entry.uf2.empty()) {
        out.note = "This entry has no UF2 image, so there is nothing to flash.";
        return out;
    }

    // front(), not a search: an OgApp entry is one image by definition (see
    // onlyOgApps, fwCatalogFilter.h), and this pane is only ever handed those.
    const ImageRef& ref = entry.uf2.front().ref;

    std::vector<uint8_t> bytes;
    if (!ref.embeddedId.empty()) {
        auto loaded = loadEmbeddedImage(ref.embeddedId);
        if (!loaded) {
            out.note = "Could not read the embedded image: " + loaded.error();
            return out;
        }
        bytes = std::move(*loaded);
    } else if (!ref.localPath.empty()) {
        std::ifstream in(std::filesystem::path(ref.localPath), std::ios::binary);
        if (!in) {
            out.note = "Could not open " + ref.localPath + ".";
            return out;
        }
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else if (!ref.url.empty()) {
        out.note = "This image lives at " + ref.url + " and has not been downloaded, so its "
                   "header cannot be read yet. Flashing downloads it first.";
        return out;
    } else {
        out.note = "This entry names no image.";
        return out;
    }

    auto info = parseUf2(std::span<const uint8_t>(bytes.data(), bytes.size()));
    if (!info) {
        // A refusal, not a footnote: parseUf2 rejects exactly what must never
        // reach a board, and the Flash button below is about to offer to write
        // this file. Say why it is not a usable image.
        out.note = "This file is not a usable UF2: " + uf2ErrorMessage(info.error()) + ".";
        return out;
    }
    out.info = *info;
    // The contract's own metadata, read from the same bytes: a main UF2 carries
    // one record for itself and one for the display image inside it, so this is
    // where "what will this file put on BOTH CPUs" comes from -- the file, not
    // the catalog and not the filename.
    out.ogInfo = findOgAppInfo(flattenUf2Payload(bytes));
    return out;
}

/// The category chips, with Online Update sitting at the end of the same row.
///
/// The fetch button lives HERE, beside the filters, rather than in Settings
/// with the address it fetches: typing a URL is configuration done once,
/// pressing "get me the current list" is something you do while looking at the
/// list. Splitting them put each half where it is actually used.
void drawCategoryChips(const std::vector<std::string>& categories, std::string& selected,
                       RemoteCatalog& remoteCatalog, const std::string& remoteCatalogUrl,
                       std::size_t shown, std::size_t total)
{
    // Tall enough for one row of buttons AND the horizontal scrollbar beneath
    // them. The scrollbar is drawn INSIDE the child's height, so a child sized
    // to exactly one row loses the bottom of every button the moment the
    // content overflows -- which it now always does on a narrow pane, since
    // Online Update joined the chips.
    const float chipsHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ScrollbarSize;
    ImGui::BeginChild("##CategoryChips", ImVec2(0, chipsHeight),
                       ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

    auto chip = [&](const char* label, bool active, const std::string& value) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, kAccentColor);
        if (ImGui::Button(label)) selected = value;
        if (active) ImGui::PopStyleColor();
        ImGui::SameLine();
    };

    chip("All", selected.empty(), "");
    for (const auto& c : categories)
        chip(c.c_str(), selected == c, c);

    // Read BEFORE the button, because clicking it can start a fetch and flip
    // this to true within the same frame. The button's enabled state must
    // reflect what was true when it was drawn.
    const bool fetching  = remoteCatalog.fetching();
    const bool configured = !remoteCatalogUrl.empty();

    ImGui::BeginDisabled(fetching || !configured);
    if (ImGui::Button(ICON_MD_CLOUD_DOWNLOAD " Online Update") && configured && !fetching)
        remoteCatalog.start(remoteCatalogUrl);
    ImGui::EndDisabled();

    // The explanation is a TOOLTIP rather than a line of text under the row.
    //
    // ImGuiHoveredFlags_AllowWhenDisabled is what makes that work at all:
    // BeginDisabled() suppresses hover reporting by default, so without it the
    // tooltip would appear only in the one state that does not need it.
    //
    // Worth naming the trade this makes, since it runs against the rule the
    // rest of this app follows -- never a disabled control whose reason is only
    // discoverable by hovering. It is deliberate here and narrow: this button
    // is an optional convenience on top of a catalog that already works from
    // built-in and local apps, so a user who never finds the reason has lost
    // nothing they were relying on. The same treatment would not be acceptable
    // on the Flash button, which still carries its reason in visible text.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        // Built as a named string rather than inline in the call: the
        // "Re-fetch <url>" arm is a temporary, and relying on a temporary's
        // c_str() surviving to the end of the full expression is the kind of
        // thing that stays correct only until someone splits the line.
        const std::string tip =
            fetching   ? std::string("Fetching the catalog now.")
          : configured ? "Re-fetch " + remoteCatalogUrl + "."
                       : std::string("No catalog URL is set. Add one on the Settings tab to "
                                     "enable Online Update.");
        ImGui::SetTooltip("%s", tip.c_str());
    }

    // The catalog folder, one click away. A user who has just been told "drop a
    // .uf2 in the catalog folder" should not then have to work out where that
    // is; the app knows, so it opens it.
    ImGui::SameLine();
    if (ImGui::Button(ICON_MD_FOLDER_OPEN "##OpenCatalog")) {
        std::error_code ec;
        std::filesystem::create_directories(catalogDir(), ec);   // may not exist yet
        // file:// URL rather than a shell call: SDL_OpenURL is the one portable
        // opener this app already uses, and generic_string() gives it forward
        // slashes on Windows, which is what a file URL requires.
        SDL_OpenURL(("file:///" + catalogDir().generic_string()).c_str());
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Open the catalog folder (%s)", catalogDir().string().c_str());

    // The count moved here from its own line below the row. It is one short
    // phrase and it belongs beside the filters that determine it -- and putting
    // it here gives the app list back the two lines that the count and the
    // Online Update explanation used to occupy.
    ImGui::SameLine();
    ImGui::TextColored(kMutedColor, "%zu of %zu apps", shown, total);

    ImGui::EndChild();
}

/// `flashRequestSlug` is set when a row is DOUBLE-clicked: the shortcut for
/// "select this and flash it".
///
/// It only records the request. Nothing here decides whether that flash may
/// happen -- drawDetail() does, on this same frame, by running the request
/// through the identical checks the Flash button is subject to
/// (flashDisabledReason, and the busy state). A
/// double-click is a faster way to press that button, never a way around it.
void drawEntryList(std::span<const CatalogEntry* const> filtered, std::string& selectedSlug,
                   std::string& flashRequestSlug, bool& rowActivated)
{
    ImGui::BeginChild("##AppExplorerList", ImVec2(0, 0), ImGuiChildFlags_Border);
    for (const CatalogEntry* e : filtered) {
        ImGui::PushID(e->slug.c_str());
        const bool isSelected = (e->slug == selectedSlug);
        if (ImGui::Selectable(e->name.empty() ? "(unnamed)" : e->name.c_str(), isSelected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            selectedSlug = e->slug;
            rowActivated = true;
            // Selection happens on either click; the flash request only on the
            // second. IsMouseDoubleClicked is checked rather than
            // IsItemHovered+double, because Selectable has already told us it
            // was activated -- this only distinguishes which gesture did it.
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                flashRequestSlug = e->slug;
        }
        ImGui::SameLine();
        ImGui::TextColored(sourceColor(e->source), "[%s]", sourceLabel(e->source));
        if (!e->category.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kMutedColor, "%s", e->category.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void drawDetail(const CatalogEntry& entry, DeviceModel& deviceModel, FlashDialog& flashDialog,
                 const AppExplorerTab::Uf2Summary& uf2,
                 std::optional<AppExplorerTab::PendingFlash>& pending, std::string& startError,
                 std::string& flashRequestSlug)
{
    ImGui::TextColored(sourceColor(entry.source), "[%s]", sourceLabel(entry.source));
    ImGui::SameLine();
    ImGui::TextUnformatted(entry.name.empty() ? "(unnamed)" : entry.name.c_str());

    if (!entry.tagline.empty())
        wrappedColored(kMutedColor, entry.tagline);

    // Not "v%s": entry.version is catalog-authored free text, not always a
    // bare semver number -- an embedded entry's version can be a git
    // describe string like "826de9a-dirty", where a forced "v" prefix reads
    // wrong ("v826de9a-dirty"). Render it exactly as the catalog gave it.
    if (!entry.version.empty()) { ImGui::SameLine(); ImGui::TextColored(kMutedColor, " %s", entry.version.c_str()); }
    if (!entry.author.empty())  ImGui::Text("Author: %s", entry.author.c_str());
    if (!entry.category.empty()) { ImGui::SameLine(); ImGui::TextColored(kMutedColor, " -- %s", entry.category.c_str()); }

    if (!entry.tags.empty()) {
        std::string joined = "Tags: ";
        for (size_t i = 0; i < entry.tags.size(); ++i) {
            if (i) joined += ", ";
            joined += entry.tags[i];
        }
        wrappedColored(kMutedColor, joined);
    }

    if (!entry.github.empty()) {
        if (ImGui::Button(ICON_MD_OPEN_IN_NEW " Open on GitHub"))
            SDL_OpenURL(entry.github.c_str());
        ImGui::SameLine();
        wrappedColored(kMutedColor, entry.github);
    }

    if (!entry.description.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", entry.description.c_str());
    }

    // --- UF2 information -------------------------------------------------
    //
    // What this pane shows, and what it deliberately no longer shows.
    //
    // Under the wiliOGbsp contract every entry that reaches this list is one
    // FreeWili OG app: a `<name>_main.uf2` written to the MAIN CPU, carrying
    // its display half inside it. There is therefore no scheme to choose, no
    // CPU to pick and no multi-step plan to preview -- a "Flash plan" that
    // always reads "one UF2 -> MAIN" is a section that never says anything.
    // It, the scheme line and the DISPLAY-retarget control are all gone: the
    // retarget existed to point a main image at the DISPLAY CPU, which is the
    // one operation the contract names outright as never to perform ("Never fw
    // flash a display application. It will not boot and it takes the display
    // CPU off USB").
    //
    // In its place is what is actually specific to the file in front of you:
    // the image's own UF2 header.
    // --- What the image says about itself ---------------------------------
    //
    // The wiliOGbsp contract's own metadata, read out of the file: every OG
    // image carries an fwog_uf2_info_t record, and a MAIN UF2 carries TWO --
    // its own and one for the display image embedded inside it. So a single
    // file can state what it will put on BOTH CPUs, which is exactly what a
    // user choosing what to flash needs and what a filename cannot tell them.
    if (!uf2.ogInfo.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted(ICON_MD_INFO " OG app record");

        const auto showRecord = [](const OgAppInfo& r) {
            ImGui::TextColored(kAccentColor, "%s", r.cpu == TargetCpu::Main ? "MAIN" : "DISPLAY");
            ImGui::SameLine();
            ImGui::Text("%s %s", r.name.c_str(), formatOgAppVersion(r.version).c_str());
            ImGui::Indent();
            if (!r.description.empty()) wrappedColored(kMutedColor, r.description);
            // A display record carries no build identity by design -- the
            // bootloader compares crc32 to decide whether to transfer it -- so
            // its absence is normal and is not reported as missing.
            if (!r.build.empty()) wrappedColored(kMutedColor, "Build: " + r.build);
            char crc[32];
            std::snprintf(crc, sizeof(crc), "crc32 %08X", r.crc32);
            wrappedColored(kMutedColor, crc);
            ImGui::Unindent();
        };

        // MAIN first: it is the CPU this file is written to, and the display
        // record describes a passenger.
        if (const auto m = ogAppInfoFor(uf2.ogInfo, TargetCpu::Main))    showRecord(*m);
        if (const auto d = ogAppInfoFor(uf2.ogInfo, TargetCpu::Display)) showRecord(*d);
    } else if (uf2.info) {
        ImGui::Separator();
        // Said plainly rather than left blank: a UF2 with no record is a
        // perfectly valid Pico image, but it is not an OG app, and under this
        // contract that is worth knowing before flashing it.
        wrappedColored(kWarnColor, ICON_MD_WARNING " This image carries no FreeWili OG app record, "
                                    "so nothing but its raw UF2 header is known about it.");
    }

    ImGui::Separator();
    ImGui::TextUnformatted(ICON_MD_MEMORY " UF2 image");

    if (uf2.info) {
        // Two decimals of KiB rather than raw bytes: payloadBytes runs to the
        // megabytes for a real image, and a bare digit string that long is
        // read wrong more often than it is read.
        const double kib = double(uf2.info->payloadBytes) / 1024.0;
        ImGui::Text("Blocks: %u", uf2.info->numBlocks);
        ImGui::Text("Payload: %llu bytes (%.1f KiB)",
                    (unsigned long long)uf2.info->payloadBytes, kib);
        ImGui::Text("Target address: 0x%08X", uf2.info->targetAddr);
        ImGui::Text("Family: %s", uf2.info->familyPresent
                                    ? "RP2040"
                                    : "no family ID present in this image");
    }
    if (!uf2.note.empty())
        wrappedColored(uf2.info ? kMutedColor : kWarnColor, uf2.note);

    if (!entry.uf2.empty()) {
        wrappedColored(kMutedColor, "Image: " + imageRefSummary(entry.uf2.front().ref));
        if (!entry.uf2.front().sha256.empty())
            wrappedColored(kMutedColor, "sha256 " + entry.uf2.front().sha256);
    }

    // An Unlisted entry is a loose .uf2 nobody has described. This writes the
    // catalog.json entry for it -- seeded from the image's own record where it
    // has one -- so the fields only a human can supply (author, tags, category)
    // are there to fill in rather than to invent from scratch.
    //
    // Offered ONLY for Unlisted: a described entry already has one, and a
    // second would make it ambiguous which wins. The write refuses in that case
    // too (see addBlankCatalogEntry), so this is convenience, not the guard.
    if (entry.source == CatalogSource::Unlisted && !entry.uf2.empty()
        && !entry.uf2.front().ref.localPath.empty()) {
        ImGui::Spacing();
        if (ImGui::Button(ICON_MD_NOTE_ADD " Add catalog entry")) {
            const std::filesystem::path image(entry.uf2.front().ref.localPath);
            const auto written = addBlankCatalogEntry(image.parent_path(),
                                                      image.filename().string(), uf2.ogInfo);
            // Both outcomes are reported. A file written silently is a file the
            // user does not know to go and edit.
            startError = written ? "Wrote a blank entry to " + written->string() +
                                   " -- edit it to fill in the author, tags and category."
                                 : "Could not add a catalog entry: " + written.error();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Create a catalog.json entry for this file, so it can carry a "
                              "name, author, tags and a category.");
    }

    // The entry exactly as catalogued. It used to be a pointer that could aim
    // at a DISPLAY-retargeted copy; with the retarget gone there is only ever
    // the one entry, and the name is kept solely so the calls below read the
    // same as their counterparts on the Default Firmware tab.
    const CatalogEntry* planEntry = &entry;

    // --- Flash button. The disabled reason is ALWAYS shown next to the
    // button when present -- never only in a tooltip, since a disabled
    // control with no visible explanation is what users file bugs about.
    ImGui::Separator();
    const std::optional<DeviceView> selectedDevice = deviceModel.selected();
    // A device whose identity could not be CONFIRMED (see selected()'s
    // comment -- most often a serial reading fwfinder's "Unknown" sentinel)
    // must not read as "nothing connected": selectedByIdOnly() hands back
    // the raw candidate for WORDING the refusal only. The Flash click below
    // still gates on selectedDevice (the confirmed one), never on this raw
    // one. See flashDisabledReason's header comment (fwCatalogFilter.h).
    const std::optional<DeviceView> rawDevice =
        selectedDevice.has_value() ? selectedDevice : deviceModel.selectedByIdOnly();
    const DeviceView* devPtr = rawDevice.has_value() ? &(*rawDevice) : nullptr;
    const std::string reason = flashDisabledReason(*planEntry, devPtr, selectedDevice.has_value());

    // Also disabled while the shared dialog is already open for a flash
    // started from either tab -- the modal blocks input to this button on
    // its own, but this keeps the visible state honest even so, and means
    // there is a single reason (flashDialog.isOpen()) for the "no-op"
    // instead of relying only on the modal's own input block. isInlineBusy()
    // covers the same ground for a one-click flash, which has no modal to
    // block anything.
    const bool flashBusy = flashDialog.isOpen() || flashDialog.isInlineBusy()
                           || pending.has_value();

    // A double-click on this entry's row, consumed here so it goes through
    // EVERY check the button below is subject to and none of its own. Taken
    // (and cleared) unconditionally, even when it cannot be honoured: a request
    // left sitting would fire the moment the obstacle cleared, which is not
    // what the user asked for two clicks ago.
    bool doubleClicked = false;
    if (flashRequestSlug == entry.slug) {
        flashRequestSlug.clear();
        doubleClicked = true;
    }
    // Refused rather than silently dropped, and worded as its own sentence: the
    // button carries `reason` beside it, but a double-click that does nothing
    // has no obviously-disabled control next to it to explain the silence.
    if (doubleClicked && !reason.empty()) {
        startError = reason;
        doubleClicked = false;
    }
    if (doubleClicked && flashBusy) doubleClicked = false;

    ImGui::BeginDisabled(!reason.empty() || flashBusy);
    const bool pressed = ImGui::Button(ICON_MD_BOLT " Flash");
    if ((pressed || doubleClicked) && selectedDevice.has_value()) {
        // ONE CLICK. The plan, the dropped-erase note and every planWarnings()
        // line are already on screen directly above this button -- that is the
        // review, and the modal's first page was a second copy of it. What the
        // click starts is a short automatic wait for a fresh scan, not another
        // decision for the user; see AppExplorerTab::PendingFlash.
        startError.clear();
        pending = AppExplorerTab::PendingFlash{ *planEntry, selectedDevice->uniqueID,
                                                selectedDevice->serial, 0 };
    }
    ImGui::EndDisabled();

    // The bar sits BESIDE the button, replacing it as the thing to look at
    // while a flash runs. Only for an inline run: a modal-started flash draws
    // its own, and two bars for one flash would be two answers to "how far".
    if (flashDialog.isInlineBusy()) {
        // The hourglass leads, before the bar, and runs for the WHOLE flash --
        // not only during the bounded waits the old spinner was limited to.
        // "Is it doing anything?" is a question the user has from the moment
        // they click, and the bar alone answers it badly: it advances in jumps
        // between steps and then sits still, by design, for as long as a wait
        // lasts (a wait's end is not predictable, so moving it would claim
        // progress that has not happened). The hourglass never stops turning
        // while a flash is live, which is the thing actually being asked.
        ImGui::SameLine();
        ImGui::TextColored(kAccentColor, "%s", flashHourglass());

        ImGui::SameLine();
        ImGui::ProgressBar(flashDialog.progressFraction(), ImVec2(220.0f, 0.0f));
    } else if (pending.has_value()) {
        ImGui::SameLine();
        ImGui::TextColored(kMutedColor, "checking the board...");
    }

    if (!reason.empty()) {
        ImGui::SameLine();
        // wrappedColored, not TextColored: Task 21's platform notice is a
        // full sentence, far longer than any of the pre-existing reasons,
        // and the same rule this file already applies to every other piece
        // of unbounded text applies to it -- a line that overflows the
        // pane's right edge silently hides whatever ran off it.
        wrappedColored(kErrorColor, reason);
    }

    // The phase readout goes UNDER the bar rather than beside it: it is a full
    // sentence ("step 1 of 2 -- waiting for the RPI-RP2 volume") and would push
    // the bar off the pane if it shared the line.
    if (flashDialog.isInlineBusy()) {
        if (const std::string line = flashDialog.progressLine(); !line.empty())
            wrappedColored(kMutedColor, line);
    }

    if (!startError.empty())
        wrappedColored(kErrorColor, startError);
}

} // namespace

// The catalog this tab browses is three sources merged together, and until
// now only two of them could ever be reached from inside the app: the remote
// one was fetched only when settings.ini already contained a URL, and nothing
// in the UI could put one there. This control is that missing half.
//
// Deliberately NOT gated on kDeviceSupportAvailable. Everything else that is
// hidden or disabled on the web build is hidden because a browser physically
// cannot do it -- reach USB mass storage, open a serial port. Fetching a
// catalog is the one thing a browser is unambiguously good at (fwHttp.cpp has
// an emscripten_fetch path for exactly this), so a web user who cannot flash
// anything can still browse the store's catalog, and taking this control away
// there would remove the only feature that build has left.
void AppExplorerTab::draw(std::span<const CatalogEntry> entries, DeviceModel& deviceModel, FlashDialog& flashDialog,
                          RemoteCatalog& remoteCatalog, std::string& remoteCatalogUrl)
{
    // The remote catalog URL field used to sit full width above both panes.
    // It is configuration typed once, so it moved to the Settings tab; only the
    // fetch action stayed, as Online Update beside the filter chips below.
    const auto categories = categoriesOf(entries);

    // Phone-narrow: the two panes become a two-screen stack -- the list full
    // width, and tapping a row navigates to the detail with a back button.
    // Same components, same state; only the arrangement changes, so nothing
    // behavioral (selection rules, flash gating) forks per layout.
    const bool compact = g_uiCompact;
    bool rowActivated = false;

    if (compact && m_compactShowDetail) {
        if (ImGui::Button(ICON_MD_ARROW_BACK " All apps")) {
            m_compactShowDetail = false;
            // Nothing consumed a pending double-click request; drop it rather
            // than let it fire from the list screen next frame.
            m_flashRequestSlug.clear();
        }
    }

    const bool showList = !compact || !m_compactShowDetail;
    if (showList) {
        const float leftWidth = compact ? 0.0f : ImGui::GetContentRegionAvail().x * 0.38f;
        ImGui::BeginChild("##AppExplorerLeft", ImVec2(leftWidth, 0), ImGuiChildFlags_Border);

        char searchBuf[128];
        copyToTextBuffer(searchBuf, sizeof(searchBuf), m_search);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##Search", ICON_MD_SEARCH " Search apps, tags, authors...",
                                      searchBuf, sizeof(searchBuf)))
            m_search = searchBuf;

        // Filtered BEFORE the chips row is drawn, because the row now shows the
        // count as well as the filters that produce it.
        const auto filtered = filterEntries(entries, m_search, m_category);
        drawCategoryChips(categories, m_category, remoteCatalog, remoteCatalogUrl,
                          filtered.size(), entries.size());

        drawEntryList(filtered, m_selectedSlug, m_flashRequestSlug, rowActivated);

        ImGui::EndChild();
    }

    if (compact) {
        if (rowActivated) {
            // Navigate NEXT frame: the list child already spent this frame's
            // full height, and a zero-height detail pane drawn under it would
            // flicker for one frame.
            m_compactShowDetail = true;
            servicePendingFlash(deviceModel, flashDialog);
            return;
        }
        if (!m_compactShowDetail) {
            // List screen: the request (if any) is carried to the detail
            // screen next frame, where the usual gating consumes it.
            servicePendingFlash(deviceModel, flashDialog);
            return;
        }
    } else {
        ImGui::SameLine();
    }

    ImGui::BeginChild("##AppExplorerRight", ImVec2(0, 0), ImGuiChildFlags_Border);

    // NOTE: looked up by slug across the FULL catalog, not just `filtered`
    // -- a search or category change must not blank the detail pane out
    // from under a selection that is still valid, only present.
    const CatalogEntry* selected = findBySlug(entries, m_selectedSlug);
    if (!selected) {
        ImGui::TextColored(kMutedColor, "Select an app from the list on the left.");
    } else {
        // Parsed ONCE per selection, never per frame. loadUf2Summary() reads
        // the whole image and walks every 512-byte block of it; a real image
        // runs to megabytes, so doing this at frame rate would be the same
        // mistake LocalCatalogCache exists to undo (see fwCatalogLocal.h).
        if (m_uf2Summary.slug != selected->slug)
            m_uf2Summary = loadUf2Summary(*selected);

        drawDetail(*selected, deviceModel, flashDialog, m_uf2Summary,
                   m_pendingFlash, m_flashStartError, m_flashRequestSlug);
    }
    // A request for an entry the detail pane did not draw this frame -- the
    // list was filtered out from under it, or the double-click landed on a row
    // whose slug is not the selection for some other reason. Nothing consumed
    // it, so it must not survive into a later frame where it would fire against
    // whatever is selected by then.
    m_flashRequestSlug.clear();

    ImGui::EndChild();

    servicePendingFlash(deviceModel, flashDialog);
}

void AppExplorerTab::servicePendingFlash(DeviceModel& deviceModel, FlashDialog& flashDialog)
{
    if (!m_pendingFlash) return;

    // Same call, same reason as the modal's: it holds the background scanner in
    // its fast-poll window so the freshness requirement below is satisfiable at
    // all. Scoped to the handful of frames this wait lasts rather than to the
    // whole time a flashable entry is on screen.
    deviceModel.requestRescan();

    const std::optional<DeviceView> current = deviceModel.selected();
    const SelectionCheck check = selectionUnchangedFresh(current, m_pendingFlash->uniqueID,
                                                         m_pendingFlash->serial,
                                                         deviceModel.snapshotAge());
    if (check == SelectionCheck::Unchanged && current.has_value()) {
        // Started against the device as re-read THIS frame, never against the
        // one captured at click time -- the capture is only what the check
        // compares against, exactly as m_openedUniqueID is for the modal.
        flashDialog.beginImmediate(m_pendingFlash->entry, *current);
        m_pendingFlash.reset();
        return;
    }

    // StaleSnapshot is the expected answer for the first few frames -- it means
    // "nobody has looked recently enough to know", and the rescan just requested
    // is what fixes it. Every OTHER refusal is a positive statement that the
    // board changed, and waiting cannot improve it: abort at once and say so.
    if (check != SelectionCheck::StaleSnapshot) {
        m_flashStartError = selectionCheckMessage(check) +
                            " Nothing was flashed. Check the board and click Flash again.";
        m_pendingFlash.reset();
        return;
    }

    // A bound, so a scanner that has stopped entirely cannot leave the button
    // showing "checking the board..." for ever. Generous on purpose: a scan
    // lands roughly every 260 ms in the fast window, so this is many scans'
    // worth of grace and only expires when they really are not arriving.
    constexpr int kMaxWaitFrames = 240;
    if (++m_pendingFlash->waitedFrames > kMaxWaitFrames) {
        m_flashStartError =
            "Could not confirm the board is still connected (no recent scan completed), "
            "so nothing was flashed. Try Rescan in the device bar, then click Flash again.";
        m_pendingFlash.reset();
    }
}

void AppExplorerTab::onTabHidden()
{
    // The DISPLAY-retarget control this used to disarm is gone entirely: under
    // the wiliOGbsp contract every listed entry is a MAIN-CPU app image, and
    // pointing one at the DISPLAY CPU is the operation the contract names as
    // never to perform. There is no confirmation left here that could go stale
    // across a visit.
    //
    // A click that has not started its flash yet is abandoned with the visit.
    // servicePendingFlash() only runs while this tab is drawn, so a pending
    // start would otherwise sit frozen until the user happened to come back and
    // then begin writing to a board from a tab they are no longer looking at.
    // A flash must start in front of the person who asked for it.
    //
    // Nothing in flight is touched: once beginImmediate() has been called the
    // pending state is already gone, and a running flash is FlashController's
    // to finish (and the modal's to report on). This only discards intent that
    // has not yet become an action.
    m_pendingFlash.reset();
    m_flashStartError.clear();
}

} // namespace fwog
