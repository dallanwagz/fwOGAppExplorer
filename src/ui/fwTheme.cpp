#include "ui/fwTheme.h"

// ImVec4::operator*() is opt-in and must be requested before imgui.h is
// first included (it is compiled into the struct definition itself, gated
// on this macro). ImLerp() lives in imgui_internal.h, not the public
// imgui.h -- fwcom's original StyleColorsWili() lives inside imgui.cpp
// itself, where both are already visible. This is the one internal header
// this file needs; nothing here patches imgui, and the upstream checkout
// stays untouched.
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include <IconsMaterialDesign.h>

namespace fwog {
namespace {

// Copied verbatim (colour-by-colour) from fwcom's ImGui::StyleColorsWili(),
// in freewili-firmware at
// freewilimain/testprojects/fwcom/imgui/imgui_draw.cpp:388-446. Kept here as a
// free function in our own file -- not a patch to the upstream imgui checkout
// -- so Dear ImGui stays pristine and imgui version bumps need nothing
// re-applied.
void applyWiliColors()
{
    ImGuiStyle* style = &ImGui::GetStyle();
    ImVec4* colors = style->Colors;

    colors[ImGuiCol_Text]                   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
    colors[ImGuiCol_Border]                 = ImVec4(0.43f, 0.0f, 0.00f, 0.50f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.38f, 0.38, 0.38, 0.54f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.45f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.66f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.96f, 0.0f, 0.0f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.24f, 0.52f, 0.88f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.96f, 0.0f, 0.0f, 0.40f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.66f, 0.0f, 0.0f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.5f, 0.0f, 0.0f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.98f, 0.0f, 0.0f, 0.31f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.98f, 0.0f, 0.0f, 0.80f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.98f, 0.0f, 0.0f, 1.00f);
    colors[ImGuiCol_Separator]              = colors[ImGuiCol_Border];
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.26f, 0.59f, 0.98f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
    colors[ImGuiCol_InputTextCursor]        = colors[ImGuiCol_Text];
    colors[ImGuiCol_TabHovered]             = colors[ImGuiCol_HeaderHovered];
    // DELIBERATE DIVERGENCE FROM fwcom, requested by the project owner after
    // reviewing a screenshot: fwcom's Tab is ImLerp(Header, TitleBgActive,
    // 0.80f), which comes out to roughly (0.72, 0, 0, 0.86) -- visually
    // almost indistinguishable from TabSelected's ~(0.79, 0, 0, 1.0), so the
    // active tab is not identifiable at a glance. That's tolerable in
    // fwcom; it is not here, because this app has a destructive firmware
    // flash path behind one of these tabs and knowing which tab you're on
    // matters more. Every other colour in this function is byte-identical
    // to fwcom's StyleColorsWili() -- do not "fix" this one back to match;
    // it's intentional.
    colors[ImGuiCol_Tab]                    = ImVec4(0.28f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TabSelected]            = ImLerp(colors[ImGuiCol_HeaderActive], colors[ImGuiCol_TitleBgActive], 0.60f);
    colors[ImGuiCol_TabSelectedOverline]    = colors[ImGuiCol_HeaderActive];
    // TabDimmed is fwcom's ImLerp(Tab, TitleBg, 0.80f); with fwcom's original
    // (brighter) Tab that lerp lands darker than Tab, as intended for an
    // unfocused tab bar. With Tab darkened above, that same formula would
    // lerp *up* toward TitleBg (0.45, 0, 0) and come out lighter than Tab --
    // inverting the active/dimmed relationship. Set explicitly, darker than
    // the new Tab, so an unfocused tab bar still reads as dimmer than a
    // focused one.
    colors[ImGuiCol_TabDimmed]              = ImVec4(0.16f, 0.00f, 0.00f, 1.00f);
    // TabDimmedSelected depends only on TabSelected (unchanged above) and
    // TitleBg, not on Tab, so fwcom's formula still produces a value dimmer
    // than TabSelected here -- no inversion, left as verbatim fwcom.
    colors[ImGuiCol_TabDimmedSelected]      = ImLerp(colors[ImGuiCol_TabSelected],  colors[ImGuiCol_TitleBg], 0.40f);
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.50f, 0.50f, 0.50f, 0.00f);
    colors[ImGuiCol_DockingPreview]         = colors[ImGuiCol_HeaderActive] * ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_PlotLines]              = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);   // Prefer using Alpha=1.0 here
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);   // Prefer using Alpha=1.0 here
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    colors[ImGuiCol_TextLink]               = colors[ImGuiCol_HeaderActive];
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
    colors[ImGuiCol_DragDropTarget]         = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavCursor]              = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
}

} // namespace

void applyTheme(Theme theme)
{
    switch (theme) {
    case Theme::Dark:    ImGui::StyleColorsDark();    break;
    case Theme::Light:   ImGui::StyleColorsLight();   break;
    case Theme::Classic: ImGui::StyleColorsClassic(); break;
    case Theme::Wili:
    default:
        // StyleColorsDark() runs first so that any ImGuiCol_ the Wili list
        // does not name still gets a sane value rather than whatever the
        // previous theme left.
        ImGui::StyleColorsDark();
        applyWiliColors();
        break;
    }
}

const char* themeName(Theme theme)
{
    switch (theme) {
    case Theme::Wili:    return "Wili";
    case Theme::Dark:    return "Dark";
    case Theme::Light:   return "Light";
    case Theme::Classic: return "Classic";
    }
    return "Wili";
}

namespace {
// ICON_MD_PALETTE relies on the Material Icons merge into the UI font:
// Fonts::ui() is FiraCode with the icon glyphs merged in (see fwFonts.cpp).
constexpr const char* kThemeLabel = ICON_MD_PALETTE " Theme";
constexpr const char* kThemePopup = "##ThemePicker";
} // namespace

float themePickerWidth()
{
    return ImGui::CalcTextSize(kThemeLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

void drawThemePicker(Theme& current)
{
    if (ImGui::Button(kThemeLabel))
        ImGui::OpenPopup(kThemePopup);

    if (ImGui::BeginPopup(kThemePopup)) {
        for (int i = 0; i < kThemeCount; ++i) {
            Theme t = static_cast<Theme>(i);
            if (ImGui::MenuItem(themeName(t), nullptr, t == current)) {
                current = t;
                applyTheme(t);
            }
        }
        ImGui::EndPopup();
    }
}

} // namespace fwog
