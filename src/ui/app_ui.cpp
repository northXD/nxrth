#include "ui/app_ui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <numeric>  // std::iota (Random X account pick)
#include <optional>
#include <random>   // std::mt19937 (Random X account pick)
#include <string>
#include <vector>

#include <windows.h>  // MultiByteToWideChar - open UTF-8/Turkish paths as wide

#include <fstream>

#include "automation/config_store.h"  // save/load the fleet AutomationConfig
#include "core/accounts.h"
#include "core/constants.h"
#include "core/logger.h"
#include "imgui.h"
#include "ui/icons_fa.h"
#include "ui/theme.h"
#include "world/items.h"  // adonai::world::ItemsDat (item names for inventory)

namespace adonai::ui {
namespace {

// Fleet AutomationConfig module keys (match the module->name() the engine
// attaches under src/automation/modules/). Toggling these on the shared
// FleetState is how every bot learns which fleet-aware routines are enabled.
// MUST match the module kName in src/automation/modules/*.h (FleetState config is
// keyed by module name; a mismatch means the toggle never enables the module).
constexpr const char* kModCollect = "collect";
constexpr const char* kModGeiger = "geiger";
constexpr const char* kModCoordinate = "coordinate";
constexpr const char* kModWebhook = "webhook";

// --- palette (deep navy + medium-blue accent; Lucifer-style) ----------------
const ImVec4 kAccent{0.16f, 0.46f, 0.85f, 1.0f};      // active tab / marks
const ImVec4 kAccentDim{0.13f, 0.40f, 0.78f, 0.30f};
const ImVec4 kAccentHover{0.18f, 0.48f, 0.88f, 0.55f};
const ImVec4 kTitleBg{0.055f, 0.078f, 0.145f, 1.0f};  // title + tab band
const ImVec4 kSideBg{0.047f, 0.067f, 0.129f, 1.0f};   // left column bg
const ImVec4 kPanelBg{0.027f, 0.039f, 0.078f, 1.0f};  // inset list / right pane
const ImVec4 kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
const ImVec4 kHoverWhite{1.0f, 1.0f, 1.0f, 0.08f};
const ImVec4 kActiveWhite{1.0f, 1.0f, 1.0f, 0.14f};
const ImVec4 kRed{0.87f, 0.28f, 0.24f, 1.0f};         // window-control glyphs
const ImVec4 kDanger{0.87f, 0.28f, 0.24f, 0.30f};     // red hover (close)
const ImVec4 kError{0.95f, 0.35f, 0.35f, 1.0f};

std::string trimmed(const char* s) {
    std::string v(s);
    const auto b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = v.find_last_not_of(" \t\r\n");
    return v.substr(b, e - b + 1);
}

// Strip one layer of surrounding "" or '' (Windows "Copy as path" wraps in ").
std::string strip_quotes(std::string p) {
    if (p.size() >= 2) {
        const char a = p.front(), z = p.back();
        if ((a == '"' && z == '"') || (a == '\'' && z == '\''))
            p = p.substr(1, p.size() - 2);
    }
    return p;
}

// UTF-8 (what ImGui InputText stores) -> UTF-16, so non-ASCII paths (e.g. the
// Turkish "Masaüstü") open correctly - a narrow std::ifstream would fail on them.
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Read a whole file given a (possibly quoted, possibly non-ASCII) UTF-8 path.
bool read_text_file(const std::string& utf8_path, std::string& out) {
    const std::wstring wpath = utf8_to_wide(strip_quotes(utf8_path));
    std::ifstream f(wpath.c_str(), std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

template <std::size_t N>
void set_buf(std::array<char, N>& buf, const std::string& s) {
    std::snprintf(buf.data(), buf.size(), "%s", s.c_str());
}

// Bot name colour by status string (BotInfo.status = to_string(BotStatus)):
//   online (in_game) -> green, stopped (update_required) -> red, else login -> orange.
ImVec4 status_color(const std::string& s) {
    if (s == "in_game") return ImVec4(0.32f, 0.85f, 0.42f, 1.0f);        // green
    if (s == "update_required") return ImVec4(0.92f, 0.32f, 0.32f, 1.0f); // red
    return ImVec4(0.96f, 0.66f, 0.22f, 1.0f);                             // orange
}

// Pill toggle switch (ImDrawList) - matches the reference's on/off switches.
bool ToggleSwitch(const char* id, bool* v) {
    const float h = ImGui::GetFrameHeight() * 0.78f;
    const float w = h * 1.85f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = h * 0.5f;
    const ImU32 bg = *v ? ImGui::GetColorU32(kAccent)
                        : (ImGui::IsItemHovered() ? IM_COL32(70, 82, 108, 255)
                                                  : IM_COL32(46, 56, 78, 255));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, r);
    const float cx = *v ? (p.x + w - r) : (p.x + r);
    dl->AddCircleFilled(ImVec2(cx, p.y + r), r - 2.0f, IM_COL32(232, 237, 246, 255));
    return clicked;
}

}  // namespace

AppUi::AppUi(adonai::bot::BotManager& manager, adonai::proxy::ProxyPool& proxy_pool,
             adonai::ui::IWindowHost& host)
    : manager_(manager), proxy_pool_(proxy_pool), host_(host) {
    set_buf(pp_rot_scheme_, "auto");
}

// ---------------------------------------------------------------------------
// Frame root: a single borderless window that fills the whole (fixed) viewport.
// No docking, no menu bar, no extra ImGui windows.
// ---------------------------------------------------------------------------
void AppUi::Draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##adonai_root", nullptr, flags);
    ImGui::PopStyleVar(3);

    // Vertical stack: custom title bar, horizontal tab strip, then the content.
    DrawTitleBar();
    DrawTabBar();

    ImGui::BeginChild("##content", ImVec2(0.0f, 0.0f), false);
    DrawContent();
    ImGui::EndChild();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Custom title bar (~34px): app mark + version, a drag handle over the empty
// area, and right-aligned lock / always-on-top / minimize / close buttons.
// ---------------------------------------------------------------------------
namespace {
// Borderless icon button with a subtle hover highlight. `glyph` colours the icon;
// `hover` is the hover/active background (red for the close button).
bool TitleIconButton(const char* id, const char* icon, float w, float h,
                     const ImVec4& glyph, const ImVec4& hover, const char* tooltip) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, kTransparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hover);
    ImGui::PushStyleColor(ImGuiCol_Text, glyph);
    const bool clicked = ImGui::Button(icon, ImVec2(w, h));
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}
}  // namespace

// ---------------------------------------------------------------------------
// Title bar (~30px): left dropdown chevron, centered "Adonai v.. [GT ..]" title,
// right-aligned RED window controls (move/on-top, lock, minimize, close).
// ---------------------------------------------------------------------------
void AppUi::DrawTitleBar() {
    const float kH = 30.0f;
    const float kBtnW = 30.0f;
    const int kNBtn = 5;
    const float kChevW = 26.0f;

    ImGui::BeginChild("##titlebar", ImVec2(0.0f, kH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 wp = ImGui::GetWindowPos();
    const float ww = ImGui::GetWindowSize().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(wp, ImVec2(wp.x + ww, wp.y + kH), ImGui::GetColorU32(kTitleBg));
    dl->AddLine(ImVec2(wp.x, wp.y + kH - 1.0f), ImVec2(wp.x + ww, wp.y + kH - 1.0f),
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.5f)));

    // Left: dropdown chevron (small app menu).
    ImGui::SetCursorPos(ImVec2(4.0f, 0.0f));
    if (TitleIconButton("menu", ICON_FA_CHEVRON_DOWN, kChevW, kH,
                        ImVec4(0.70f, 0.78f, 0.90f, 1.0f), kHoverWhite, nullptr))
        ImGui::OpenPopup("##appmenu");
    if (ImGui::BeginPopup("##appmenu")) {
        ImGui::TextDisabled("Adonai v%s - GT %s", adonai::constants::APP_VERSION.data(),
                            adonai::constants::GAME_VER.data());
        ImGui::Separator();
        if (ImGui::MenuItem("Her zaman ustte", nullptr, host_.is_always_on_top()))
            host_.set_always_on_top(!host_.is_always_on_top());
        if (ImGui::MenuItem("Pencereyi kilitle", nullptr, host_.is_locked()))
            host_.set_locked(!host_.is_locked());
        ImGui::Separator();
        if (ImGui::MenuItem("Cikis")) host_.request_close();
        ImGui::EndPopup();
    }

    // Right: red window controls.
    ImGui::SetCursorPos(ImVec2(ww - kNBtn * kBtnW, 0.0f));
    if (TitleIconButton("top", ICON_FA_UP_DOWN_LEFT_RIGHT, kBtnW, kH, kRed, kHoverWhite,
                        "Her zaman ustte"))
        host_.set_always_on_top(!host_.is_always_on_top());
    ImGui::SameLine(0.0f, 0.0f);
    if (TitleIconButton("lock", host_.is_locked() ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN,
                        kBtnW, kH, kRed, kHoverWhite, "Pencereyi kilitle"))
        host_.set_locked(!host_.is_locked());
    ImGui::SameLine(0.0f, 0.0f);
    if (TitleIconButton("min", ICON_FA_WINDOW_MINIMIZE, kBtnW, kH, kRed, kHoverWhite,
                        "Kucult"))
        host_.minimize();
    ImGui::SameLine(0.0f, 0.0f);
    // Fixed-size window: the maximize glyph is shown for parity only (no-op).
    TitleIconButton("max", ICON_FA_EXPAND, kBtnW, kH, kRed, kHoverWhite, "Sabit boyut");
    ImGui::SameLine(0.0f, 0.0f);
    if (TitleIconButton("close", ICON_FA_POWER_OFF, kBtnW, kH, kRed, kDanger, "Kapat"))
        host_.request_close();

    // Centered title text (non-interactive).
    char title[96];
    std::snprintf(title, sizeof(title), "Adonai v%s  [GT %s]",
                  adonai::constants::APP_VERSION.data(),
                  adonai::constants::GAME_VER.data());
    const float tw = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPos(ImVec2((ww - tw) * 0.5f, (kH - ImGui::GetFontSize()) * 0.5f));
    ImGui::TextUnformatted(title);

    // Drag handle over the middle gap (between chevron and buttons). Submitted
    // last so it wins hover there; the centered text above is non-interactive.
    float dx0 = kChevW + 4.0f;
    float dx1 = ww - kNBtn * kBtnW - 4.0f;
    if (dx1 < dx0 + 4.0f) dx1 = dx0 + 4.0f;
    ImGui::SetCursorPos(ImVec2(dx0, 0.0f));
    ImGui::InvisibleButton("##drag", ImVec2(dx1 - dx0, kH));
    if (ImGui::IsItemActive() && !host_.is_locked()) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        host_.drag_by(d.x, d.y);
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Horizontal top tab strip: 7 tabs (Bots/List/Executor/Proxy/Switch/Database/
// Settings). Active tab = solid blue with white text; others transparent + dim.
// ---------------------------------------------------------------------------
void AppUi::DrawTabBar() {
    struct Tab { const char* icon; const char* label; };
    static const Tab kTabs[] = {
        {ICON_FA_ROBOT, "Bots"},        {ICON_FA_LIST, "List"},
        {ICON_FA_CODE, "Executor"},     {ICON_FA_GLOBE, "Proxy"},
        {ICON_FA_RIGHT_LEFT, "Switch"}, {ICON_FA_DATABASE, "Database"},
        {ICON_FA_GEAR, "Settings"},
    };
    constexpr int kTabCount = 7;
    const float kH = 34.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
    ImGui::BeginChild("##tabbar", ImVec2(0.0f, kH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), ImGui::GetColorU32(kTitleBg));
    dl->AddLine(ImVec2(wp.x, wp.y + kH - 1.0f), ImVec2(wp.x + ws.x, wp.y + kH - 1.0f),
                ImGui::GetColorU32(kAccentDim));

    ImGui::SetCursorPos(ImVec2(8.0f, 5.0f));
    for (int i = 0; i < kTabCount; ++i) {
        const bool sel = (section_ == i);
        char lbl[48];
        std::snprintf(lbl, sizeof(lbl), "%s  %s", kTabs[i].icon, kTabs[i].label);
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? kAccent : kTransparent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? kAccent : kHoverWhite);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, sel ? kAccent : kActiveWhite);
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            sel ? ImVec4(1, 1, 1, 1) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button(lbl, ImVec2(0.0f, kH - 10.0f)))
            section_ = i;
        ImGui::PopStyleColor(4);
        ImGui::PopID();
        ImGui::SameLine(0.0f, 4.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Content pane: section header + a scrollable body that renders only the
// selected section.
// ---------------------------------------------------------------------------
void AppUi::DrawContent() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::BeginChild("##contentpad", ImVec2(0.0f, 0.0f), false);

    switch (section_) {
    case 0: DrawBotsTab(); break;         // Bots   (list column + detail pane)
    case 1: DrawListSection(); break;     // List   (full-width table)
    case 2: DrawAutomationSection(); break; // Executor
    case 3: DrawProxySection(); break;    // Proxy
    case 4: DrawSwitchSection(); break;   // Switch (world/warp)
    case 5: DrawAccountsSection(); break; // Database (bulk add)
    case 6: DrawSettingsSection(); break; // Settings (console + toggles)
    default: break;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// §Bots tab: LEFT column (Select All / Multi-Select / bot list / search /
// Load-Save / Add-Remove) + RIGHT per-bot detail pane. Mirrors the reference.
// ---------------------------------------------------------------------------
void AppUi::DrawBotsTab() {
    const float kLeftW = 178.0f;

    // ---- LEFT COLUMN -------------------------------------------------------
    ImGui::BeginChild("##botsleft", ImVec2(kLeftW, 0.0f), false);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Select All");
    ImGui::SameLine(kLeftW - 46.0f);
    ToggleSwitch("##selall", &select_all_);

    {  // Enable Multi-Select toggle (full width)
        const bool on = multi_select_;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? kAccent : kSideBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? kAccentHover : kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHover);
        if (ImGui::Button("Enable Multi-Select", ImVec2(-FLT_MIN, 0.0f)))
            multi_select_ = !multi_select_;
        ImGui::PopStyleColor(3);
    }

    // Bot list box (fills the middle; ~146px reserved for the controls below).
    std::vector<adonai::bot::BotInfo> rows = manager_.list();
    // Auto-select the first bot so the detail pane is populated (like the ref).
    if (selected_bot_ < 0 && !rows.empty())
        selected_bot_ = static_cast<int>(rows.front().id);
    std::string filter = trimmed(bot_search_.data());
    std::transform(filter.begin(), filter.end(), filter.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    float list_h = ImGui::GetContentRegionAvail().y - 146.0f;
    if (list_h < 60.0f) list_h = 60.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanelBg);
    ImGui::BeginChild("##botlist", ImVec2(0.0f, list_h), true);
    int shown = 0;
    for (const auto& r : rows) {
        std::string uname = r.username.empty() ? "(token)" : r.username;
        if (!filter.empty()) {
            std::string low = uname;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (low.find(filter) == std::string::npos) continue;
        }
        ++shown;
        const bool sel = (selected_bot_ == static_cast<int>(r.id));
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "%s##b%u", uname.c_str(), r.id);
        ImGui::PushStyleColor(ImGuiCol_Text, status_color(r.status));
        if (ImGui::Selectable(lbl, sel))
            selected_bot_ = static_cast<int>(r.id);
        ImGui::PopStyleColor();
    }
    if (shown == 0)
        ImGui::TextDisabled(rows.empty() ? "(bot yok)" : "(eslesme yok)");
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Search.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search for bots..", bot_search_.data(),
                             bot_search_.size());

    // Load | Save.
    const float half = (kLeftW - 4.0f) * 0.5f;
    if (ImGui::Button("Load", ImVec2(half, 0.0f)))
        section_ = 5;  // Database tab hosts the bulk importer
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(-FLT_MIN, 0.0f))) {
        // bot list persistence not wired yet (accounts live in the Database tab)
    }

    // Add Bot / Remove Bot (both plain dark, like the reference).
    if (ImGui::Button("Add Bot", ImVec2(-FLT_MIN, 0.0f)))
        open_add_popup_ = true;
    {
        const bool has = selected_bot_ >= 0;
        if (!has) ImGui::BeginDisabled();
        if (ImGui::Button("Remove Bot", ImVec2(-FLT_MIN, 0.0f))) {
            manager_.stop(static_cast<std::uint32_t>(selected_bot_));
            selected_bot_ = -1;
        }
        if (!has) ImGui::EndDisabled();
    }

    ImGui::EndChild();  // ##botsleft

    // ---- RIGHT PANE --------------------------------------------------------
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanelBg);
    ImGui::BeginChild("##botsright", ImVec2(0.0f, 0.0f), true);
    if (selected_bot_ >= 0) {
        DrawBotDetail();
    } else {
        const ImVec2 av = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(av.x * 0.5f - 82.0f, av.y * 0.5f - 8.0f));
        ImGui::TextDisabled("Detay icin soldan bir bot secin");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- Add Bot popup -----------------------------------------------------
    if (open_add_popup_) {
        ImGui::OpenPopup("Add Bot##popup");
        open_add_popup_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Add Bot##popup", nullptr, ImGuiWindowFlags_NoResize)) {
        DrawAddBotSection();
        ImGui::Separator();
        if (ImGui::Button("Kapat", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// List tab: full-width fleet table (one row per BotManager::list() BotInfo).
// Clicking a row selects the bot and jumps to the Bots tab's detail pane.
// ---------------------------------------------------------------------------
void AppUi::DrawListSection() {
    std::vector<adonai::bot::BotInfo> rows = manager_.list();
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });

    // id -> proxy label (BotEntry.proxy_key is the resolved "ip:port" or none).
    std::map<std::uint32_t, std::string> proxy_of;
    for (const auto& [id, entry] : manager_.bots)
        proxy_of[id] = entry.proxy_key.value_or("direct");

    ImGui::Text("%zu bot", rows.size());
    ImGui::SameLine();
    ImGui::TextDisabled(ICON_FA_CIRCLE_CHECK " yesil=online  " ICON_FA_GEAR
                        " turuncu=login  " ICON_FA_CIRCLE_XMARK
                        " kirmizi=durmus  -  bota tikla -> detay");
    ImGui::Spacing();

    if (ImGui::BeginTable("fleet", 8,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn("Bot");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("World");
        ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 46);
        ImGui::TableSetupColumn("Gems", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Proxy");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 74);
        ImGui::TableHeadersRow();

        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(r.id));

            const ImVec4 col = status_color(r.status);

            ImGui::TableSetColumnIndex(0);
            const bool selected = (selected_bot_ == static_cast<int>(r.id));
            // Click a row -> select that bot and open it in the Bots tab.
            if (ImGui::Selectable(std::to_string(r.id).c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                selected_bot_ = static_cast<int>(r.id);
                section_ = 0;  // jump to Bots tab (detail pane)
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(col, "%s", r.username.empty() ? "(token)" : r.username.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(col, "%s", r.status.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(r.world.empty() ? "-" : r.world.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", r.ping_ms);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d", r.gems);
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(proxy_of[r.id].c_str());

            ImGui::TableSetColumnIndex(7);
            if (ImGui::SmallButton(ICON_FA_CIRCLE_STOP " Stop")) {
                manager_.stop(r.id);
                if (selected_bot_ == static_cast<int>(r.id))
                    selected_bot_ = -1;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// §2.9 proxy resolution glue (manual | pool | none). No exception escapes.
// ---------------------------------------------------------------------------
bool AppUi::ResolveSpawnProxy(std::optional<adonai::bot::Socks5Config>& out) {
    out.reset();
    const std::string host = trimmed(px_host_.data());
    const bool manual = !host.empty() || px_port_ > 0;
    if (manual) {
        if (host.empty()) { add_error_ = "proxy host is required"; return false; }
        if (px_port_ <= 0 || px_port_ > 65535) {
            add_error_ = "proxy port is required";
            return false;
        }
        adonai::bot::Socks5Config cfg;
        cfg.host = host;
        cfg.port = static_cast<std::uint16_t>(px_port_);
        if (const std::string u = trimmed(px_user_.data()); !u.empty()) cfg.username = u;
        if (const std::string p = trimmed(px_pass_.data()); !p.empty()) cfg.password = p;
        out = std::move(cfg);
        return true;
    }
    if (!use_proxy_pool_) return true;  // Ok(None) - direct
    try {
        out = proxy_pool_.choose(manager_.proxy_key_counts());
    } catch (const std::exception& e) {
        add_error_ = e.what();
        return false;
    }
    return true;
}

bool AppUi::ResolveLoginProxy(std::optional<adonai::proxy::RotatingLoginProxy>& out) {
    out.reset();
    try {
        out = proxy_pool_.rotating_login_proxy();
    } catch (const std::exception& e) {
        add_error_ = e.what();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Accounts: paste or load accounts_stats.json (or user:pass lines), extract
// every account, then bulk-spawn them (each gets a pool proxy).
// ---------------------------------------------------------------------------
void AppUi::DrawAccountsSection() {
    ImGui::TextWrapped(
        "accounts_stats.json (veya user:pass satirlari) yapistir ya da dosyadan "
        "yukle; hepsini tek tikla ekle. Her bota havuzdan proxy atanir.");
    ImGui::Spacing();

    // Load from a file path (parses the FULL file, not the preview box).
    ImGui::SetNextItemWidth(-160.0f);
    ImGui::InputTextWithHint("##accpath", "C:\\...\\accounts_stats.json", acc_path_.data(),
                             acc_path_.size());
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_LAYER_GROUP " Dosyadan ayikla", ImVec2(150.0f, 0.0f))) {
        const std::string path = strip_quotes(trimmed(acc_path_.data()));
        std::string content;
        if (read_text_file(path, content)) {  // handles quotes + Turkish paths
            acc_parsed_ = adonai::core::parse_account_stats(content);
            acc_added_offset_ = 0;  // fresh list -> reset the "Add X" cursor
            set_buf(acc_text_, content.size() < acc_text_.size()
                                   ? content
                                   : content.substr(0, acc_text_.size() - 1));
            acc_status_ = std::to_string(acc_parsed_.size()) + " hesap ayiklandi";
        } else {
            acc_status_ = "dosya acilamadi: " + path;
        }
    }

    ImGui::TextUnformatted("veya yapistir:");
    ImGui::InputTextMultiline("##acctext", acc_text_.data(), acc_text_.size(),
                              ImVec2(-1.0f, 120.0f));
    if (ImGui::Button(ICON_FA_LAYER_GROUP " Yapistirilani ayikla")) {
        acc_parsed_ = adonai::core::parse_account_stats(std::string(acc_text_.data()));
        acc_added_offset_ = 0;  // fresh list -> reset the "Add X" cursor
        acc_status_ = std::to_string(acc_parsed_.size()) + " hesap ayiklandi";
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("Login modu", &acc_mode_, "Standard\0Newly\0");
    ImGui::SameLine();
    ImGui::Checkbox("Havuz proxy", &acc_use_pool_);

    ImGui::BeginDisabled(acc_parsed_.empty());

    // Shared spawn helper: spawns the accounts at the given indices (into
    // acc_parsed_), honouring the pool-proxy rule (skip rather than leak the real
    // IP when the pool is exhausted). Used by First X / Random X / Add X / All.
    auto spawn_indices = [&](const std::vector<std::size_t>& idxs) {
        int added = 0, skipped = 0;
        for (std::size_t i : idxs) {
            if (i >= acc_parsed_.size()) continue;
            const auto& a = acc_parsed_[i];
            std::optional<adonai::bot::Socks5Config> proxy;
            std::optional<adonai::proxy::RotatingLoginProxy> login_proxy;
            if (acc_use_pool_) {
                try {
                    proxy = proxy_pool_.choose(manager_.proxy_key_counts());
                } catch (...) {
                }
                try {
                    login_proxy = proxy_pool_.rotating_login_proxy();
                } catch (...) {
                }
                // Pool ran out: spawning proxy-less would login via the REAL IP
                // (24h ban). Skip instead of leaking.
                if (!proxy && !login_proxy) {
                    ++skipped;
                    continue;
                }
            }
            if (acc_mode_ == 1)
                manager_.spawn_newly(a.username, a.password, std::move(proxy),
                                     std::move(login_proxy));
            else
                manager_.spawn(a.username, a.password, std::move(proxy), std::move(login_proxy));
            ++added;
        }
        acc_status_ = std::to_string(added) + " bot eklendi";
        if (skipped > 0)
            acc_status_ += ", " + std::to_string(skipped) + " atlandi (proxy havuzu tukendi)";
    };

    // "X" count for the First/Random/Add buttons: load only a SUBSET of the parsed
    // accounts (e.g. the first 10 of 711) instead of all of them.
    if (acc_count_ < 1) acc_count_ = 1;
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("Adet (X)", &acc_count_);
    if (acc_count_ < 1) acc_count_ = 1;
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu ayiklandi)", acc_parsed_.size());

    // First X: accounts [0, X).
    if (ImGui::Button(ICON_FA_USER_PLUS " Ilk X ekle", ImVec2(150.0f, 0.0f))) {
        std::vector<std::size_t> sel;
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(acc_count_),
                                                    acc_parsed_.size());
        for (std::size_t i = 0; i < n; ++i) sel.push_back(i);
        spawn_indices(sel);
    }
    ImGui::SameLine();
    // Random X: X distinct accounts drawn at random.
    if (ImGui::Button(ICON_FA_USER_PLUS " Rastgele X ekle", ImVec2(165.0f, 0.0f))) {
        std::vector<std::size_t> idx(acc_parsed_.size());
        std::iota(idx.begin(), idx.end(), std::size_t{0});
        std::mt19937 rng{std::random_device{}()};
        std::shuffle(idx.begin(), idx.end(), rng);
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(acc_count_),
                                                    idx.size());
        idx.resize(n);
        spawn_indices(idx);
    }

    // Add X: the NEXT batch of X, advancing a cursor so repeated clicks add
    // [0,X), [X,2X), ... without re-adding the same accounts (wraps at the end).
    if (ImGui::Button(ICON_FA_PLUS " Sonraki X ekle", ImVec2(150.0f, 0.0f))) {
        if (acc_added_offset_ >= acc_parsed_.size()) acc_added_offset_ = 0;
        std::vector<std::size_t> sel;
        const std::size_t start = acc_added_offset_;
        const std::size_t end = std::min<std::size_t>(
            start + static_cast<std::size_t>(acc_count_), acc_parsed_.size());
        for (std::size_t i = start; i < end; ++i) sel.push_back(i);
        acc_added_offset_ = end;
        spawn_indices(sel);
    }
    ImGui::SameLine();
    // All (with the offset reset so a later Add X starts clean).
    if (ImGui::Button(ICON_FA_LAYER_GROUP " Hepsini ekle", ImVec2(150.0f, 0.0f))) {
        std::vector<std::size_t> sel(acc_parsed_.size());
        std::iota(sel.begin(), sel.end(), std::size_t{0});
        acc_added_offset_ = 0;
        spawn_indices(sel);
    }
    ImGui::EndDisabled();

    if (!acc_status_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kAccent, "%s", acc_status_.c_str());
    }

    // Preview of extracted usernames.
    if (!acc_parsed_.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Ayiklanan hesaplar (%zu):", acc_parsed_.size());
        ImGui::BeginChild("##accpreview", ImVec2(0.0f, 0.0f), true);
        for (const auto& a : acc_parsed_) {
            ImGui::TextUnformatted(a.username.c_str());
            if (!a.login_token.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(ltoken)");
            }
        }
        ImGui::EndChild();
    }
}

// ---------------------------------------------------------------------------
// Add-bot form: 3 login modes -> BotManager::spawn_*. (HAR/Requestly removed.)
// ---------------------------------------------------------------------------
void AppUi::DrawAddBotSection() {
    ImGui::SetNextItemWidth(220.0f);
    ImGui::Combo("Login mode", &add_mode_, "Standard\0Newly\0Ltoken\0");

    switch (add_mode_) {
    case 2:  // Ltoken
        ImGui::InputText("Login token", add_ltoken_.data(), add_ltoken_.size());
        break;
    default:  // Standard / Newly
        ImGui::InputText("Username", add_user_.data(), add_user_.size());
        ImGui::InputText("Password", add_pass_.data(), add_pass_.size(),
                         ImGuiInputTextFlags_Password);
        break;
    }

    ImGui::SeparatorText("Proxy");
    ImGui::Checkbox("Use proxy pool", &use_proxy_pool_);
    ImGui::InputText("Proxy host", px_host_.data(), px_host_.size());
    ImGui::InputInt("Proxy port", &px_port_);
    if (px_port_ < 0) px_port_ = 0;
    ImGui::InputText("Proxy user", px_user_.data(), px_user_.size());
    ImGui::InputText("Proxy pass", px_pass_.data(), px_pass_.size(),
                     ImGuiInputTextFlags_Password);
    ImGui::TextDisabled("Leave host/port blank to use the pool (or go direct).");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_USER_PLUS " Add bot", ImVec2(140.0f, 0.0f))) {
        add_error_.clear();
        std::optional<adonai::bot::Socks5Config> proxy;
        std::optional<adonai::proxy::RotatingLoginProxy> login_proxy;
        if (ResolveSpawnProxy(proxy) && ResolveLoginProxy(login_proxy)) {
            std::uint32_t id = 0;
            const std::string user = trimmed(add_user_.data());
            const std::string pass(add_pass_.data());
            switch (add_mode_) {
            case 0:
                id = manager_.spawn(user, pass, std::move(proxy), std::move(login_proxy));
                break;
            case 1:
                id = manager_.spawn_newly(user, pass, std::move(proxy),
                                          std::move(login_proxy));
                break;
            case 2:
                id = manager_.spawn_ltoken(trimmed(add_ltoken_.data()), std::move(proxy),
                                           std::move(login_proxy));
                break;
            }
            selected_bot_ = static_cast<int>(id);
        }
    }
    if (!add_error_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kError, "%s", add_error_.c_str());
    }
}

// ---------------------------------------------------------------------------
// Proxy pool editor (§2.7). Buffers load once from ProxyPool; Save calls
// update(); live utilization from view(proxy_key_counts()).
// ---------------------------------------------------------------------------
void AppUi::LoadProxyEditor() {
    const auto v = proxy_pool_.view(manager_.proxy_key_counts());
    pp_enabled_ = v.enabled;
    pp_max_per_ip_ = static_cast<int>(v.max_bots_per_ip == 0 ? 1 : v.max_bots_per_ip);
    pp_spread_ = (v.spread_mode == "round_robin") ? 1 : 0;
    set_buf(pp_proxies_, v.proxies_text);
    pp_rot_enabled_ = v.rotating_login_enabled;
    set_buf(pp_rot_scheme_,
            v.rotating_login_scheme.empty() ? "auto" : v.rotating_login_scheme);
    pp_rot_port_span_ = v.rotating_login_port_span == 0 ? 2000 : v.rotating_login_port_span;
    set_buf(pp_rot_proxies_, v.rotating_login_proxy_text);
    proxy_loaded_ = true;
}

void AppUi::ApplyProxyConfig() {
    try {
        proxy_pool_.update(
            pp_enabled_, static_cast<std::size_t>(pp_max_per_ip_),
            pp_spread_ == 1 ? "round_robin" : "least_loaded", pp_proxies_.data(),
            pp_rot_enabled_, pp_rot_scheme_.data(),
            static_cast<std::uint16_t>(pp_rot_port_span_), pp_rot_proxies_.data());
        proxy_status_ = "Otomatik kaydedildi.";
        adonai::log("[Proxy] pool config applied (auto-save).");
    } catch (const std::exception& e) {
        proxy_status_ = std::string("Hata: ") + e.what();
    }
}

void AppUi::DrawProxySection() {
    if (!proxy_loaded_)
        LoadProxyEditor();

    // Changes apply live: toggles the moment they flip, text boxes when focus
    // leaves. No Save button - so a config change (e.g. turning rotating login
    // off) is in effect for the very next spawn without an easy-to-forget click.
    bool apply = false;

    if (ImGui::Checkbox("Enable game pool", &pp_enabled_)) apply = true;
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderInt("Max bots per IP", &pp_max_per_ip_, 1, 20);
    if (ImGui::IsItemDeactivatedAfterEdit()) apply = true;
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("Spread mode", &pp_spread_, "least_loaded\0round_robin\0")) apply = true;

    ImGui::TextUnformatted("Game / world proxies (one per line)");
    ImGui::TextDisabled(
        "host:port  ·  host:port:user:pass  ·  sticky: host:10001-10060:user:pass");
    ImGui::InputTextMultiline("##game", pp_proxies_.data(), pp_proxies_.size(),
                              ImVec2(-1.0f, 110.0f));
    if (ImGui::IsItemDeactivatedAfterEdit()) apply = true;

    ImGui::SeparatorText("Rotating / bypass login pool");
    ImGui::TextDisabled(
        "DataImpulse sticky: one exit IP per login attempt (ltoken is bound to it).");
    if (ImGui::Checkbox("Enable rotating login", &pp_rot_enabled_)) apply = true;
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("Login scheme", pp_rot_scheme_.data(), pp_rot_scheme_.size());
    if (ImGui::IsItemDeactivatedAfterEdit()) apply = true;
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputInt("Sticky port span", &pp_rot_port_span_);
    if (pp_rot_port_span_ < 1) pp_rot_port_span_ = 1;
    if (pp_rot_port_span_ > 65535) pp_rot_port_span_ = 65535;
    if (ImGui::IsItemDeactivatedAfterEdit()) apply = true;
    ImGui::InputTextMultiline("##bypass", pp_rot_proxies_.data(), pp_rot_proxies_.size(),
                              ImVec2(-1.0f, 90.0f));
    if (ImGui::IsItemDeactivatedAfterEdit()) apply = true;

    if (apply) ApplyProxyConfig();

    ImGui::Spacing();
    ImGui::TextDisabled("Degisiklikler otomatik uygulanir (Save gerekmez).");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Reload", ImVec2(120.0f, 0.0f))) {
        LoadProxyEditor();
        proxy_status_ = "Diskten yeniden yuklendi.";
    }
    if (!proxy_status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", proxy_status_.c_str());
    }

    // 403 quarantine: proxies a bot got a 403 from are set aside fleet-wide so no
    // bot logs in through them; the bot pulls the next pool proxy automatically.
    auto quarantined = adonai::proxy::quarantined_proxies();
    if (!quarantined.empty()) {
        ImGui::SeparatorText("Karantina (403 / rate-limit - koseye ayrildi)");
        ImGui::TextDisabled(
            "Bu IP'ler 403 verdi ya da oyuna baglanamadi (rate-limit); hicbir bot bunlari "
            "kullanmaz.");
        for (const auto& q : quarantined) ImGui::BulletText("%s", q.c_str());
        if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Karantinayi temizle", ImVec2(210.0f, 0.0f))) {
            adonai::proxy::clear_quarantine();
            proxy_status_ = "Karantina temizlendi.";
        }
    }

    // Live utilization (fleet-wide per-IP counts).
    ImGui::SeparatorText("Utilization");
    const auto v = proxy_pool_.view(manager_.proxy_key_counts());
    ImGui::Text("total %zu · available %zu · active %zu", v.total, v.available, v.active);
    if (ImGui::BeginTable("util", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 140.0f))) {
        ImGui::TableSetupColumn("Proxy");
        ImGui::TableSetupColumn("IP");
        ImGui::TableSetupColumn("Active", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Cap", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();
        for (const auto& p : v.proxies) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(p.label.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(p.ip.c_str());
            ImGui::TableSetColumnIndex(2);
            if (p.full)
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f), "%zu", p.active);
            else
                ImGui::Text("%zu", p.active);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%zu", p.capacity);
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// Automation: fleet-wide module flags on the shared FleetState (every bot sees
// them) + per-bot toggles for the selected bot (SetAutoCollect / etc.).
// ---------------------------------------------------------------------------
void AppUi::DrawAutomationSection() {
    if (FontBold()) ImGui::PushFont(FontBold());
    ImGui::TextColored(kAccent, "Executor - fleet otomasyon");
    if (FontBold()) ImGui::PopFont();
    ImGui::TextWrapped(
        "Motor icindeki native, fleet-farkinda moduller ortak bir FleetState'e karsi "
        "calisir. Modul acikken her tick calisir (ayri start/stop yok). Bu ayarlar TUM "
        "botlara uygulanir; tek bir bot icin bota tiklayip Automation sekmesine bak.");
    ImGui::Separator();
    ImGui::Spacing();

    auto cfg = manager_.fleet()->config_snapshot();

    // Seed the param editor buffers once from the live config.
    if (!auto_params_loaded_) {
        auto_geiger_dig_ = cfg.param("geiger_dig", "1") != "0";
        try {
            auto_collect_radius_ = std::stoi(cfg.param("collect_radius", "3"));
        } catch (...) {
            auto_collect_radius_ = 3;
        }
        set_buf(auto_webhook_url_, cfg.param("webhook_url", ""));
        set_buf(auto_worlds_, cfg.param("coordinate_worlds", ""));
        set_buf(geiger_hunt_, cfg.param("geiger_hunt_worlds", ""));
        set_buf(geiger_depot_, cfg.param("geiger_depot_worlds", ""));
        set_buf(geiger_pickup_, cfg.param("geiger_pickup_worlds", ""));
        set_buf(geiger_webhook_url_, cfg.param("geiger_webhook_url", ""));
        set_buf(geiger_drop_ids_, cfg.param("geiger_drop_ids", ""));
        geiger_wear_ = cfg.param("geiger_wear", "1") != "0";
        try { geiger_item_ = std::stoi(cfg.param("geiger_item", "2204")); } catch (...) { geiger_item_ = 2204; }
        try { geiger_recharge_min_ = std::stoi(cfg.param("geiger_recharge_min", "30")); } catch (...) { geiger_recharge_min_ = 30; }
        auto_params_loaded_ = true;
    }

    bool changed = false;
    ImGui::SeparatorText("Moduller");

    // AutoGeiger runs ONLY on the bots explicitly armed below. A freshly-spawned
    // bot is NEVER auto-armed (no warp-on-spawn): cfg.module_bot_ids["geiger"] is the
    // single source of truth for who runs it (empty scope = nobody). Editing the
    // worlds/params does NOT arm any bot.
    ImGui::SeparatorText("AutoGeiger (geiger farm)");

    std::vector<std::uint32_t> geiger_armed;
    if (auto git = cfg.module_bot_ids.find(kModGeiger); git != cfg.module_bot_ids.end())
        geiger_armed = git->second;

    // --- config (always editable; editing never arms a bot) ------------------
    if (ImGui::CollapsingHeader("Geiger ayarlari (dunyalar / item / webhook)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        ImGui::TextDisabled("Dunya listeleri: satir/virgul ile ayir (WORLD veya WORLD:door)");

        ImGui::TextUnformatted("Hunt (aranacak) dunyalar");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextMultiline("##ghunt", geiger_hunt_.data(), geiger_hunt_.size(),
                                      ImVec2(0.0f, 46.0f))) {
            cfg.params["geiger_hunt_worlds"] = trimmed(geiger_hunt_.data());
            changed = true;
        }
        ImGui::TextUnformatted("Depot (loot birakilacak) dunyalar");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextMultiline("##gdepot", geiger_depot_.data(), geiger_depot_.size(),
                                      ImVec2(0.0f, 46.0f))) {
            cfg.params["geiger_depot_worlds"] = trimmed(geiger_depot_.data());
            changed = true;
        }
        ImGui::TextUnformatted("Pickup (geiger alinacak) dunyalar");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextMultiline("##gpick", geiger_pickup_.data(), geiger_pickup_.size(),
                                      ImVec2(0.0f, 40.0f))) {
            cfg.params["geiger_pickup_worlds"] = trimmed(geiger_pickup_.data());
            changed = true;
        }

        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Geiger item id", &geiger_item_)) {
            if (geiger_item_ < 0) geiger_item_ = 0;
            cfg.params["geiger_item"] = std::to_string(geiger_item_);
            changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Recharge (dk)", &geiger_recharge_min_)) {
            if (geiger_recharge_min_ < 0) geiger_recharge_min_ = 0;
            cfg.params["geiger_recharge_min"] = std::to_string(geiger_recharge_min_);
            changed = true;
        }
        if (ImGui::Checkbox("Geiger'i otomatik giy (wear)", &geiger_wear_)) {
            cfg.params["geiger_wear"] = geiger_wear_ ? "1" : "0";
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Hedef tile'i kaz (dig)", &auto_geiger_dig_)) {
            cfg.params["geiger_dig"] = auto_geiger_dig_ ? "1" : "0";
            changed = true;
        }
        ImGui::TextUnformatted("Depoya SADECE prize itemleri birakilir (dahili liste).");
        ImGui::TextDisabled("Ekstra/ozel drop item id'leri (virgul/bosluk): dahili prize listesine eklenir");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##gdropids", "orn: 2242, 2244, 2246", geiger_drop_ids_.data(),
                                     geiger_drop_ids_.size())) {
            cfg.params["geiger_drop_ids"] = trimmed(geiger_drop_ids_.data());
            changed = true;
        }

        ImGui::TextUnformatted("Geiger webhook (Discord) - fleet ozet, tek mesaj duzenlenir");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##gwh", "https://discord.com/api/webhooks/...",
                                     geiger_webhook_url_.data(), geiger_webhook_url_.size())) {
            cfg.params["geiger_webhook_url"] = trimmed(geiger_webhook_url_.data());
            changed = true;
        }

        // Persist worlds/item/recharge/webhook/enables to data/automation_config.json
        // (survives restart; the per-bot arming is intentionally NOT auto-restored).
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Ayarlari kaydet", ImVec2(200.0f, 0.0f))) {
            adonai::automation::save_automation_config(cfg);
            auto_save_status_ = "Kaydedildi -> data/automation_config.json";
        }
        if (!auto_save_status_.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kAccent, "%s", auto_save_status_.c_str());
        }
        ImGui::Unindent();
    }

    // --- which bots run AutoGeiger (multi-select; Ctrl+click) ----------------
    std::vector<adonai::bot::BotInfo> grows = manager_.list();
    std::sort(grows.begin(), grows.end(),
              [](const adonai::bot::BotInfo& a, const adonai::bot::BotInfo& b) { return a.id < b.id; });
    ImGui::Spacing();
    ImGui::TextUnformatted("Hangi botlarda calissin: tikla = tekli sec, Ctrl+tik = coklu sec.");
    if (ImGui::SmallButton("Tumunu sec")) {
        for (const auto& r : grows) exec_sel_.insert(r.id);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Secimi temizle")) exec_sel_.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu secili  ·  %zu bot geiger-acik", exec_sel_.size(), geiger_armed.size());

    ImGui::BeginChild("geigerbots", ImVec2(0.0f, 150.0f), true);
    for (const auto& r : grows) {
        const bool sel = exec_sel_.count(r.id) > 0;
        const bool is_armed =
            std::find(geiger_armed.begin(), geiger_armed.end(), r.id) != geiger_armed.end();
        std::string lbl = std::to_string(r.id) + "  " + r.username;
        if (is_armed) lbl += "   [GEIGER ACIK]";
        if (!r.world.empty()) lbl += "  @" + r.world;
        ImGui::PushID(static_cast<int>(r.id));
        if (ImGui::Selectable(lbl.c_str(), sel)) {
            if (ImGui::GetIO().KeyCtrl) {
                if (sel) exec_sel_.erase(r.id);
                else exec_sel_.insert(r.id);
            } else {
                exec_sel_.clear();
                exec_sel_.insert(r.id);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    const bool have_sel = !exec_sel_.empty();
    ImGui::BeginDisabled(!have_sel);
    if (ImGui::Button("Secili botlarda AC")) {
        std::vector<std::uint32_t> scope = geiger_armed;
        for (std::uint32_t id : exec_sel_)
            if (std::find(scope.begin(), scope.end(), id) == scope.end()) scope.push_back(id);
        std::sort(scope.begin(), scope.end());
        cfg.enabled[kModGeiger] = true;              // module on
        cfg.module_bot_ids[kModGeiger] = std::move(scope);  // non-empty -> ONLY these run
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Secili botlarda KAPAT")) {
        std::vector<std::uint32_t> scope;
        for (std::uint32_t id : geiger_armed)
            if (!exec_sel_.count(id)) scope.push_back(id);
        if (scope.empty()) cfg.enabled[kModGeiger] = false;  // nobody armed -> module off
        cfg.module_bot_ids[kModGeiger] = std::move(scope);   // empty -> nobody
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Hepsini durdur (geiger)")) {
        cfg.enabled[kModGeiger] = false;
        cfg.module_bot_ids[kModGeiger] = {};  // empty scope = nobody
        changed = true;
    }

    bool collect = cfg.is_on(kModCollect);
    if (ImGui::Checkbox("Auto-collect (fleet)", &collect)) {
        cfg.enabled[kModCollect] = collect;
        changed = true;
    }
    if (collect) {
        ImGui::Indent();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderInt("Collect yaricapi", &auto_collect_radius_, 1, 8)) {
            cfg.params["collect_radius"] = std::to_string(auto_collect_radius_);
            changed = true;
        }
        ImGui::Unindent();
    }

    bool coord = cfg.is_on(kModCoordinate);
    if (ImGui::Checkbox("Fleet coordination (ortak hedefler)", &coord)) {
        cfg.enabled[kModCoordinate] = coord;
        changed = true;
    }
    if (coord) {
        ImGui::Indent();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##worlds", "Dunyalar: WORLDA|door, WORLDB",
                                     auto_worlds_.data(), auto_worlds_.size())) {
            cfg.params["coordinate_worlds"] = trimmed(auto_worlds_.data());
            changed = true;
        }
        ImGui::Unindent();
    }

    bool webhook = cfg.is_on(kModWebhook);
    if (ImGui::Checkbox("Webhook bildirimleri", &webhook)) {
        cfg.enabled[kModWebhook] = webhook;
        changed = true;
    }
    if (webhook) {
        ImGui::Indent();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##wurl", "https://discord.com/api/webhooks/...",
                                     auto_webhook_url_.data(), auto_webhook_url_.size())) {
            cfg.params["webhook_url"] = trimmed(auto_webhook_url_.data());
            changed = true;
        }
        ImGui::Unindent();
    }

    if (changed) manager_.fleet()->set_config(cfg);

    ImGui::Spacing();
    ImGui::Separator();
    std::size_t on_count = 0;
    for (const char* k : {kModGeiger, kModCollect, kModCoordinate, kModWebhook})
        if (cfg.is_on(k)) ++on_count;
    ImGui::TextDisabled("%zu bot  ·  %zu modul aktif", manager_.list().size(), on_count);
}

// ---------------------------------------------------------------------------
// Console: shared ring buffer from core/logger.h, optional per-bot filter.
// ---------------------------------------------------------------------------
void AppUi::DrawConsoleSection() {
    ImGui::Checkbox("Auto-scroll", &console_autoscroll_);
    ImGui::SameLine();
    ImGui::Checkbox("Only selected bot", &console_only_selected_);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Clear"))
        adonai::Logger::Instance().Clear();
    ImGui::Separator();

    ImGui::BeginChild("log", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : adonai::Logger::Instance().Snapshot()) {
        if (console_only_selected_ && line.bot_id != selected_bot_)
            continue;
        if (line.bot_id >= 0)
            ImGui::Text("[Bot#%d] %s", line.bot_id, line.text.c_str());
        else
            ImGui::TextUnformatted(line.text.c_str());
    }
    if (console_autoscroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}


// ===========================================================================
// §3  Per-bot detail (right pane of the Bots tab): header + World/Inventory/Logs
// ===========================================================================
namespace {
// Titled sub-panel (matches the reference's boxed panels): a header strip with a
// centered "icon  title", then content flows below. Pair with PanelEnd().
void PanelBegin(const char* id, const char* icon, const char* title, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kSideBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild(id, size, true);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowSize().x;
    const float hh = ImGui::GetTextLineHeight() + 8.0f;
    dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + hh), ImGui::GetColorU32(kTitleBg));
    dl->AddLine(ImVec2(wp.x, wp.y + hh - 1.0f), ImVec2(wp.x + w, wp.y + hh - 1.0f),
                ImGui::GetColorU32(kAccentDim));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s  %s", icon, title);
    const float tw = ImGui::CalcTextSize(buf).x;
    dl->AddText(ImVec2(wp.x + (w - tw) * 0.5f, wp.y + 4.0f),
                ImGui::GetColorU32(ImVec4(0.75f, 0.85f, 0.97f, 1.0f)), buf);
    ImGui::SetCursorPosY(hh + 6.0f);
}
void PanelEnd() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
// Small dim gear button (settings affordance, for visual parity).
void GearButton(const char* id) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, kTransparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kHoverWhite);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::SmallButton(ICON_FA_GEAR);
    ImGui::PopStyleColor(3);
    ImGui::PopID();
}
}  // namespace

void AppUi::DrawBotDetail() {
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    std::optional<adonai::bot::BotState> snap = manager_.get_state(id);
    if (!snap) {
        ImGui::TextDisabled("bot #%u yok (kaldirilmis).", id);
        return;
    }
    const adonai::bot::BotState& st = *snap;

    // Tight frame padding + resize-down fitting so all seven sub-tabs fit the
    // pane width (shrink to fit instead of scrolling), like the reference.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
    const bool tb = ImGui::BeginTabBar("##botdetailtabs", ImGuiTabBarFlags_FittingPolicyResizeDown);
    ImGui::PopStyleVar();
    if (tb) {
        if (ImGui::BeginTabItem("Main")) { DrawDetailMain(st); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("World")) { DrawMinimapTab(st); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Inventory")) { DrawInventoryTab(st); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Console")) { DrawBotChatTab(st); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Automation")) { DrawDetailAutomation(st); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Rotation")) { DrawDetailRotation(st); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Logs")) { DrawBotLogsTab(st); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

// Main sub-tab: 2x2 panels - Bot Information | Movement / Preferences | Intervals.
void AppUi::DrawDetailMain(const adonai::bot::BotState& st) {
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 6.0f;
    const float colW = (avail.x - gap) * 0.5f;
    const float rowH = (avail.y - gap) * 0.5f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));

    // ---------- Bot Information ----------
    PanelBegin("##pinfo", ICON_FA_USERS, "Bot Information", ImVec2(colW, rowH));
    {
        const std::string status = adonai::bot::to_string(st.status);
        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        ImGui::TextColored(status_color(status), "%s", status.c_str());
        ImGui::Text("Gems: %d     Ping: %u ms", st.gems, st.ping_ms);
        ImGui::Text("World: %s  (%d, %d)",
                    st.world_name.empty() ? "-" : st.world_name.c_str(),
                    static_cast<int>(st.pos_x), static_cast<int>(st.pos_y));
        ImGui::Text("Players: %zu    Items: %zu", st.players.size(), st.inventory.size());
        ImGui::Spacing();

        // World Name + inline Leave/Warp (one row, like the reference), then
        // Connect/Disconnect below - keeps all four buttons inside the panel.
        ImGui::SetNextItemWidth(-116.0f);
        ImGui::InputTextWithHint("##dworld", "World Name", warp_name_.data(),
                                 warp_name_.size());
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::SmallButton("Leave"))
            manager_.send_cmd(id, adonai::bot::cmd::LeaveWorld{});
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::SmallButton("Warp")) {
            std::string w = trimmed(warp_name_.data());
            if (!w.empty()) manager_.send_cmd(id, adonai::bot::cmd::Warp{w, ""});
        }
        const float hw = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (ImGui::Button("Connect", ImVec2(hw, 0.0f)))
            manager_.send_cmd(id, adonai::bot::cmd::Reconnect{});
        ImGui::SameLine();
        if (ImGui::Button("Disconnect", ImVec2(-FLT_MIN, 0.0f)))
            manager_.send_cmd(id, adonai::bot::cmd::Disconnect{});
    }
    PanelEnd();

    ImGui::SameLine();

    // ---------- Movement ----------
    PanelBegin("##pmove", ICON_FA_UP_DOWN_LEFT_RIGHT, "Movement", ImVec2(colW, rowH));
    {
        auto mv = [&](int dx, int dy) {
            manager_.send_cmd(id, adonai::bot::cmd::Move{dx * move_steps_, dy * move_steps_});
        };
        const float bw = 38.0f, bh = 28.0f;
        const float cw = ImGui::GetContentRegionAvail().x;
        float padx = (cw - (bw * 3.0f + gap * 2.0f)) * 0.5f;
        if (padx < 0.0f) padx = 0.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padx + bw + gap);
        if (ImGui::Button(ICON_FA_ARROW_UP, ImVec2(bw, bh))) mv(0, -1);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padx);
        if (ImGui::Button(ICON_FA_ARROW_LEFT, ImVec2(bw, bh))) mv(-1, 0);
        ImGui::SameLine(0.0f, gap);
        if (ImGui::Button(ICON_FA_RIGHT_TO_BRACKET, ImVec2(bw, bh)))
            manager_.send_cmd(id, adonai::bot::cmd::Respawn{});
        ImGui::SameLine(0.0f, gap);
        if (ImGui::Button(ICON_FA_ARROW_RIGHT, ImVec2(bw, bh))) mv(1, 0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padx + bw + gap);
        if (ImGui::Button(ICON_FA_ARROW_DOWN, ImVec2(bw, bh))) mv(0, 1);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ToggleSwitch("##mvdyn", &pref_dyn_delay_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64.0f);
        ImGui::InputInt("##steps", &move_steps_, 0, 0);
        if (move_steps_ < 1) move_steps_ = 1;
        if (move_steps_ > 100) move_steps_ = 100;
        ImGui::SameLine();
        GearButton("##mvgear");
        ImGui::SameLine();
        ImGui::TextDisabled("adim");
    }
    PanelEnd();

    // ---------- Preferences ----------
    PanelBegin("##ppref", ICON_FA_GEAR, "Preferences", ImVec2(colW, rowH));
    {
        const float pcw = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        auto pref_row = [&](const char* pid, const char* label, bool cur, bool gear) -> int {
            ImGui::PushID(pid);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            if (gear) {
                ImGui::SameLine();
                ImGui::SetCursorPosX(pcw - 66.0f);
                GearButton("g");
            }
            ImGui::SameLine();
            ImGui::SetCursorPosX(pcw - 44.0f);
            bool v = cur;
            int result = ToggleSwitch("t", &v) ? (v ? 1 : 0) : -1;
            ImGui::PopID();
            return result;
        };
        if (int r = pref_row("byp", "Bypass Logon", pref_bypass_, true); r >= 0)
            pref_bypass_ = (r != 0);
        if (int r = pref_row("arc", "Auto Reconnect", st.auto_reconnect, true); r >= 0)
            manager_.send_cmd(id, adonai::bot::cmd::SetAutoReconnect{r != 0});
        if (int r = pref_row("acol", "Auto Collect", st.auto_collect, true); r >= 0)
            manager_.send_cmd(id, adonai::bot::cmd::SetAutoCollect{r != 0});
        if (int r = pref_row("aacc", "Auto Accept", pref_auto_accept_, false); r >= 0) {
            pref_auto_accept_ = (r != 0);
            if (r) manager_.send_cmd(id, adonai::bot::cmd::AcceptAccess{});
        }
    }
    PanelEnd();

    ImGui::SameLine();

    // ---------- Intervals ----------
    PanelBegin("##pintv", ICON_FA_CLOCK, "Intervals", ImVec2(colW, rowH));
    {
        const float pcw = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Dynamic Delay");
        ImGui::SameLine();
        ImGui::SetCursorPosX(pcw - 44.0f);
        ToggleSwitch("##dd", &pref_dyn_delay_);

        int mvms = static_cast<int>(st.delays.walk_ms);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##ivmove", &mvms, 0, 2000, "Move  -  %d ms")) {
            adonai::bot::BotDelays d = st.delays;
            d.walk_ms = static_cast<std::uint64_t>(mvms);
            manager_.send_cmd(id, adonai::bot::cmd::SetDelays{d});
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##ivcol", &iv_collect_ms_, 0, 2000, "Collect  -  %d ms");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##ivhit", &iv_hit_ms_, 0, 2000, "Hit  -  %d ms");
        int plms = static_cast<int>(st.delays.place_ms);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##ivplace", &plms, 0, 2000, "Place  -  %d ms")) {
            adonai::bot::BotDelays d = st.delays;
            d.place_ms = static_cast<std::uint64_t>(plms);
            manager_.send_cmd(id, adonai::bot::cmd::SetDelays{d});
        }
    }
    PanelEnd();

    ImGui::PopStyleVar();
}

// Automation sub-tab: per-bot fleet-aware toggles + one-shot actions.
void AppUi::DrawDetailAutomation(const adonai::bot::BotState& st) {
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    const float col = 210.0f;
    ImGui::Spacing();

    // ---- Fleet modules (shared across ALL bots via FleetState; live toggle,
    //      no start/stop - a module ticks whenever its flag is on) ----
    if (FontBold()) ImGui::PushFont(FontBold());
    ImGui::TextColored(kAccent, "Fleet otomasyon (tum botlar)");
    if (FontBold()) ImGui::PopFont();
    ImGui::Spacing();

    auto cfg = manager_.fleet()->config_snapshot();
    bool changed = false;
    auto mod_row = [&](const char* key, const char* label) {
        bool on = cfg.is_on(key);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(col);
        if (ToggleSwitch(key, &on)) {
            cfg.enabled[key] = on;
            changed = true;
        }
    };
    mod_row(kModGeiger, "AutoGeiger (geiger farm)");
    mod_row(kModCollect, "Auto-collect (fleet)");
    mod_row(kModCoordinate, "Fleet coordination");
    mod_row(kModWebhook, "Webhook bildirim");
    {  // geiger dig sub-option
        bool dig = cfg.param("geiger_dig", "1") != "0";
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("    Geiger: hedefi kaz");
        ImGui::SameLine(col);
        if (ToggleSwitch("##gdig", &dig)) {
            cfg.params["geiger_dig"] = dig ? "1" : "0";
            changed = true;
        }
    }
    if (changed) manager_.fleet()->set_config(cfg);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Per-bot settings ----
    if (FontBold()) ImGui::PushFont(FontBold());
    ImGui::TextColored(kAccent, "Bu bot");
    if (FontBold()) ImGui::PopFont();
    ImGui::Spacing();

    bool ac = st.auto_collect;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Auto-collect (bu bot)");
    ImGui::SameLine(col);
    if (ToggleSwitch("##dac", &ac)) manager_.send_cmd(id, adonai::bot::cmd::SetAutoCollect{ac});

    bool ar = st.auto_reconnect;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Auto-reconnect");
    ImGui::SameLine(col);
    if (ToggleSwitch("##dar2", &ar)) manager_.send_cmd(id, adonai::bot::cmd::SetAutoReconnect{ar});

    int radius = st.collect_radius_tiles;
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::SliderInt("Collect yaricapi (tile)", &radius, 1, 5))
        manager_.send_cmd(id, adonai::bot::cmd::SetCollectConfig{
                                  static_cast<std::uint8_t>(radius), st.collect_blacklist});

    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_CIRCLE_CHECK " Access kabul et"))
        manager_.send_cmd(id, adonai::bot::cmd::AcceptAccess{});
}

// Rotation sub-tab: which proxy the bot uses + reconnect (pool-managed rotation).
void AppUi::DrawDetailRotation(const adonai::bot::BotState& st) {
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    ImGui::Spacing();

    std::string proxy = "direct";
    auto it = manager_.bots.find(id);
    if (it != manager_.bots.end() && it->second.proxy_key)
        proxy = *it->second.proxy_key;

    ImGui::Text("Proxy: %s", proxy.c_str());
    ImGui::Text("Auto reconnect: %s", st.auto_reconnect ? "acik" : "kapali");
    ImGui::Text("Ping: %u ms", st.ping_ms);
    ImGui::Spacing();
    ImGui::TextWrapped("Proxy rotasyonu havuz (Proxy sekmesi) tarafindan yonetilir; "
                       "her yeniden baglantida siradaki proxy atanir.");
    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Reconnect"))
        manager_.send_cmd(id, adonai::bot::cmd::Reconnect{});
}

// Logs sub-tab: the shared logger stream filtered to this bot.
void AppUi::DrawBotLoggerTab(const adonai::bot::BotState& st) {
    (void)st;
    ImGui::BeginChild("##botlogger", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : adonai::Logger::Instance().Snapshot()) {
        if (line.bot_id != selected_bot_) continue;
        ImGui::TextUnformatted(line.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// Inventory: one row per slot. Wear/Unwear (by is_active), Drop (1), Drop All
// (amount), Trash (1) -> manager_.send_cmd (mirrors Mori bot-detail buttons).
void AppUi::DrawInventoryTab(const adonai::bot::BotState& st) {
    if (st.inventory.empty()) {
        ImGui::TextDisabled("envanter bos (bot henuz dunyada degil).");
        return;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    std::shared_ptr<const adonai::world::ItemsDat> items = manager_.items_dat();

    // Search box: filter rows by item name or id number (case-insensitive).
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##invsearch", ICON_FA_MAGNIFYING_GLASS " Item ara (isim veya id)",
                             inv_search_.data(), inv_search_.size());
    std::string needle = inv_search_.data();
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::size_t shown = 0;
    ImGui::TextDisabled("%zu slot", st.inventory.size());
    ImGui::Spacing();

    if (ImGui::BeginTable("inv", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("Adet", ImGuiTableColumnFlags_WidthFixed, 52);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 288);
        ImGui::TableHeadersRow();

        for (const auto& slot : st.inventory) {
            const adonai::world::ItemInfo* info =
                items ? items->find_by_id(slot.item_id) : nullptr;
            const std::string display =
                (info && !info->name.empty()) ? info->name
                                              : "#" + std::to_string(slot.item_id);
            // Apply the search filter (match against the name OR the raw id).
            if (!needle.empty()) {
                std::string hay = display;
                std::transform(hay.begin(), hay.end(), hay.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const std::string idstr = std::to_string(slot.item_id);
                if (hay.find(needle) == std::string::npos &&
                    idstr.find(needle) == std::string::npos)
                    continue;
            }
            ++shown;

            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(slot.item_id));

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(display.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", static_cast<unsigned>(slot.amount));

            ImGui::TableSetColumnIndex(2);
            if (slot.is_active) {
                if (ImGui::SmallButton(ICON_FA_SHIRT " Unwear"))
                    manager_.send_cmd(id, adonai::bot::cmd::Unwear{slot.item_id});
            } else {
                if (ImGui::SmallButton(ICON_FA_SHIRT " Wear"))
                    manager_.send_cmd(id, adonai::bot::cmd::Wear{slot.item_id});
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_DOWN_LONG " Drop"))
                manager_.send_cmd(id, adonai::bot::cmd::Drop{slot.item_id, 1});
            ImGui::SameLine();
            if (ImGui::SmallButton("Drop All"))
                manager_.send_cmd(id, adonai::bot::cmd::Drop{slot.item_id, slot.amount});
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_TRASH " Trash"))
                manager_.send_cmd(id, adonai::bot::cmd::Trash{slot.item_id, 1});

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!needle.empty() && shown == 0)
        ImGui::TextDisabled("eslesme yok.");
}

// Colored-block minimap (fg=item-hashed color, bg=dim) + self (green) + others
// (yellow). Coords are in TILES; scaled to fit the content region.
void AppUi::DrawMinimapTab(const adonai::bot::BotState& st) {
    if (st.world_width == 0 || st.world_height == 0 || st.tiles.empty()) {
        ImGui::TextDisabled("dunya verisi yok (bot henuz dunyada degil).");
        return;
    }
    const int W = static_cast<int>(st.world_width);
    const int H = static_cast<int>(st.world_height);

    // Item-id -> real tile colour LUT (items.dat base_color), built once when
    // items are available. Avoids a per-tile linear find_by_id every frame.
    static std::vector<ImU32> lut;
    static bool lut_built = false;
    if (!lut_built) {
        std::shared_ptr<const adonai::world::ItemsDat> items = manager_.items_dat();
        if (items && !items->items.empty()) {
            std::uint32_t maxId = 0;
            for (const auto& it : items->items)
                if (it.id > maxId) maxId = it.id;
            lut.assign(static_cast<std::size_t>(maxId) + 1, 0);
            for (const auto& it : items->items) {
                if (it.base_color != 0) {
                    const std::uint32_t rgb = adonai::world::bgra_to_rgb(it.base_color);
                    lut[it.id] = IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
                }
            }
            lut_built = true;
        }
    }
    auto tile_col = [&](std::uint16_t id) -> ImU32 {
        return (id < lut.size()) ? lut[id] : 0;
    };

    ImGui::TextDisabled("%dx%d  -  " ICON_FA_CIRCLE_CHECK " yesil=bot   sari=oyuncular (%zu)",
                        W, H, st.players.size());
    ImGui::Spacing();

    // Integer pixels-per-tile => crisp, seamless, never sheared.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    int px = static_cast<int>((avail.x / W < avail.y / H) ? (avail.x / W) : (avail.y / H));
    if (px < 1) px = 1;
    if (px > 12) px = 12;
    const float fpx = static_cast<float>(px);
    const float mapW = static_cast<float>(W * px), mapH = static_cast<float>(H * px);

    // Centre the map in the pane.
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const ImVec2 origin(c0.x + std::max(0.0f, (avail.x - mapW) * 0.5f),
                        c0.y + std::max(0.0f, (avail.y - mapH) * 0.5f));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + mapW, origin.y + mapH), IM_COL32(8, 10, 16, 255));

    const std::size_t n = st.tiles.size();
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * W + x;
            if (idx >= n) break;  // short/ragged tile buffer: stop safely
            const adonai::bot::TileInfo& t = st.tiles[idx];
            ImU32 c = 0;
            if (t.fg_item_id != 0) {
                c = tile_col(t.fg_item_id);
                if (c == 0) c = IM_COL32(96, 104, 120, 255);  // solid, colour unknown
            } else if (t.bg_item_id != 0) {
                const ImU32 bc = tile_col(t.bg_item_id);
                c = (bc != 0) ? IM_COL32(static_cast<int>((bc & 0xFF) * 0.45f),
                                         static_cast<int>(((bc >> 8) & 0xFF) * 0.45f),
                                         static_cast<int>(((bc >> 16) & 0xFF) * 0.45f), 255)
                              : IM_COL32(22, 26, 36, 255);  // dim background
            }
            if (c != 0) {
                const ImVec2 p0(origin.x + x * fpx, origin.y + y * fpx);
                dl->AddRectFilled(p0, ImVec2(p0.x + fpx, p0.y + fpx), c);
            }
        }
    }

    // Other players (yellow), then self (green + white ring) on top.
    for (const auto& p : st.players) {
        const ImVec2 pp(origin.x + p.pos_x * fpx, origin.y + p.pos_y * fpx);
        dl->AddCircleFilled(pp, std::max(2.5f, fpx * 0.6f), IM_COL32(240, 190, 60, 255));
    }
    const ImVec2 me(origin.x + st.pos_x * fpx, origin.y + st.pos_y * fpx);
    const float mr = std::max(3.5f, fpx * 0.75f);
    dl->AddCircleFilled(me, mr, IM_COL32(70, 225, 120, 255));
    dl->AddCircle(me, mr + 1.5f, IM_COL32(255, 255, 255, 230), 0, 2.0f);

    ImGui::Dummy(ImVec2(avail.x, mapH));
}

// That bot's console ring buffer, autoscrolled to the tail.
namespace {
// Scrollable per-bot ring view with an auto-scroll toggle + line count. Used by
// both the Console (in-game chat) and Logs (system) detail tabs.
void DrawRingView(const char* id, const std::vector<std::string>& lines,
                  bool& autoscroll, const char* empty_hint) {
    ImGui::Checkbox("Auto-scroll", &autoscroll);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu satir", lines.size());
    ImGui::Separator();
    ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
    if (lines.empty())
        ImGui::TextDisabled("%s", empty_hint);
    for (const auto& line : lines)
        ImGui::TextUnformatted(line.c_str());
    if (autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}
}  // namespace

// Logs tab = this bot's SYSTEM log ring (BotState.console, in memory per bot).
void AppUi::DrawBotLogsTab(const adonai::bot::BotState& st) {
    DrawRingView("##botlogs", st.console, detail_logs_autoscroll_, "(log yok)");
}

// Console tab = this bot's IN-GAME chat/console ring (BotState.chat), distinct
// from the system Logs.
void AppUi::DrawBotChatTab(const adonai::bot::BotState& st) {
    DrawRingView("##botchat", st.chat, detail_chat_autoscroll_,
                 "(dunyaya girince oyun-ici mesajlar burada gorunur)");
}

// ---------------------------------------------------------------------------
// Switch tab: world/warp + respawn/leave + chat for the selected bot.
// ---------------------------------------------------------------------------
void AppUi::DrawSwitchSection() {
    if (FontBold()) ImGui::PushFont(FontBold());
    ImGui::TextColored(kAccent, "Switch - dunya / warp");
    if (FontBold()) ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();

    if (selected_bot_ < 0) {
        ImGui::TextDisabled("Once Bots sekmesinden bir bot secin.");
        return;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);

    std::optional<adonai::bot::BotState> snap = manager_.get_state(id);
    if (snap)
        ImGui::Text("Aktif dunya: %s",
                    snap->world_name.empty() ? "-" : snap->world_name.c_str());
    ImGui::Spacing();

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##warpname", "Dunya adi (orn. START)", warp_name_.data(),
                             warp_name_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputTextWithHint("##warpid", "id (ops.)", warp_id_.data(), warp_id_.size());
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_RIGHT_LEFT " Warp")) {
        std::string w = trimmed(warp_name_.data());
        if (!w.empty())
            manager_.send_cmd(id, adonai::bot::cmd::Warp{w, trimmed(warp_id_.data())});
    }

    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Respawn"))
        manager_.send_cmd(id, adonai::bot::cmd::Respawn{});
    ImGui::SameLine();
    if (ImGui::Button("Leave World"))
        manager_.send_cmd(id, adonai::bot::cmd::LeaveWorld{});

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Sohbet");
    static char say_buf[128] = {0};
    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputTextWithHint("##say", "mesaj", say_buf, sizeof(say_buf));
    ImGui::SameLine();
    if (ImGui::Button("Say")) {
        std::string t = trimmed(say_buf);
        if (!t.empty()) manager_.send_cmd(id, adonai::bot::cmd::Say{t});
    }
}

// ---------------------------------------------------------------------------
// Settings tab: window/app toggles + the shared console (its own filters).
// ---------------------------------------------------------------------------
void AppUi::DrawSettingsSection() {
    if (FontBold()) ImGui::PushFont(FontBold());
    ImGui::TextColored(kAccent, "Settings");
    if (FontBold()) ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();

    bool aot = host_.is_always_on_top();
    if (ImGui::Checkbox("Her zaman ustte", &aot)) host_.set_always_on_top(aot);
    bool lk = host_.is_locked();
    if (ImGui::Checkbox("Pencere kilidi (tasima kapali)", &lk)) host_.set_locked(lk);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (FontBold()) ImGui::PushFont(FontBold());
    ImGui::TextColored(kAccent, "Console");
    if (FontBold()) ImGui::PopFont();
    ImGui::Spacing();
    DrawConsoleSection();
}

}  // namespace adonai::ui
