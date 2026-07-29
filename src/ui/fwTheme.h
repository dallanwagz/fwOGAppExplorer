#pragma once

namespace fwog {

enum class Theme { Wili = 0, Dark = 1, Light = 2, Classic = 3 };
constexpr int kThemeCount = 4;

void applyTheme(Theme theme);
const char* themeName(Theme theme);

/// Draws the theme picker as a single button that opens a popup listing every
/// theme, the current one check-marked. Picking one writes `current` and calls
/// applyTheme() immediately, so the change is visible on the same frame.
///
/// A button-plus-popup rather than ImGui::BeginMenu(): this is drawn inside an
/// ordinary window (the device bar) and BeginMenu() only works inside a menu
/// bar, which this app no longer has.
void drawThemePicker(Theme& current);

/// Width the drawThemePicker() button occupies with the current style, for
/// callers that right-align a row of widgets and must know the total before
/// drawing the first one.
float themePickerWidth();

} // namespace fwog
