#pragma once

#include "ui/fwTheme.h"

#include <functional>
#include <string>

namespace fwog {

class DeviceModel;

/// The persistent header shown above every tab: what is connected, which CPU
/// is which, and how each was identified. Deliberately not tucked inside one
/// tab -- the most dangerous thing this app can do is flash the wrong CPU,
/// and the cheapest mitigation is that this stays visible no matter which
/// tab the user is on.
class DeviceBar {
public:
    /// Draws a fixed-height child window listing model.devices(). Calls
    /// model.refresh() itself (once per frame -- see fwDeviceModel.h on why
    /// that is safe) so callers only need to draw this once per frame, above
    /// the tab bar. Selecting a row calls model.select(); the selection is
    /// what every flash operation elsewhere targets.
    ///
    /// `identificationNote` is TabRecovery::identificationNote(): one line
    /// about the Recovery tab's CPU identification, empty when none has ever
    /// completed. It is drawn HERE, in the one thing on screen at all times,
    /// rather than only on the tab that produced it, because it changes what
    /// the Flash buttons on the OTHER tabs will do -- and because the moment it
    /// is DISCARDED is the moment a user would otherwise watch a capability
    /// they had just earned disappear with the explanation left on a page they
    /// are no longer looking at.
    ///
    /// `identificationDiscarded` is TabRecovery::identificationDiscarded(): it
    /// decides only the colour, and it is a separate boolean rather than
    /// something inferred from the note's text because a display must never
    /// depend on reading words out of a sentence somebody may reword.
    ///
    /// `theme` is the app's live theme selection, drawn here as a picker
    /// immediately left of the Rescan button and written in place when the user
    /// changes it. It lives in this bar rather than in a menu bar of its own
    /// because the bar is the app's only always-visible chrome; the caller
    /// still owns the value and is what persists it to settings.
    /// `onInstallBootloader`, when set, is invoked by the "Install it" button of
    /// the banner shown for a board with no OG bootloader (see
    /// ogBootloaderState(), fwCpuIdentify.h). The caller owns the tab bar, so it
    /// alone can switch to the Default Firmware card that performs the install.
    /// Passing an empty function suppresses the banner's button but not the
    /// banner: the fact is worth stating even where nothing can act on it.
    static void draw(DeviceModel& model, const std::string& identificationNote,
                     bool identificationDiscarded, Theme& theme,
                     const std::function<void()>& onInstallBootloader = {});
};

} // namespace fwog
