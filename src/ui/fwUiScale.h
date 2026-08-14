#pragma once

namespace fwog {

/// True when the viewport is phone-narrow (set once per frame by fwApp.cpp,
/// read by any tab that lays out differently on a small screen). A global
/// rather than a parameter threaded through every draw() signature: it is
/// one bit of per-frame display geometry, and the alternative was touching
/// five call chains to deliver it.
///
/// The threshold lives with the writer in fwApp.cpp. Compact currently
/// changes: single-pane navigation in the App Explorer, touch-sized style
/// paddings, and a scrolling tab bar.
inline bool g_uiCompact = false;

} // namespace fwog
