#pragma once

struct SDL_Window;

namespace fwog {

/// Give the window the FreeWili orca from resources/product.ico, so it has an
/// identity in a taskbar, a dock and Alt-Tab instead of a generic placeholder.
///
/// NOT COMPILED ON WINDOWS, and that is not an oversight -- read
/// resources/app.rc before adding it there. On Windows the icon is wired up
/// ENTIRELY by linking that .rc: SDL3's SDL_RegisterApp adopts the first
/// RT_GROUP_ICON in the executable as the window class icon, which sets the
/// title bar, Alt-Tab, taskbar and Explorer icons in one go. That mechanism
/// has no equivalent here -- an ELF has no icon resources and no window class
/// -- so on Linux the same product.ico has to be decoded and handed to SDL at
/// runtime instead. This adds the missing half; it does not disagree with the
/// Windows half.
///
/// Best-effort by design: every failure below logs and returns, because the
/// consequence of any of them is a window with the default icon, and refusing
/// to start an app that flashes firmware over a missing decoration would be
/// the wrong trade by a wide margin.
///
/// WHAT THIS ACTUALLY CHANGES DEPENDS ON THE DISPLAY SERVER, and only the X11
/// half was verified here:
///
///   * X11: SDL sets _NET_WM_ICON on the window and window managers read it.
///     VERIFIED -- the property was read back off a live window (128x128, the
///     largest DIB in product.ico) under Xvfb.
///   * Wayland: SDL 3.4 implements this through the xdg_toplevel_icon_v1
///     protocol and FAILS the call outright on a compositor that does not
///     offer it. NOT verified here -- doing so needs a real compositor. Where
///     it is unavailable the icon comes from a .desktop file matched to the
///     window's app_id (which SDL derives from the executable name, so
///     "fwOGAppExplorer"), and this project ships no .desktop file. That is a
///     packaging gap this call cannot close.
void applyWindowIcon(SDL_Window* window);

} // namespace fwog
