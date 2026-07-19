// Nxrth UI theme: modern dark ImGui style + font loading (Tahoma + FontAwesome 6).
#include "imgui.h"
#include "ui/theme.h"
#include "ui/icons_fa.h"

#include <windows.h>
#include <string>

namespace nxrth::ui {
namespace {

// Stored fonts, resolved by LoadFonts(). ImGui owns the atlas; these are views.
ImFont* g_font_body      = nullptr;  // also the default font
ImFont* g_font_bold      = nullptr;
ImFont* g_font_title     = nullptr;
ImFont* g_font_icons_big = nullptr;

// ImGui keeps the pointer to the glyph range, so it must outlive the frame.
static const ImWchar kIconRange[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };

// Text glyphs to rasterize for the body/bold/title faces. The default ImGui range
// stops at U+00FF (covers ç ö ü) but NOT the Turkish letters ğ Ğ ş Ş ı İ, which
// live in Latin Extended-A — without this range they render as '?'/garbage. We
// also pull in General Punctuation (smart quotes, dashes, ellipsis, bullet) and
// currency symbols (₺ €) so AI answers and UI text display fully. Missing glyphs
// are simply skipped by the atlas builder, so an over-broad range is harmless.
static const ImWchar kTextRange[] = {
    0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement
    0x0100, 0x017F,  // Latin Extended-A  (Turkish ğ Ğ ş Ş ı İ + Central-European)
    0x0180, 0x024F,  // Latin Extended-B
    0x2010, 0x2027,  // General Punctuation (– — ' ' " " … •)
    0x20A0, 0x20BF,  // Currency symbols (₺ Turkish lira, €, etc.)
    0,
};

// Absolute path to a font inside the Windows Fonts directory.
std::string WinFont(const char* name) {
    char dir[MAX_PATH] = {0};
    UINT n = ::GetWindowsDirectoryA(dir, MAX_PATH);
    std::string path(dir, n);
    if (!path.empty() && path.back() != '\\') path.push_back('\\');
    path += "Fonts\\";
    path += name;
    return path;
}

// Merges fa-solid-900.ttf at `size` on top of the font just added to the atlas.
void MergeIcons(ImFontAtlas* atlas, const char* fa_path, float size) {
    ImFontConfig cfg;
    cfg.MergeMode        = true;
    cfg.GlyphMinAdvanceX = size;   // monospaced icon column
    cfg.PixelSnapH       = true;
    atlas->AddFontFromFileTTF(fa_path, size, &cfg, kIconRange);
}

// Adds one Tahoma face at `size`, then merges the icon font over it.
// Returns nullptr if the Tahoma file could not be loaded.
ImFont* AddTextFont(ImFontAtlas* atlas, const std::string& tahoma,
                    float size, const char* fa_path) {
    ImFont* f = atlas->AddFontFromFileTTF(tahoma.c_str(), size, nullptr, kTextRange);
    if (!f) return nullptr;
    MergeIcons(atlas, fa_path, size);
    return f;
}

} // namespace

void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);

    // --- Geometry: rounded, roomy, subtle borders --------------------------
    s.WindowPadding    = ImVec2(12, 12);
    s.FramePadding     = ImVec2(10, 6);
    s.CellPadding      = ImVec2(8, 5);
    s.ItemSpacing      = ImVec2(9, 8);
    s.ItemInnerSpacing = ImVec2(7, 6);
    s.IndentSpacing    = 22.0f;
    s.ScrollbarSize    = 12.0f;
    s.GrabMinSize      = 10.0f;

    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize  = 1.0f;
    s.PopupBorderSize  = 1.0f;
    s.FrameBorderSize  = 1.0f;
    s.TabBorderSize    = 0.0f;

    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 4.0f;
    s.PopupRounding     = 5.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 5.0f;

    s.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.AntiAliasedLines         = true;
    s.AntiAliasedFill          = true;

    // --- Palette: deep navy with a single medium-blue accent (Lucifer-style) --
    const ImVec4 accent      = ImVec4(0.130f, 0.400f, 0.780f, 1.00f); // medium blue
    const ImVec4 accentHover = ImVec4(0.180f, 0.480f, 0.880f, 1.00f);
    const ImVec4 accentDim   = ImVec4(0.130f, 0.400f, 0.780f, 0.32f);

    const ImVec4 bg0     = ImVec4(0.039f, 0.055f, 0.106f, 1.00f); // window (deep navy)
    const ImVec4 bg1     = ImVec4(0.063f, 0.086f, 0.157f, 1.00f); // child/frame/button
    const ImVec4 bg2     = ImVec4(0.094f, 0.129f, 0.212f, 1.00f); // hovered frame
    const ImVec4 bg3     = ImVec4(0.129f, 0.176f, 0.271f, 1.00f); // active/header
    const ImVec4 border  = ImVec4(0.192f, 0.271f, 0.408f, 0.55f); // navy hairline
    const ImVec4 text    = ImVec4(0.808f, 0.855f, 0.918f, 1.00f); // bluish white
    const ImVec4 textDim = ImVec4(0.482f, 0.549f, 0.647f, 1.00f); // muted blue-gray

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = text;
    c[ImGuiCol_TextDisabled]         = textDim;
    c[ImGuiCol_WindowBg]             = bg0;
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = ImVec4(0.090f, 0.096f, 0.114f, 0.98f);
    c[ImGuiCol_Border]               = border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = bg1;
    c[ImGuiCol_FrameBgHovered]       = bg2;
    c[ImGuiCol_FrameBgActive]        = bg3;
    c[ImGuiCol_TitleBg]              = bg0;
    c[ImGuiCol_TitleBgActive]        = bg0;
    c[ImGuiCol_TitleBgCollapsed]     = bg0;
    c[ImGuiCol_MenuBarBg]            = bg1;
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = bg3;
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.30f, 0.35f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = accent;
    c[ImGuiCol_CheckMark]            = accent;
    c[ImGuiCol_SliderGrab]           = accent;
    c[ImGuiCol_SliderGrabActive]     = accentHover;
    c[ImGuiCol_Button]               = bg2;
    c[ImGuiCol_ButtonHovered]        = bg3;
    c[ImGuiCol_ButtonActive]         = accent;
    c[ImGuiCol_Header]               = accentDim;
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.42f, 0.51f, 0.96f, 0.42f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.42f, 0.51f, 0.96f, 0.62f);
    c[ImGuiCol_Separator]            = border;
    c[ImGuiCol_SeparatorHovered]     = accentHover;
    c[ImGuiCol_SeparatorActive]      = accent;
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = accentDim;
    c[ImGuiCol_ResizeGripActive]     = accent;
    c[ImGuiCol_Tab]                  = bg1;
    c[ImGuiCol_TabHovered]           = bg3;
    c[ImGuiCol_TabActive]            = bg3;
    c[ImGuiCol_TabUnfocused]         = bg0;
    c[ImGuiCol_TabUnfocusedActive]   = bg1;
    c[ImGuiCol_PlotLines]            = accent;
    c[ImGuiCol_PlotLinesHovered]     = accentHover;
    c[ImGuiCol_PlotHistogram]        = accent;
    c[ImGuiCol_PlotHistogramHovered] = accentHover;
    c[ImGuiCol_TableHeaderBg]        = bg1;
    c[ImGuiCol_TableBorderStrong]    = border;
    c[ImGuiCol_TableBorderLight]     = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.018f);
    c[ImGuiCol_TextSelectedBg]       = accentDim;
    c[ImGuiCol_DragDropTarget]       = accentHover;
    c[ImGuiCol_NavHighlight]         = accent;
    c[ImGuiCol_NavWindowingHighlight]= accentHover;
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0, 0, 0, 0.55f);
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontAtlas* atlas = io.Fonts;
    atlas->Clear();

    const std::string tahoma   = WinFont("tahoma.ttf");
    const std::string tahomabd = WinFont("tahomabd.ttf");
    // Relative to the working directory (the app is launched with its project
    // root as CWD), so no absolute install path is baked in.
    const char* fa = "data\\fonts\\fa-solid-900.ttf";

    // Body (regular Tahoma ~16) + merged icons. This is the default font.
    g_font_body = AddTextFont(atlas, tahoma, 16.0f, fa);
    if (!g_font_body) {
        // Tahoma missing: fall back to the built-in font.
        g_font_body = atlas->AddFontDefault();
        io.FontDefault = g_font_body;
        g_font_bold = g_font_title = g_font_body;
        g_font_icons_big = atlas->AddFontFromFileTTF(fa, 20.0f, nullptr, kIconRange);
        if (!g_font_icons_big) g_font_icons_big = g_font_body;
        atlas->Build();
        return;
    }
    io.FontDefault = g_font_body;

    // Bold (~16) and Title (~19) from tahomabd.ttf, each with merged icons.
    g_font_bold  = AddTextFont(atlas, tahomabd, 16.0f, fa);
    g_font_title = AddTextFont(atlas, tahomabd, 19.0f, fa);
    if (!g_font_bold)  g_font_bold  = g_font_body;
    if (!g_font_title) g_font_title = g_font_bold;

    // Large FontAwesome alone (~20) for the sidebar nav.
    g_font_icons_big = atlas->AddFontFromFileTTF(fa, 20.0f, nullptr, kIconRange);
    if (!g_font_icons_big) g_font_icons_big = g_font_body;

    atlas->Build();
}

ImFont* FontBold()       { return g_font_bold; }
ImFont* FontTitle()      { return g_font_title; }
ImFont* FontIconsLarge() { return g_font_icons_big; }

} // namespace nxrth::ui
