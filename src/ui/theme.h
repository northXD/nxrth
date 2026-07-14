// Adonai UI theme: modern dark styling + font loading (Tahoma + FontAwesome).
#pragma once

struct ImFont;

namespace adonai::ui {

// Applies Adonai's modern dark ImGui style (rounding, spacing, palette).
void ApplyTheme();

// Loads Tahoma (body / bold / title sizes) from the Windows font dir and MERGES
// FontAwesome 6 Solid (data/fonts/fa-solid-900.ttf) into each so ICON_FA_* glyphs
// render inline with text. Falls back to the default font if Tahoma is missing.
void LoadFonts();

// Bold Tahoma (+icons), for headers/labels. Null until LoadFonts() runs.
ImFont* FontBold();
// Large Tahoma (+icons), for the title bar. Null until LoadFonts() runs.
ImFont* FontTitle();
// Large icon font (bigger FontAwesome), for the sidebar nav. Null until loaded.
ImFont* FontIconsLarge();

}  // namespace adonai::ui
