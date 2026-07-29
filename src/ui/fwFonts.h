#pragma once

#include <imgui.h>

namespace fwog::Fonts {

/// Loads the UI font (FiraCode with Material Icons merged in) and a
/// standalone 24px icon font into the atlas. Call once, before the first
/// ImGui_ImplSDLRenderer3_Init() / atlas bake, and before the first
/// NewFrame().
void initialize(ImGuiIO& io, int pixelSize);

/// FiraCode at `pixelSize`, Material Icons glyphs merged in. Null before
/// initialize() runs.
ImFont* ui();

/// Material Icons only, sized for 24x24 toolbar buttons. Null before
/// initialize() runs.
ImFont* icons24();

} // namespace fwog::Fonts
