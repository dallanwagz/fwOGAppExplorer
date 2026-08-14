#pragma once

#include "catalog/fwOgAppInfo.h"    // OgAppInfo
#include "catalog/fwUf2Header.h"   // Uf2Info
#include "core/fwTypes.h"
#include "device/fwDeviceModel.h"   // BoardFingerprint

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fwog {

class DeviceModel;
class FlashDialog;
class RemoteCatalog;

/// The App Explorer tab: search and filter the merged catalog on the left, with
/// the selected entry's details and its UF2 header on the right.
///
/// Everything listed here is a FreeWili OG APP under the wiliOGbsp contract --
/// one `<name>_main.uf2` written to the MAIN CPU, carrying its display half
/// inside it (see onlyOgApps(), fwCatalogFilter.h). The display bootloader and
/// the original deprecated firmware are board-level operations with their own
/// cards on the Default Firmware tab and are filtered out before they reach
/// here. That single assumption is why this tab has no scheme picker, no
/// multi-step plan preview and no DISPLAY-CPU retarget: there is exactly one
/// thing an app can do.
///
/// Owns only UI state (search text, category filter, current selection, the
/// cached UF2 header for that selection, and an in-flight Flash click) across
/// frames. The catalog itself is not owned here -- it is passed in fresh every
/// frame by the caller, which built it via mergeCatalogs().
class AppExplorerTab {
public:
    /// Draws the tab body for one frame. `entries` is this frame's merged
    /// catalog (embedded + local + remote), already filtered to OG apps; it is
    /// only read here and never retained past the call, so the "caller must not
    /// mutate while pointers are live" contract on filterEntries() (see
    /// fwCatalogFilter.h) is satisfied simply by this function returning before
    /// the caller's next mutation of its own vector.
    ///
    /// `flashDialog` is shared with DefaultFirmwareTab (owned by App::run()) so
    /// only one flash can ever be in flight across the whole app.
    ///
    /// `remoteCatalog`/`remoteCatalogUrl`: the URL is CONFIGURED on the Settings
    /// tab, not here. This tab only offers the Online Update button that
    /// re-fetches whatever address is stored. Both are owned by App::run() and
    /// outlive every call here.
    void draw(std::span<const CatalogEntry> entries, DeviceModel& deviceModel, FlashDialog& flashDialog,
              RemoteCatalog& remoteCatalog, std::string& remoteCatalogUrl);

    /// Call the frame this tab STOPS being the active tab (see fwApp.cpp's
    /// tab-bar loop). Abandons a Flash click that has been accepted but has not
    /// yet started -- see PendingFlash.
    void onTabHidden();

    /// An entry's UF2 header, read from the image itself and cached for as long
    /// as that entry stays selected.
    ///
    /// `info` empty with a non-empty `note` is the ordinary "cannot show it,
    /// here is why" case: a remote image not yet downloaded, an unreadable file,
    /// or an image parseUf2() rejects outright. Both empty means nothing is
    /// selected.
    struct Uf2Summary {
        std::string            slug;   ///< which entry this describes
        std::optional<Uf2Info> info;
        std::string            note;
        /// The fwog_uf2_info_t records the image carries -- normally two for a
        /// main UF2 (its own and the display image inside it). Empty for a UF2
        /// that is not an OG app, which is not an error.
        std::vector<OgAppInfo> ogInfo;
    };

    /// A Flash click that has been accepted but not yet started, because the
    /// device snapshot behind it is not recent enough to prove the board is
    /// still the one that was clicked on.
    ///
    /// This exists so that ONE click is genuinely one click. The modal used to
    /// get its freshness by holding the scanner in its fast-poll window for as
    /// long as its Start button was on screen; a button that starts immediately
    /// has no such window, and keeping the scanner fast the entire time a user
    /// merely BROWSES a flashable app would enumerate USB continuously for a
    /// click that may never come. So the rescan is requested when the click
    /// happens, and the flash starts on the first frame the snapshot is fresh --
    /// typically within a few hundred milliseconds, with no second click.
    ///
    /// The board identity is captured at click time and re-compared every frame
    /// of the wait: the wait is short, but it is not zero, and a swap inside it
    /// must abort the flash rather than redirect it at whatever is now plugged
    /// in. That is the same rule the modal applies across its human pause.
    struct PendingFlash {
        /// Carried by value rather than by slug, so that what gets flashed is
        /// exactly what was on screen when the click happened, even if the
        /// catalog is rebuilt (it is, every frame) in between.
        CatalogEntry     entry;
        uint64_t         uniqueID = 0;
        BoardFingerprint print;
        int              waitedFrames = 0;
    };

private:
    /// Advances a PendingFlash by one frame: requests the rescan, re-checks the
    /// board, and either starts the flash or abandons it with a reason. Called
    /// once per frame from draw(), AFTER the panes, so the button and the state
    /// it reflects belong to the same frame.
    void servicePendingFlash(DeviceModel& deviceModel, FlashDialog& flashDialog);

    std::string m_search;
    std::string m_category;       ///< empty == "All"
    std::string m_selectedSlug;   ///< empty == nothing selected
    /// Compact (phone) layout only: the stack position -- false shows the
    /// full-width list, true shows the detail screen with a back button.
    bool m_compactShowDetail = false;

    /// Recomputed only when the selection changes -- see loadUf2Summary(), which
    /// reads the whole image and walks every block of it.
    Uf2Summary m_uf2Summary;

    std::optional<PendingFlash> m_pendingFlash;
    /// Why the last pending flash was abandoned, shown beside the button until
    /// the next click. Empty when there is nothing to report.
    std::string m_flashStartError;

    /// Set by a double-clicked row, consumed by the detail pane on the SAME
    /// frame and cleared unconditionally at the end of draw() whether or not
    /// anything acted on it. It never survives a frame, so a request that could
    /// not be honoured cannot fire later against a different selection.
    std::string m_flashRequestSlug;
};

} // namespace fwog
