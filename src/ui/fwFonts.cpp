#include "ui/fwFonts.h"

#include <FiraCode-Regular.hpp>
#include <MaterialIcons-Regular.hpp>
#include <IconsMaterialDesign.h>

// Ported from fwcom's FontManager (imgui/FontManager.cpp), keeping the
// comments that explain *why*. The codeFonts size map is dropped -- this app
// has no code editor -- but uiFont and iconFont24 are kept as-is.

namespace fwog::Fonts {
namespace {

ImFont* g_uiFont = nullptr;
ImFont* g_iconFont24 = nullptr;

// Glyph range covering the Material Icons private-use block. ImGui keeps
// a pointer to this array, so it must outlive initialize().
static const ImWchar kMaterialIconsGlyphRanges[] = { ICON_MIN_MD, ICON_MAX_16_MD, 0 };

// Glyph range for the UI text font: Basic Latin + Latin Supplement (default)
// PLUS General Punctuation (U+2000-U+206F) so common typographic glyphs
// render correctly. Without this, list bullets (U+2022), en/em dashes,
// smart quotes and ellipsis come out as the ImGui missing-glyph fallback
// ('?'). ImGui keeps a pointer to this array, so it must outlive
// initialize().
static const ImWchar kUiTextGlyphRanges[] = {
    0x0020, 0x00FF,   // Basic Latin + Latin Supplement (the ImGui default)
    0x2000, 0x206F,   // General Punctuation (bullet, en/em dash, quotes, ellipsis)
    0,
};

void mergeMaterialIconsInto(ImGuiIO& io, ImFont* /*intoFont*/, int pixelSize)
{
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = (float)pixelSize; // monospace-align icons with UI text
    // Material Icons glyphs sit high on the text line, so they look top-heavy
    // inside framed buttons. Nudge them down to vertically center them.
    cfg.GlyphOffset.y = (float)pixelSize * 0.10f;
    // Static array: the atlas must not take ownership and free it.
    cfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(fonts_MaterialIcons_Regular_ttf,
                                    (int)fonts_MaterialIcons_Regular_ttf_len,
                                    (float)pixelSize, &cfg, kMaterialIconsGlyphRanges);
}

ImFont* loadStandaloneIconFont(ImGuiIO& io, int pixelSize)
{
    ImFontConfig cfg;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = (float)pixelSize;
    cfg.FontDataOwnedByAtlas = false;
    return io.Fonts->AddFontFromMemoryTTF(fonts_MaterialIcons_Regular_ttf,
                                           (int)fonts_MaterialIcons_Regular_ttf_len,
                                           (float)pixelSize, &cfg, kMaterialIconsGlyphRanges);
}

} // namespace

void initialize(ImGuiIO& io, int pixelSize)
{
    // fonts_FiraCode_FiraCode_Regular_ttf is a static array (compiled-in, not
    // heap-allocated). AddFontFromMemoryTTF() defaults FontDataOwnedByAtlas to
    // true when no config is supplied, which makes the atlas free() this
    // pointer when the atlas's input data is cleared -- undefined behavior
    // against static/.data memory. Do not simplify this back to a bare
    // `nullptr` config.
    //
    // This ImFontConfig is safe to reuse across the calls below:
    // AddFontFromMemoryTTF() copies *font_cfg_template into a local
    // ImFontConfig and only mutates that local copy (SizePixels, GlyphRanges,
    // FontData, FontDataSize) before handing it to AddFont(); our template
    // object here is never written back to. The per-call SizePixels and
    // GlyphRanges arguments below still take effect because
    // AddFontFromMemoryTTF applies them to its local copy after cloning the
    // template.
    ImFontConfig firaCodeCfg;
    firaCodeCfg.FontDataOwnedByAtlas = false;

    g_uiFont = io.Fonts->AddFontFromMemoryTTF(fonts_FiraCode_FiraCode_Regular_ttf,
                                               (int)fonts_FiraCode_FiraCode_Regular_ttf_len,
                                               (float)pixelSize, &firaCodeCfg, kUiTextGlyphRanges);
    mergeMaterialIconsInto(io, g_uiFont, pixelSize);

    g_iconFont24 = loadStandaloneIconFont(io, 24);
}

ImFont* ui()
{
    return g_uiFont;
}

ImFont* icons24()
{
    return g_iconFont24;
}

} // namespace fwog::Fonts
