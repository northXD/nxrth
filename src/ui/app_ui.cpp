#include "ui/app_ui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <numeric>  // std::iota (Random X account pick)
#include <optional>
#include <random>   // std::mt19937 (Random X account pick)
#include <string>
#include <vector>

#include <windows.h>  // MultiByteToWideChar - open UTF-8/non-ASCII paths as wide
#include <commdlg.h>  // GetOpenFileName / GetSaveFileName (Load / Save dialogs)

#include <fstream>

#include "automation/config_store.h"  // save/load the fleet AutomationConfig
#include "core/accounts.h"
#include "core/constants.h"
#include "core/logger.h"
#include "imgui.h"
#include "recovery/fleet_store.h"
#include "ui/icons_fa.h"
#include "ui/theme.h"
#include "world/items.h"  // nxrth::world::ItemsDat (item names for inventory)

namespace nxrth::ui {
namespace {

// Fleet AutomationConfig module keys (match the module->name() the engine
// attaches under src/automation/modules/). Toggling these on the shared
// FleetState is how every bot learns which fleet-aware routines are enabled.
// MUST match the module kName in src/automation/modules/*.h (FleetState config is
// keyed by module name; a mismatch means the toggle never enables the module).
constexpr const char* kModCollect = "collect";
constexpr const char* kModGeiger = "geiger";
constexpr const char* kModCoordinate = "coordinate";

constexpr std::string_view kLuaAddBotTemplate = R"lua(local tokens = [[
PASTE TOKENS HERE
]]

local useProxy = true
local connectBot = true

local function parseTokenLine(line)
    local bot = {}
    for key_value in line:gmatch("[^|]+") do
        local key, value = key_value:match("([^:]+):(.+)")
        if key and value then bot[key] = value end
    end
    return bot
end

local function configureAndAddBot(bot)
    if not bot.token then return end
    bot.connect = connectBot
    if type(calculateBackpackCost) == "function" then
        if useProxy then bot.proxy = "auto" end
        bot.type = TOKEN
    else
        bot.name = bot.token
        bot.platform = tonumber(bot.platform)
    end

    local ok, result = addBot(bot)
    if ok then
        print("Bot has been added: " .. tostring(result))
    else
        print("Bot could not be added: " .. tostring(result))
    end
end

for line in tokens:gmatch("[^\r\n]+") do
    configureAndAddBot(parseTokenLine(line))
end
)lua";

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

// UTF-8 (what ImGui InputText stores) -> UTF-16, so non-ASCII paths (e.g. accented
// or non-Latin folder names) open correctly - a narrow std::ifstream would fail on them.
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

// Native Save-as dialog; returns the chosen path or "" if cancelled.
std::string save_file_dialog(const char* filter, const char* def_ext, const char* def_name) {
    char path[MAX_PATH] = {0};
    if (def_name) std::snprintf(path, sizeof(path), "%s", def_name);
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrDefExt = def_ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    return GetSaveFileNameA(&ofn) ? std::string(path) : std::string();
}

// Native Open dialog; returns the chosen path or "" if cancelled.
std::string open_file_dialog(const char* filter) {
    char path[MAX_PATH] = {0};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? std::string(path) : std::string();
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

// --- Proxy tab (ui4) row identity -------------------------------------------
// A stable key per proxy row: "host:port:user". Used to associate async SOCKS5
// check results (and the current selection) with a parsed proxy line.
std::string proxy_row_key(const nxrth::proxy::ProxyEntry& e) {
    return e.host + ":" + std::to_string(e.port) + ":" + e.username.value_or("");
}

// --- Database tab (ui5) items.dat enum name maps -----------------------------
// items.dat stores these as small integers; Growtopia's own editors show them
// with a friendly label + the raw number. Unknown values fall back to the number.
const char* collision_type_name(std::uint8_t t) {
    switch (t) {
        case 0: return "none";
        case 1: return "solid";
        case 2: return "jump-through";
        case 3: return "water";
        case 4: return "adventure";
        case 5: return "climbable";
        case 6: return "vip";
        case 7: return "waterfall";
        default: return "type";
    }
}
const char* clothing_type_name(std::uint8_t t) {
    switch (t) {
        case 0: return "hat";
        case 1: return "shirt";
        case 2: return "pants";
        case 3: return "feet";
        case 4: return "face";
        case 5: return "hand";
        case 6: return "back";
        case 7: return "hair";
        case 8: return "chest";
        case 9: return "necklace";
        case 10: return "ances";
        default: return "none";
    }
}
// Growtopia's item action/behaviour type (a.k.a. the "type" column). Only the
// common values are named; anything else shows as "type".
const char* action_type_name(std::uint8_t t) {
    switch (t) {
        case 0: return "fist";
        case 1: return "wearable";
        case 2: return "consumable";
        case 3: return "gems";
        case 7: return "door";
        case 8: return "portal";
        case 9: return "background";
        case 10: return "seed";
        case 11: return "clothes";
        case 13: return "sign";
        case 17: return "foreground";
        case 18: return "main door";
        case 20: return "platform";
        case 22: return "sfx foreground";
        case 23: return "lock";
        case 33: return "provider";
        case 47: return "component";
        default: return "type";
    }
}

}  // namespace

AppUi::AppUi(nxrth::bot::BotManager& manager, nxrth::proxy::ProxyPool& proxy_pool,
             nxrth::ui::IWindowHost& host)
    : manager_(manager), proxy_pool_(proxy_pool), host_(host),
      lua_engine_(std::make_unique<nxrth::lua::LuaEngine>(manager, proxy_pool)),
      script_store_(std::make_unique<nxrth::script::ScriptStore>()),
      ai_mcp_(std::make_unique<nxrth::mcp::McpServer>(manager, proxy_pool, true)),
      ai_controller_(std::make_unique<nxrth::ai::AiController>(
          nxrth::mcp::McpServer::tool_definitions(),
          [this](const std::string& name, const nlohmann::json& arguments) {
              const nlohmann::json request = {
                  {"jsonrpc", "2.0"},
                  {"id", ai_rpc_id_++},
                  {"method", "tools/call"},
                  {"params", {{"name", name}, {"arguments", arguments}}},
              };
              const auto response = ai_mcp_->handle(request);
              if (!response)
                  return nlohmann::json{{"isError", true},
                                        {"error", "MCP tool returned no response"}};
              if (response->contains("error")) {
                  const auto& rpc_error = response->at("error");
                  return nlohmann::json{
                      {"isError", true},
                      {"error", rpc_error.value("message", "MCP tool failed")},
                  };
              }
              auto result = response->value("result", nlohmann::json::object());
              if (result.contains("structuredContent") &&
                  !result["structuredContent"].is_null()) {
                  auto data = result["structuredContent"];
                  if (result.value("isError", false) && data.is_object())
                      data["isError"] = true;
                  return data;
              }
              return result;
          })),
      lua_source_(nxrth::script::ScriptStore::kMaxScriptBytes + 1, '\0'),
      ai_prompt_(256 * 1024 + 1, '\0') {
    set_buf(pp_rot_scheme_, "auto");
    set_buf(lua_script_name_, "script.lua");
    set_buf(ai_model_, "gpt-5.4-mini");
    std::memcpy(lua_source_.data(), kLuaAddBotTemplate.data(), kLuaAddBotTemplate.size());
}

AppUi::~AppUi() {
    if (ai_controller_) {
        ai_controller_->cancel();
        ai_controller_.reset();
    }
    ai_mcp_.reset();
    SecureZeroMemory(ai_api_key_.data(), ai_api_key_.size());
    // Stop the background SOCKS5 checker before our members are destroyed.
    proxy_check_cancel_.store(true);
    if (proxy_check_thread_.joinable()) proxy_check_thread_.join();
}

const std::vector<nxrth::bot::BotInfo>& AppUi::CachedBotList() {
    const auto now = std::chrono::steady_clock::now();
    if (now >= bot_list_refresh_) {
        bot_list_cache_ = manager_.list();
        bot_list_refresh_ = now + std::chrono::milliseconds(100);
    }
    return bot_list_cache_;
}

const nxrth::bot::BotState* AppUi::CachedBotState(std::uint32_t id) {
    const auto now = std::chrono::steady_clock::now();
    if (id != bot_state_cache_id_ || now >= bot_state_refresh_) {
        bot_state_cache_ = manager_.get_state(id);
        bot_state_cache_id_ = id;
        bot_state_refresh_ = now + std::chrono::milliseconds(100);
    }
    return bot_state_cache_ ? &*bot_state_cache_ : nullptr;
}

// ---------------------------------------------------------------------------
// Frame root: a single borderless window that fills the whole (fixed) viewport.
// No docking, no menu bar, no extra ImGui windows.
// ---------------------------------------------------------------------------
void AppUi::Draw() {
    // AI HTTP work completes on a worker, but every app/MCP mutation is pumped
    // here on the BotManager owner thread.
    if (ai_controller_) ai_controller_->pump();

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
    ImGui::Begin("##nxrth_root", nullptr, flags);
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
// Title bar (~30px): left dropdown chevron, centered "Nxrth v.. [GT ..]" title,
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
        ImGui::TextDisabled("Nxrth v%s - GT %s", nxrth::constants::APP_VERSION.data(),
                            nxrth::constants::GAME_VER.data());
        ImGui::Separator();
        if (ImGui::MenuItem("Always on top", nullptr, host_.is_always_on_top()))
            host_.set_always_on_top(!host_.is_always_on_top());
        if (ImGui::MenuItem("Lock window", nullptr, host_.is_locked()))
            host_.set_locked(!host_.is_locked());
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) host_.request_close();
        ImGui::EndPopup();
    }

    // Right: red window controls.
    ImGui::SetCursorPos(ImVec2(ww - kNBtn * kBtnW, 0.0f));
    if (TitleIconButton("top", ICON_FA_UP_DOWN_LEFT_RIGHT, kBtnW, kH, kRed, kHoverWhite,
                        "Always on top"))
        host_.set_always_on_top(!host_.is_always_on_top());
    ImGui::SameLine(0.0f, 0.0f);
    if (TitleIconButton("lock", host_.is_locked() ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN,
                        kBtnW, kH, kRed, kHoverWhite, "Lock window"))
        host_.set_locked(!host_.is_locked());
    ImGui::SameLine(0.0f, 0.0f);
    if (TitleIconButton("min", ICON_FA_WINDOW_MINIMIZE, kBtnW, kH, kRed, kHoverWhite,
                        "Minimize"))
        host_.minimize();
    ImGui::SameLine(0.0f, 0.0f);
    // Fixed-size window: the maximize glyph is shown for parity only (no-op).
    TitleIconButton("max", ICON_FA_EXPAND, kBtnW, kH, kRed, kHoverWhite, "Fixed size");
    ImGui::SameLine(0.0f, 0.0f);
    if (TitleIconButton("close", ICON_FA_POWER_OFF, kBtnW, kH, kRed, kDanger, "Close"))
        host_.request_close();

    // Centered title text (non-interactive).
    char title[128];
    std::snprintf(title, sizeof(title), "Nxrth v%s  [GT %s]    discord.gg/nxrth",
                  nxrth::constants::APP_VERSION.data(),
                  nxrth::constants::GAME_VER.data());
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
    // Anchor the drag to the absolute screen cursor: capture the offset on grab,
    // then track it. Moving by ImGui's per-frame (client-relative) MouseDelta made
    // the window jitter, because the window slid out from under the cursor.
    if (!host_.is_locked()) {
        if (ImGui::IsItemActivated()) host_.begin_drag();
        if (ImGui::IsItemActive()) host_.drag_update();
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Horizontal top tab strip. Active tab = solid blue with white text; others
// transparent + dim.
// ---------------------------------------------------------------------------
void AppUi::DrawTabBar() {
    struct Tab { const char* icon; const char* label; };
    static const Tab kTabs[] = {
        {ICON_FA_ROBOT, "Bots"},        {ICON_FA_LIST, "List"},
        {ICON_FA_CODE, "Executor"},     {ICON_FA_GLOBE, "Proxy"},
        {ICON_FA_DATABASE, "Database"}, {ICON_FA_GEAR, "Settings"},
        {ICON_FA_ROBOT, "AI"},
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
    case 4: DrawAccountsSection(); break; // Database (items.dat + accounts)
    case 5: DrawSettingsSection(); break; // Settings (console + toggles)
    case 6: DrawAiSection(); break;        // Built-in AI operator
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
    const auto& rows = CachedBotList();
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
    std::vector<std::uint32_t> visible;  // filtered ids, for Select-All
    for (const auto& r : rows) {
        std::string uname = r.username.empty() ? "(token)" : r.username;
        if (!filter.empty()) {
            std::string low = uname;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (low.find(filter) == std::string::npos) continue;
        }
        ++shown;
        visible.push_back(r.id);
        // Highlighted if it's the focused bot OR (in multi-select) part of the set.
        const bool sel = (selected_bot_ == static_cast<int>(r.id)) ||
                         (multi_select_ && bots_sel_.count(r.id) != 0);
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "%s##b%u", uname.c_str(), r.id);
        ImGui::PushStyleColor(ImGuiCol_Text, status_color(r.status));
        if (ImGui::Selectable(lbl, sel)) {
            if (multi_select_ && ImGui::GetIO().KeyCtrl) {
                // Windows-style Ctrl+click: toggle just this row.
                if (bots_sel_.count(r.id)) bots_sel_.erase(r.id);
                else bots_sel_.insert(r.id);
            } else {
                // Plain click: single selection (drop the multi set).
                bots_sel_.clear();
                if (multi_select_) bots_sel_.insert(r.id);
            }
            selected_bot_ = static_cast<int>(r.id);
        }
        ImGui::PopStyleColor();
    }
    if (shown == 0)
        ImGui::TextDisabled(rows.empty() ? "(no bots)" : "(no matches)");
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Select-All: fill/clear the multi-selection on toggle; otherwise keep the
    // toggle reflecting whether every visible row is selected.
    if (select_all_ != prev_select_all_) {
        prev_select_all_ = select_all_;
        bots_sel_.clear();
        if (select_all_) {
            multi_select_ = true;
            for (std::uint32_t id : visible) bots_sel_.insert(id);
        }
    } else if (multi_select_) {
        bool all = !visible.empty();
        for (std::uint32_t id : visible)
            if (!bots_sel_.count(id)) { all = false; break; }
        select_all_ = prev_select_all_ = all;
    } else if (!bots_sel_.empty()) {
        bots_sel_.clear();  // multi-select off -> drop the extra selection
    }

    // Search.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search for bots..", bot_search_.data(),
                             bot_search_.size());

    // Load | Save.
    const float half = (kLeftW - 4.0f) * 0.5f;
    if (ImGui::Button("Load", ImVec2(half, 0.0f)))
        LoadBotRecords();
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(-FLT_MIN, 0.0f)))
        SaveBotRecords();
    if (!bots_status_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", bots_status_.c_str());
        ImGui::PopStyleColor();
    }

    // Add Bot / Remove Bot (both plain dark, like the reference).
    if (ImGui::Button("Add Bot", ImVec2(-FLT_MIN, 0.0f)))
        open_add_popup_ = true;
    {
        const bool multi = multi_select_ && !bots_sel_.empty();
        const std::size_t nsel = multi ? bots_sel_.size() : (selected_bot_ >= 0 ? 1u : 0u);
        const bool has = nsel > 0;
        if (!has) ImGui::BeginDisabled();
        char rlbl[32];
        std::snprintf(rlbl, sizeof(rlbl), nsel > 1 ? "Remove %zu Bots" : "Remove Bot", nsel);
        if (ImGui::Button(rlbl, ImVec2(-FLT_MIN, 0.0f))) {
            if (multi) {
                for (std::uint32_t id : bots_sel_) manager_.stop(id);
                bots_sel_.clear();
                select_all_ = prev_select_all_ = false;
            } else if (selected_bot_ >= 0) {
                manager_.stop(static_cast<std::uint32_t>(selected_bot_));
            }
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
        ImGui::TextDisabled("Select a bot on the left for details");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- Add Bot popup -----------------------------------------------------
    if (open_add_popup_) {
        ImGui::OpenPopup("Add Bot##popup");
        open_add_popup_ = false;
    }
    if (ImGui::BeginPopupModal("Add Bot##popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        DrawAddBotSection();
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// List tab: full-width fleet table (one row per BotManager::list() BotInfo).
// Clicking a row selects the bot and jumps to the Bots tab's detail pane.
// ---------------------------------------------------------------------------
void AppUi::DrawListSection() {
    std::vector<nxrth::bot::BotInfo> rows = CachedBotList();
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });

    // id -> proxy label (BotEntry.proxy_key is the resolved "ip:port" or none).
    std::map<std::uint32_t, std::string> proxy_of;
    for (const auto& [id, entry] : manager_.bots)
        proxy_of[id] = entry.proxy_key.value_or("direct");

    ImGui::Text("%zu bot", rows.size());
    ImGui::SameLine();
    ImGui::TextDisabled(ICON_FA_CIRCLE_CHECK " green=online  " ICON_FA_GEAR
                        " orange=login  " ICON_FA_CIRCLE_XMARK
                        " red=stopped  -  click a bot -> details");
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
bool AppUi::ResolveSpawnProxy(std::optional<nxrth::bot::Socks5Config>& out) {
    out.reset();
    const std::string custom = trimmed(add_proxy_.data());
    if (!custom.empty()) {
        // Custom proxy: "host:port" or "host:port:user:pass".
        std::vector<std::string> f;
        std::size_t start = 0;
        for (;;) {
            const std::size_t c = custom.find(':', start);
            if (c == std::string::npos) { f.push_back(custom.substr(start)); break; }
            f.push_back(custom.substr(start, c - start));
            start = c + 1;
        }
        int port = 0;
        try { port = std::stoi(f.size() > 1 ? f[1] : std::string()); } catch (...) { port = 0; }
        if (f.size() < 2 || f[0].empty() || port <= 0 || port > 65535) {
            add_error_ = "custom proxy must be host:port[:user:pass]";
            return false;
        }
        nxrth::bot::Socks5Config cfg;
        cfg.host = f[0];
        cfg.port = static_cast<std::uint16_t>(port);
        if (f.size() >= 4) {
            if (!f[2].empty()) cfg.username = f[2];
            if (!f[3].empty()) cfg.password = f[3];
        }
        out = std::move(cfg);
        return true;
    }
    // No custom proxy -> assign one from the pool.
    try {
        out = proxy_pool_.choose(manager_.proxy_key_counts());
    } catch (const std::exception& e) {
        add_error_ = e.what();
        return false;
    }
    if (!out) {
        add_error_ = "game proxy pool is disabled or unavailable; enter a custom proxy";
        return false;
    }
    return true;
}

bool AppUi::ResolveLoginProxy(std::optional<nxrth::proxy::RotatingLoginProxy>& out) {
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
// Database tab (ui5): read-only items.dat viewer. Left = the item list; right =
// the selected item's metadata + a small Search panel (live filter + Display
// Seeds / Null toggles). Item data is the fleet-shared std::shared_ptr<const
// ItemsDat> loaded once by the BotManager.
// ---------------------------------------------------------------------------
void AppUi::DrawAccountsSection() {
    const auto& dat = manager_.items_dat();
    if (!dat || dat->items.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled(
            "items.dat not loaded - place items.dat next to Nxrth.exe (or in ./data).");
        ImGui::TextWrapped(
            "The Database tab reads Growtopia item metadata (id, flags, type, collision, "
            "clothing, rarity, growth) straight from items.dat.");
        return;
    }
    const auto& items = dat->items;

    // Lowercased search query (empty = no name filter).
    std::string q = trimmed(db_search_.data());
    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto is_seed = [](const nxrth::world::ItemInfo& it) {
        // Growtopia seeds pair with a block and their name ends in "Seed".
        const std::string& n = it.name;
        return n.size() >= 4 && (n.compare(n.size() - 4, 4, "Seed") == 0 ||
                                 n.compare(n.size() - 4, 4, "seed") == 0);
    };

    // Filtered index list (Display Seeds / Display Null Items + search).
    std::vector<int> filtered;
    filtered.reserve(items.size());
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const auto& it = items[i];
        if (it.name.empty() && !db_display_null_) continue;
        if (!db_display_seeds_ && is_seed(it)) continue;
        if (!q.empty()) {
            std::string low = it.name;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (low.find(q) == std::string::npos) continue;
        }
        filtered.push_back(i);
    }
    // Auto Update Items: live-follow the first match as the search text changes.
    if (db_auto_update_ && !q.empty() && !filtered.empty())
        db_sel_item_ = static_cast<int>(items[filtered.front()].id);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 6.0f;
    const float leftW = (avail.x - gap) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));

    // ---------- LEFT: Items list ----------
    PanelBegin("##dbitems", ICON_FA_LIST, "Items", ImVec2(leftW, avail.y));
    {
        ImGui::BeginChild("##dbitemlist", ImVec2(0.0f, 0.0f), false);
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(filtered.size()));
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                const auto& it = items[filtered[row]];
                const bool sel = (db_sel_item_ == static_cast<int>(it.id));
                const std::string label =
                    it.name.empty() ? ("Item #" + std::to_string(it.id)) : it.name;
                char lbl[96];
                std::snprintf(lbl, sizeof(lbl), "%s##it%u", label.c_str(), it.id);
                if (it.name.empty())
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                if (ImGui::Selectable(lbl, sel)) db_sel_item_ = static_cast<int>(it.id);
                if (it.name.empty()) ImGui::PopStyleColor();
            }
        }
        if (filtered.empty()) ImGui::TextDisabled("(no items match)");
        ImGui::EndChild();
    }
    PanelEnd();

    // ---------- RIGHT: Item Description (top) + Search (bottom) ----------
    ImGui::SameLine();
    ImGui::BeginChild("##dbright", ImVec2(0.0f, avail.y), false);
    {
        const float descH = (avail.y - gap) * 0.60f;
        const nxrth::world::ItemInfo* it =
            db_sel_item_ >= 0 ? dat->find_by_id(static_cast<std::uint32_t>(db_sel_item_))
                              : nullptr;
        if (!it && !filtered.empty()) {
            it = &items[filtered.front()];
            db_sel_item_ = static_cast<int>(it->id);
        }

        PanelBegin("##dbdesc", ICON_FA_LIST, "Item Description", ImVec2(0.0f, descH));
        if (it) {
            auto centered = [](const std::string& s) {
                const float w = ImGui::GetContentRegionAvail().x;
                const float tw = ImGui::CalcTextSize(s.c_str()).x;
                if (tw < w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - tw) * 0.5f);
                ImGui::TextUnformatted(s.c_str());
            };
            auto named = [](const char* name, unsigned v) {
                return std::string(name) + " (" + std::to_string(v) + ")";
            };
            centered("ID | " + std::to_string(it->id));
            centered("Flags | " + std::to_string(it->flags));
            centered("Type | " + named(action_type_name(it->action_type), it->action_type));
            centered("Collision | " +
                     named(collision_type_name(it->collision_type), it->collision_type));
            centered("Clothing Type | " +
                     named(clothing_type_name(it->clothing_type), it->clothing_type));
            centered("Strength | " + std::to_string(it->block_health));
            centered("Rarity | " + std::to_string(it->rarity));
            centered("Growth | " + std::to_string(it->grow_time) + "s");
            // items.dat has no explicit "level" field; surface the material byte here.
            centered("Level | " + std::to_string(it->material));

            ImGui::Spacing();
            const float bw = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
            if (ImGui::Button(ICON_FA_FLOPPY_DISK " Copy ID", ImVec2(bw, 0.0f))) {
                ImGui::SetClipboardText(std::to_string(it->id).c_str());
                db_copy_status_ = "Copied id " + std::to_string(it->id);
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS " View Item Sprite",
                              ImVec2(-FLT_MIN, 0.0f))) {
                db_copy_status_ =
                    it->texture_file_name.empty()
                        ? "no sprite sheet for this item"
                        : (it->texture_file_name + "  @ (" + std::to_string(it->texture_x) +
                           "," + std::to_string(it->texture_y) + ")");
            }
            if (!db_copy_status_.empty())
                ImGui::TextColored(kAccent, "%s", db_copy_status_.c_str());
        } else {
            ImGui::TextDisabled("Select an item on the left.");
        }
        PanelEnd();

        // ---------- Search panel ----------
        PanelBegin("##dbsearch", ICON_FA_MAGNIFYING_GLASS, "Search", ImVec2(0.0f, 0.0f));
        auto toggle_row = [](const char* label, bool* v) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 52.0f);
            ToggleSwitch(label, v);
        };
        toggle_row("Auto Update Items", &db_auto_update_);
        toggle_row("Display Seeds", &db_display_seeds_);
        toggle_row("Display Null Items", &db_display_null_);
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##dbsearchbox", "Search for items.", db_search_.data(),
                                 db_search_.size());
        PanelEnd();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Bulk import: paste or load accounts_stats.json (or user:pass lines), extract
// every account, then bulk-spawn them (each gets a pool proxy). Folded into the
// Add-bot popup as a collapsible section.
// ---------------------------------------------------------------------------
void AppUi::DrawBulkImport() {
    ImGui::TextWrapped(
        "Paste accounts_stats.json (or user:pass lines) or load from a file; "
        "add all with one click. Each bot is assigned a proxy from the pool.");
    ImGui::Spacing();

    // Load from a file path (parses the FULL file, not the preview box).
    ImGui::SetNextItemWidth(-160.0f);
    ImGui::InputTextWithHint("##accpath", "C:\\...\\accounts_stats.json", acc_path_.data(),
                             acc_path_.size());
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_LAYER_GROUP " Extract from file", ImVec2(150.0f, 0.0f))) {
        const std::string path = strip_quotes(trimmed(acc_path_.data()));
        std::string content;
        if (read_text_file(path, content)) {  // handles quotes + non-ASCII paths
            acc_parsed_ = nxrth::core::parse_account_stats(content);
            acc_added_offset_ = 0;  // fresh list -> reset the "Add X" cursor
            set_buf(acc_text_, content.size() < acc_text_.size()
                                   ? content
                                   : content.substr(0, acc_text_.size() - 1));
            acc_status_ = std::to_string(acc_parsed_.size()) + " accounts extracted";
        } else {
            acc_status_ = "could not open file: " + path;
        }
    }

    ImGui::TextUnformatted("or paste:");
    ImGui::InputTextMultiline("##acctext", acc_text_.data(), acc_text_.size(),
                              ImVec2(-1.0f, 120.0f));
    if (ImGui::Button(ICON_FA_LAYER_GROUP " Extract pasted")) {
        acc_parsed_ = nxrth::core::parse_account_stats(std::string(acc_text_.data()));
        acc_added_offset_ = 0;  // fresh list -> reset the "Add X" cursor
        acc_status_ = std::to_string(acc_parsed_.size()) + " accounts extracted";
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("Login mode", &acc_mode_, "Standard\0Ltoken (OAuth / Provider)\0");
    ImGui::SameLine();
    ImGui::Checkbox("Pool proxy", &acc_use_pool_);

    ImGui::BeginDisabled(acc_parsed_.empty());

    // Shared spawn helper: spawns the accounts at the given indices (into
    // acc_parsed_), honouring the pool-proxy rule (skip rather than leak the real
    // IP when the pool is exhausted). Used by First X / Random X / Add X / All.
    auto spawn_indices = [&](const std::vector<std::size_t>& idxs) {
        int added = 0, skipped = 0, missing_tokens = 0;
        for (std::size_t i : idxs) {
            if (i >= acc_parsed_.size()) continue;
            const auto& a = acc_parsed_[i];
            std::optional<nxrth::bot::LtokenRecord> ltoken_record;
            if (acc_mode_ == 1) {
                ltoken_record = nxrth::bot::parse_ltoken_string(a.login_token);
                if (!ltoken_record) {
                    ++missing_tokens;
                    continue;
                }
            }
            std::optional<nxrth::bot::Socks5Config> proxy;
            std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy;
            if (acc_use_pool_) {
                try {
                    proxy = proxy_pool_.choose(manager_.proxy_key_counts());
                } catch (...) {
                }
                if (acc_mode_ == 0 ||
                    (ltoken_record && ltoken_record->kind ==
                                          nxrth::bot::LtokenRecord::Kind::ProviderToken)) {
                    try {
                        login_proxy = proxy_pool_.rotating_login_proxy();
                    } catch (...) {
                    }
                }
                // Pool ran out: spawning proxy-less would login via the REAL IP
                // (24h ban). Skip instead of leaking.
                if (!proxy && (acc_mode_ == 1 || !login_proxy)) {
                    ++skipped;
                    continue;
                }
            }
            if (acc_mode_ == 1) {
                manager_.spawn_ltoken(
                    a.login_token, std::move(proxy), std::move(login_proxy),
                    acc_use_pool_ ? nxrth::bot::ProxyPolicy::Pool
                                  : nxrth::bot::ProxyPolicy::Direct);
            } else {
                manager_.spawn(
                    a.username, a.password, std::move(proxy), std::move(login_proxy),
                    acc_use_pool_ ? nxrth::bot::ProxyPolicy::Pool
                                  : nxrth::bot::ProxyPolicy::Direct);
            }
            ++added;
        }
        acc_status_ = std::to_string(added) + " bots added";
        if (skipped > 0)
            acc_status_ += ", " + std::to_string(skipped) + " skipped (proxy pool exhausted)";
        if (missing_tokens > 0)
            acc_status_ += ", " + std::to_string(missing_tokens) + " invalid ltoken";
    };

    // "X" count for the First/Random/Add buttons: load only a SUBSET of the parsed
    // accounts (e.g. the first 10 of 711) instead of all of them.
    if (acc_count_ < 1) acc_count_ = 1;
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("Count (X)", &acc_count_);
    if (acc_count_ < 1) acc_count_ = 1;
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu extracted)", acc_parsed_.size());

    // First X: accounts [0, X).
    if (ImGui::Button(ICON_FA_USER_PLUS " Add first X", ImVec2(150.0f, 0.0f))) {
        std::vector<std::size_t> sel;
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(acc_count_),
                                                    acc_parsed_.size());
        for (std::size_t i = 0; i < n; ++i) sel.push_back(i);
        spawn_indices(sel);
    }
    ImGui::SameLine();
    // Random X: X distinct accounts drawn at random.
    if (ImGui::Button(ICON_FA_USER_PLUS " Add random X", ImVec2(165.0f, 0.0f))) {
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
    if (ImGui::Button(ICON_FA_PLUS " Add next X", ImVec2(150.0f, 0.0f))) {
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
    if (ImGui::Button(ICON_FA_LAYER_GROUP " Add all", ImVec2(150.0f, 0.0f))) {
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
        ImGui::TextDisabled("Extracted accounts (%zu):", acc_parsed_.size());
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
// Add-bot form (ui3): one field per value so any seller's delivery format fits.
// The credential field accepts a GrowID, a Mail, a raw token, OR a full keyed
// record (mac:..|wk:..|..|token:..|vid:..). All records are refresh tokens
// validated through checktoken (see login flow).
// ---------------------------------------------------------------------------
void AppUi::DrawAddBotSection() {
    const float w = 300.0f;
    auto field = [&](const char* id, const char* hint, char* buf, std::size_t n, bool pw) {
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint(id, hint, buf, n,
                                 pw ? ImGuiInputTextFlags_Password : ImGuiInputTextFlags_None);
    };
    field("##cred", "Enter GrowID / Mail / Token..", add_cred_.data(), add_cred_.size(), false);
    field("##pass", "Enter password..", add_pass_.data(), add_pass_.size(), true);
    field("##mac", "Enter Mac.. [Optional]", add_mac_.data(), add_mac_.size(), false);
    field("##rid", "Enter Rid.. [Optional]", add_rid_.data(), add_rid_.size(), false);
    field("##hash", "Enter Hash.. [Optional]", add_hash_.data(), add_hash_.size(), false);
    field("##otp", "Enter OTP Secret.. [Optional]", add_otp_.data(), add_otp_.size(), false);
    field("##cproxy", "Enter Custom Proxy.. [Optional]", add_proxy_.data(), add_proxy_.size(), false);
    ImGui::SetNextItemWidth(w);
    ImGui::Combo("##platform", &add_platform_, "Windows\0Android\0iOS\0macOS\0Linux\0");

    if (!add_error_.empty()) ImGui::TextColored(kError, "%s", add_error_.c_str());

    if (ImGui::Button("Add", ImVec2(w, 0.0f))) {
        add_error_.clear();
        SpawnFromAddForm();
    }

    // Bulk import (accounts_stats.json / user:pass lists) - collapsed by default
    // so the single-add form stays clean, but many bots can still be added here.
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Bulk import from file / paste")) DrawBulkImport();
}

// Spawn a bot from the current Add-bot form fields. Shared by the Add button and
// the Bots-tab Load (which populates the fields from a saved pipe record first).
void AppUi::SpawnFromAddForm() {
    const std::string cred = trimmed(add_cred_.data());
    const std::string pass(add_pass_.data());
    const std::string mac = trimmed(add_mac_.data());
    const std::string rid = trimmed(add_rid_.data());
    const std::string hash = trimmed(add_hash_.data());
    const std::string otp = trimmed(add_otp_.data());
    if (cred.empty()) {
        add_error_ = "Enter a GrowID / Mail / Token.";
        return;
    }
    std::optional<nxrth::bot::Socks5Config> proxy;
    if (!ResolveSpawnProxy(proxy)) return;
    const auto proxy_policy = !trimmed(add_proxy_.data()).empty()
                                  ? nxrth::bot::ProxyPolicy::Custom
                                  : (proxy ? nxrth::bot::ProxyPolicy::Pool
                                           : nxrth::bot::ProxyPolicy::Direct);

    std::uint32_t id = 0;
    if (cred.find('|') != std::string::npos || cred.find(':') != std::string::npos) {
        // A full keyed / positional token record pasted into the credential field.
        const auto parsed = nxrth::bot::parse_ltoken_string(cred);
        if (!parsed) {
            add_error_ = "Invalid token record.";
            return;
        }
        std::optional<nxrth::proxy::RotatingLoginProxy> lp;
        if (parsed->kind == nxrth::bot::LtokenRecord::Kind::ProviderToken && !ResolveLoginProxy(lp))
            return;
        id = manager_.spawn_ltoken(cred, std::move(proxy), std::move(lp), proxy_policy);
    } else if (!pass.empty()) {
        // GrowID / Mail + password (legacy dashboard login).
        std::optional<nxrth::proxy::RotatingLoginProxy> lp;
        if (!ResolveLoginProxy(lp)) return;
        id = manager_.spawn(cred, pass, std::move(proxy), std::move(lp), proxy_policy);
    } else if (!mac.empty() && rid.size() == 32) {
        // Bare token + device fields -> assemble a provider record.
        const std::string rec = "token:" + cred + "|rid:" + rid + "|mac:" + mac + "|wk:NONE0";
        if (!nxrth::bot::parse_ltoken_string(rec)) {
            add_error_ = "Invalid token / device fields.";
            return;
        }
        std::optional<nxrth::proxy::RotatingLoginProxy> lp;
        if (!ResolveLoginProxy(lp)) return;
        id = manager_.spawn_ltoken(rec, std::move(proxy), std::move(lp), proxy_policy);
    } else {
        add_error_ = "Enter a password (GrowID/Mail) or Mac + Rid (token).";
        return;
    }
    selected_bot_ = static_cast<int>(id);
    add_cred_.fill('\0');
    add_pass_.fill('\0');
    add_otp_.fill('\0');
}

// Save the manager's authoritative launch catalog. Protected .fleet files keep
// exact secrets under Windows DPAPI; legacy .txt is an explicit plaintext export.
void AppUi::SaveBotRecords() {
    if (manager_.launch_records().empty()) {
        bots_status_ = "No bots to save.";
        return;
    }
    const std::string path =
        save_file_dialog("Protected fleet (*.fleet)\0*.fleet\0Legacy plaintext (*.txt)\0*.txt\0",
                         "fleet", "bots.fleet");
    if (path.empty()) return;
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto format = extension == ".txt"
                            ? nxrth::recovery::FleetBackupFormat::LegacyText
                            : nxrth::recovery::FleetBackupFormat::Protected;
    const auto result = nxrth::recovery::FleetStore::save_to_path(
        std::filesystem::path(path), format, manager_);
    if (!result.ok) {
        bots_status_ = "Save failed: " + result.error;
        return;
    }
    bots_status_ = "Saved " + std::to_string(result.bot_count) + " bot(s)";
    if (format == nxrth::recovery::FleetBackupFormat::Protected)
        bots_status_ += " (protected, exact).";
    else
        bots_status_ += result.lossy_record_count == 0
                            ? " (plaintext)."
                            : " (plaintext; some policies were lossy).";
}

// Load exact local records without surfacing their contents in the UI/logs.
void AppUi::LoadBotRecords() {
    const std::string path = open_file_dialog(
        "Fleet backups (*.fleet;*.txt)\0*.fleet;*.txt\0Protected fleet (*.fleet)\0*.fleet\0Legacy plaintext (*.txt)\0*.txt\0");
    if (path.empty()) return;
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto format = extension == ".txt"
                            ? nxrth::recovery::FleetBackupFormat::LegacyText
                            : nxrth::recovery::FleetBackupFormat::Protected;
    nxrth::recovery::FleetLoadOptions options;
    options.replace_existing = false;
    options.restore_automation = false;
    const auto result = nxrth::recovery::FleetStore::load_from_path(
        std::filesystem::path(path), format, manager_, proxy_pool_, options);
    if (result.spawned_count == 0 && !result.ok) {
        bots_status_ = "Load failed: " +
                       (result.error.empty() ? std::string("no bot could be restored")
                                             : result.error);
        return;
    }
    if (!result.new_bot_ids.empty()) selected_bot_ = static_cast<int>(result.new_bot_ids.back());
    bot_list_refresh_ = {};
    bots_status_ = "Loaded " + std::to_string(result.spawned_count) + " bot(s).";
    if (!result.issues.empty())
        bots_status_ += " " + std::to_string(result.issues.size()) + " skipped.";
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
    pp_shuffle_ = v.shuffle_selection;
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
            pp_spread_ == 1 ? "round_robin" : "least_loaded", pp_shuffle_,
            pp_proxies_.data(),
            pp_rot_enabled_, pp_rot_scheme_.data(),
            static_cast<std::uint16_t>(pp_rot_port_span_), pp_rot_proxies_.data());
        proxy_status_ = "Auto-saved.";
        nxrth::log("[Proxy] pool config applied (auto-save).");
    } catch (const std::exception& e) {
        proxy_status_ = std::string("Error: ") + e.what();
    }
}

// Proxy tab (ui4): two segmented sub-tabs (Socks5 game pool / Logon Bypass pool),
// each a live table over the corresponding pool text buffer. Add/Remove/Load/Save
// edit the buffer (auto-applied); Check Proxies runs an async SOCKS5 handshake
// probe; the GT column shows the result, and "Remove Invalid Proxies" drops the
// ones that failed. Pool behaviour lives behind the Settings button.
void AppUi::DrawProxySection() {
    if (!proxy_loaded_) LoadProxyEditor();

    const float segW = (ImGui::GetContentRegionAvail().x - 4.0f) * 0.5f;
    auto seg = [&](const char* label, int idx) {
        const bool on = (proxy_subtab_ == idx);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? kAccent : kSideBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? kAccent : kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1)
                                               : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button(label, ImVec2(segW, 0.0f))) proxy_subtab_ = idx;
        ImGui::PopStyleColor(4);
    };
    seg(ICON_FA_GLOBE "  Socks5 Proxies", 0);
    ImGui::SameLine();
    seg(ICON_FA_ARROWS_ROTATE "  Logon Bypass Proxies", 1);
    ImGui::Spacing();

    if (proxy_subtab_ == 0)
        DrawProxyTable(pp_proxies_, /*game_pool=*/true);
    else
        DrawProxyTable(pp_rot_proxies_, /*game_pool=*/false);
}

// Kick off an async SOCKS5 reachability check for every proxy in `buffer_text`.
// The UI thread only parses (no DNS); a small worker pool does the blocking
// handshakes and writes results into proxy_check_status_ under the mutex.
void AppUi::StartProxyCheck(const std::string& buffer_text) {
    if (proxy_checking_.load()) return;

    std::vector<nxrth::proxy::ProxyEntry> entries;
    try {
        entries = nxrth::proxy::parse_proxy_lines(buffer_text);
    } catch (...) {
    }
    std::vector<std::pair<std::string, nxrth::proxy::Socks5Config>> work;
    work.reserve(entries.size());
    for (const auto& e : entries) {
        nxrth::proxy::Socks5Config cfg;
        cfg.host = e.host;
        cfg.port = e.port;
        cfg.username = e.username;
        cfg.password = e.password;
        work.emplace_back(proxy_row_key(e), std::move(cfg));
    }
    if (work.empty()) return;

    {
        std::lock_guard<std::mutex> lk(proxy_check_mu_);
        for (const auto& w : work) proxy_check_status_[w.first] = 0;  // 0 = checking
    }
    if (proxy_check_thread_.joinable()) proxy_check_thread_.join();
    proxy_check_cancel_.store(false);
    proxy_check_done_.store(0);
    proxy_check_total_.store(static_cast<int>(work.size()));
    proxy_checking_.store(true);
    proxy_check_thread_ = std::thread([this, work = std::move(work)]() mutable {
        std::atomic<std::size_t> next{0};
        const unsigned n = static_cast<unsigned>(std::min<std::size_t>(work.size(), 12));
        auto worker = [this, &work, &next]() {
            for (;;) {
                if (proxy_check_cancel_.load()) return;
                const std::size_t i = next.fetch_add(1);
                if (i >= work.size()) return;
                const bool ok = nxrth::proxy::probe_socks5(work[i].second);
                {
                    std::lock_guard<std::mutex> lk(proxy_check_mu_);
                    proxy_check_status_[work[i].first] = ok ? 1 : -1;
                }
                proxy_check_done_.fetch_add(1);
            }
        };
        std::vector<std::thread> pool;
        pool.reserve(n);
        for (unsigned k = 0; k < n; ++k) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
        proxy_checking_.store(false);
    });
}

// One sub-tab's table + action bar. `buffer` is the pool text this tab edits
// (pp_proxies_ for the game pool, pp_rot_proxies_ for the bypass pool).
void AppUi::DrawProxyTable(std::array<char, 8192>& buffer, bool game_pool) {
    // Parse the buffer into display rows. parse_proxy_lines throws on a bad line,
    // so fall back to per-line parsing (skipping bad ones) to stay crash-safe.
    std::vector<nxrth::proxy::ProxyEntry> entries;
    try {
        entries = nxrth::proxy::parse_proxy_lines(buffer.data());
    } catch (...) {
        const std::string all(buffer.data());
        std::size_t start = 0;
        while (start <= all.size()) {
            const std::size_t nl = all.find('\n', start);
            const std::string line =
                all.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            try {
                if (auto e = nxrth::proxy::parse_optional_proxy(line)) entries.push_back(*e);
            } catch (...) {
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }

    const auto view = proxy_pool_.view(manager_.proxy_key_counts());

    std::string q = trimmed(proxy_search_.data());
    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Snapshot the async check results once for this frame.
    std::unordered_map<std::string, int> status;
    {
        std::lock_guard<std::mutex> lk(proxy_check_mu_);
        status = proxy_check_status_;
    }
    auto status_of = [&](const std::string& key) -> int {
        auto f = status.find(key);
        return f == status.end() ? 2 : f->second;  // 2 = never checked
    };

    // "Remove Invalid Proxies" — the only status we track (SOCKS5 connect/auth fail).
    int invalid = 0;
    for (const auto& e : entries)
        if (status_of(proxy_row_key(e)) == -1) ++invalid;
    ImGui::BeginDisabled(invalid == 0);
    char rinv[48];
    std::snprintf(rinv, sizeof(rinv), ICON_FA_TRASH " Remove Invalid Proxies (%d)", invalid);
    if (ImGui::Button(rinv)) {
        std::string rebuilt;
        for (const auto& e : entries) {
            if (status_of(proxy_row_key(e)) == -1) continue;
            rebuilt += e.raw;
            rebuilt += "\n";
        }
        set_buf(buffer, rebuilt);
        ApplyProxyConfig();
    }
    ImGui::EndDisabled();
    if (proxy_checking_.load()) {
        ImGui::SameLine();
        ImGui::TextDisabled("checking %d / %d...", proxy_check_done_.load(),
                            proxy_check_total_.load());
    }

    // ---- table ----
    const float bottomH = 70.0f;  // reserve room for "Total" + the button bar
    const ImVec2 tsz(0.0f, ImGui::GetContentRegionAvail().y - bottomH);
    const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    std::size_t shown = 0;
    if (ImGui::BeginTable("##proxytbl", 9, tf, tsz)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("IP Address");
        ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("Username");
        ImGui::TableSetupColumn("Password");
        ImGui::TableSetupColumn("Country", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("Socks5", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("HTTP", ImGuiTableColumnFlags_WidthFixed, 42);
        ImGui::TableSetupColumn("GT", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            const std::string key = proxy_row_key(e);
            if (!q.empty()) {
                std::string hay = e.host + " " + e.username.value_or("");
                std::transform(hay.begin(), hay.end(), hay.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (hay.find(q) == std::string::npos) continue;
            }
            ++shown;
            const std::string scheme =
                e.scheme ? *e.scheme : nxrth::proxy::infer_proxy_scheme_from_port(e.port);
            const bool is_socks = scheme.rfind("socks", 0) == 0;
            const bool is_http = scheme.rfind("http", 0) == 0;
            const int st = status_of(key);
            std::size_t count = 0;
            if (game_pool && i < view.proxies.size()) count = view.proxies[i].active;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char rid[80];
            std::snprintf(rid, sizeof(rid), "%s##pr%zu", e.host.c_str(), i);
            if (ImGui::Selectable(rid, proxy_sel_key_ == key,
                                  ImGuiSelectableFlags_SpanAllColumns))
                proxy_sel_key_ = key;
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", static_cast<unsigned>(e.port));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(e.username ? e.username->c_str() : "-");
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(e.password ? "****" : "-");
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted("-");  // country: no GeoIP DB
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%zu", count);
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(is_socks ? "yes" : "-");
            ImGui::TableSetColumnIndex(7);
            ImGui::TextUnformatted(is_http ? "yes" : "-");
            ImGui::TableSetColumnIndex(8);
            if (st == 2)
                ImGui::TextDisabled("-");
            else if (st == 0)
                ImGui::TextDisabled("...");
            else if (st == 1)
                ImGui::TextColored(ImVec4(0.32f, 0.85f, 0.42f, 1.0f), "%s", ICON_FA_CIRCLE_CHECK);
            else
                ImGui::TextColored(kError, "%s", ICON_FA_CIRCLE_XMARK);
        }
        ImGui::EndTable();
    }

    ImGui::Text("Total Proxies: %zu", entries.size());

    // ---- action bar: Add | Remove | Load | Save | Check | Settings | Search ----
    if (ImGui::Button(ICON_FA_PLUS " Add")) {
        proxy_add_buf_.fill('\0');
        open_proxy_add_ = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(proxy_sel_key_.empty());
    if (ImGui::Button(ICON_FA_TRASH " Remove")) {
        std::string rebuilt;
        for (const auto& e : entries) {
            if (proxy_row_key(e) == proxy_sel_key_) continue;
            rebuilt += e.raw;
            rebuilt += "\n";
        }
        set_buf(buffer, rebuilt);
        proxy_sel_key_.clear();
        ApplyProxyConfig();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Load")) {
        const std::string path =
            open_file_dialog("Proxy list (*.txt)\0*.txt\0All files\0*.*\0");
        std::string content;
        if (!path.empty() && read_text_file(path, content)) {
            set_buf(buffer, content);
            ApplyProxyConfig();
            proxy_status_ = "Loaded proxy list.";
        } else if (!path.empty()) {
            proxy_status_ = "Load failed.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save")) {
        const std::string path =
            save_file_dialog("Proxy list (*.txt)\0*.txt\0All files\0*.*\0", "txt",
                             game_pool ? "proxies.txt" : "bypass.txt");
        if (!path.empty()) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (out) {
                out << buffer.data();
                proxy_status_ = "Saved proxy list.";
            } else {
                proxy_status_ = "Save failed.";
            }
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(proxy_checking_.load() || entries.empty());
    if (ImGui::Button(ICON_FA_CIRCLE_CHECK " Check Proxies")) StartProxyCheck(buffer.data());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_GEAR " Settings")) open_proxy_settings_ = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##proxysearch", "Search for proxy..", proxy_search_.data(),
                             proxy_search_.size());
    if (!proxy_status_.empty()) ImGui::TextDisabled("%s", proxy_status_.c_str());

    // ---- Add popup ----
    if (open_proxy_add_) {
        ImGui::OpenPopup("Add Proxy##pp");
        open_proxy_add_ = false;
    }
    if (ImGui::BeginPopupModal("Add Proxy##pp", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("host:port   or   host:port:user:pass");
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputTextWithHint("##ppadd", "1.2.3.4:1080:user:pass", proxy_add_buf_.data(),
                                 proxy_add_buf_.size());
        const std::string line = trimmed(proxy_add_buf_.data());
        if (ImGui::Button("Add", ImVec2(120.0f, 0.0f)) && !line.empty()) {
            bool ok = false;
            try {
                ok = nxrth::proxy::parse_optional_proxy(line).has_value();
            } catch (...) {
                ok = false;
            }
            if (ok) {
                std::string cur(buffer.data());
                if (!cur.empty() && cur.back() != '\n') cur += "\n";
                cur += line;
                cur += "\n";
                set_buf(buffer, cur);
                ApplyProxyConfig();
                proxy_status_ = "Added proxy.";
                ImGui::CloseCurrentPopup();
            } else {
                proxy_status_ = "Invalid proxy line.";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- Settings popup (pool behaviour + 403 quarantine) ----
    if (open_proxy_settings_) {
        ImGui::OpenPopup("Proxy Settings##pp");
        open_proxy_settings_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Proxy Settings##pp", nullptr, ImGuiWindowFlags_NoResize)) {
        bool apply = false;
        ImGui::SeparatorText("Game / world pool");
        if (ImGui::Checkbox("Enable game pool", &pp_enabled_)) apply = true;
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderInt("Max bots per IP", &pp_max_per_ip_, 1, 20);
        if (ImGui::IsItemDeactivatedAfterEdit()) apply = true;
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("Spread mode", &pp_spread_, "least_loaded\0round_robin\0")) apply = true;
        if (ImGui::Checkbox("Shuffle proxy selection", &pp_shuffle_)) apply = true;

        ImGui::SeparatorText("Logon bypass pool");
        ImGui::TextDisabled("One clean exit IP per login attempt (ltoken binds to it).");
        if (ImGui::Checkbox("Enable rotating login", &pp_rot_enabled_)) apply = true;
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Login scheme", pp_rot_scheme_.data(), pp_rot_scheme_.size());
        if (ImGui::IsItemDeactivatedAfterEdit()) apply = true;
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputInt("Sticky port span", &pp_rot_port_span_);
        if (pp_rot_port_span_ < 1) pp_rot_port_span_ = 1;
        if (pp_rot_port_span_ > 65535) pp_rot_port_span_ = 65535;
        if (ImGui::IsItemDeactivatedAfterEdit()) apply = true;
        if (apply) ApplyProxyConfig();

        auto quarantined = nxrth::proxy::quarantined_proxies();
        if (!quarantined.empty()) {
            ImGui::SeparatorText("Quarantine (403 / rate-limit)");
            ImGui::TextDisabled("These IPs are set aside fleet-wide; no bot will use them.");
            for (const auto& qq : quarantined) ImGui::BulletText("%s", qq.c_str());
            if (ImGui::Button("Clear quarantine")) {
                nxrth::proxy::clear_quarantine();
                proxy_status_ = "Quarantine cleared.";
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    (void)shown;
}

// ---------------------------------------------------------------------------
// Automation: fleet-wide module flags on the shared FleetState (every bot sees
// them) + per-bot toggles for the selected bot (SetAutoCollect / etc.).
// ---------------------------------------------------------------------------
void AppUi::SyncAutomationEditor(
    const std::shared_ptr<const nxrth::bot::AutomationConfig>& source) {
    if (auto_params_loaded_ && auto_params_source_ == source) return;

    const auto& cfg = *source;
    auto_geiger_dig_ = cfg.param("geiger_dig", "1") != "0";
    try {
        auto_collect_radius_ = std::stoi(cfg.param("collect_radius", "3"));
    } catch (...) {
        auto_collect_radius_ = 3;
    }
    set_buf(auto_worlds_, cfg.param("coordinate_worlds", ""));
    set_buf(geiger_hunt_, cfg.param("geiger_hunt_worlds", ""));
    set_buf(geiger_depot_, cfg.param("geiger_depot_worlds", ""));
    set_buf(geiger_pickup_, cfg.param("geiger_pickup_worlds", ""));
    set_buf(geiger_webhook_url_, cfg.param("geiger_webhook_url", ""));
    set_buf(geiger_drop_ids_, cfg.param("geiger_drop_ids", ""));
    geiger_wear_ = cfg.param("geiger_wear", "1") != "0";
    try { geiger_item_ = std::stoi(cfg.param("geiger_item", "2204")); }
    catch (...) { geiger_item_ = 2204; }
    try { geiger_recharge_min_ = std::stoi(cfg.param("geiger_recharge_min", "30")); }
    catch (...) { geiger_recharge_min_ = 30; }
    try { geiger_signal_wait_ms_ = std::stoi(cfg.param("geiger_signal_wait_ms", "4200")); }
    catch (...) { geiger_signal_wait_ms_ = 4200; }
    try { geiger_settle_ms_ = std::stoi(cfg.param("geiger_settle_ms", "700")); }
    catch (...) { geiger_settle_ms_ = 700; }
    try { geiger_max_steps_ = std::stoi(cfg.param("geiger_max_steps", "70")); }
    catch (...) { geiger_max_steps_ = 70; }
    try { geiger_pickup_scan_ms_ = std::stoi(cfg.param("geiger_pickup_scan_ms", "3000")); }
    catch (...) { geiger_pickup_scan_ms_ = 3000; }
    try {
        geiger_pickup_empty_scans_ =
            std::stoi(cfg.param("geiger_pickup_empty_scans", "12"));
    } catch (...) {
        geiger_pickup_empty_scans_ = 12;
    }
    auto_params_source_ = source;
    auto_params_loaded_ = true;
}

// The Executor and every bot-detail Automation tab call this exact editor.
// All fields map directly to the single FleetState AutomationConfig; selecting
// another bot therefore never creates or loads a per-bot copy of the worlds.
bool AppUi::DrawSharedAutomationEditor(nxrth::bot::AutomationConfig& cfg, bool compact) {
    bool changed = false;

    ImGui::SeparatorText("AutoGeiger (shared settings)");
    if (ImGui::CollapsingHeader("Geiger settings (worlds / item / webhook)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        ImGui::TextDisabled("World lists: separate by line/comma (WORLD or WORLD:door)");

        ImGui::TextUnformatted("Hunt (to search) worlds");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextMultiline("##ghunt", geiger_hunt_.data(), geiger_hunt_.size(),
                                      ImVec2(0.0f, 46.0f))) {
            cfg.params["geiger_hunt_worlds"] = trimmed(geiger_hunt_.data());
            changed = true;
        }
        ImGui::TextUnformatted("Depot (drop loot) worlds");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextMultiline("##gdepot", geiger_depot_.data(), geiger_depot_.size(),
                                      ImVec2(0.0f, 46.0f))) {
            cfg.params["geiger_depot_worlds"] = trimmed(geiger_depot_.data());
            changed = true;
        }
        ImGui::TextUnformatted("Pickup (grab geiger) worlds");
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
        if (!compact) ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Recharge (min)", &geiger_recharge_min_)) {
            if (geiger_recharge_min_ < 0) geiger_recharge_min_ = 0;
            cfg.params["geiger_recharge_min"] = std::to_string(geiger_recharge_min_);
            changed = true;
        }
        if (ImGui::Checkbox("Auto-wear geiger (wear)", &geiger_wear_)) {
            cfg.params["geiger_wear"] = geiger_wear_ ? "1" : "0";
            changed = true;
        }
        if (!compact) ImGui::SameLine();
        if (ImGui::Checkbox("Dig target tile (dig)", &auto_geiger_dig_)) {
            cfg.params["geiger_dig"] = auto_geiger_dig_ ? "1" : "0";
            changed = true;
        }

        ImGui::SeparatorText("Search and pickup timing");
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Signal wait (ms)", &geiger_signal_wait_ms_, 0)) {
            geiger_signal_wait_ms_ = std::max(500, geiger_signal_wait_ms_);
            cfg.params["geiger_signal_wait_ms"] = std::to_string(geiger_signal_wait_ms_);
            changed = true;
        }
        if (!compact) ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::InputInt("Settle (ms)", &geiger_settle_ms_, 0)) {
            geiger_settle_ms_ = std::max(0, geiger_settle_ms_);
            cfg.params["geiger_settle_ms"] = std::to_string(geiger_settle_ms_);
            changed = true;
        }
        if (!compact) ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputInt("Max steps", &geiger_max_steps_, 0)) {
            geiger_max_steps_ = std::max(1, geiger_max_steps_);
            cfg.params["geiger_max_steps"] = std::to_string(geiger_max_steps_);
            changed = true;
        }
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Pickup scan (ms)", &geiger_pickup_scan_ms_, 0)) {
            geiger_pickup_scan_ms_ = std::max(500, geiger_pickup_scan_ms_);
            cfg.params["geiger_pickup_scan_ms"] = std::to_string(geiger_pickup_scan_ms_);
            changed = true;
        }
        if (!compact) ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::InputInt("Empty scan limit", &geiger_pickup_empty_scans_, 0)) {
            geiger_pickup_empty_scans_ = std::max(1, geiger_pickup_empty_scans_);
            cfg.params["geiger_pickup_empty_scans"] =
                std::to_string(geiger_pickup_empty_scans_);
            changed = true;
        }

        ImGui::TextUnformatted("ONLY prize items are dropped at the depot (built-in list).");
        ImGui::TextDisabled("Extra/custom drop item ids (comma/space): added to the built-in prize list");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##gdropids", "e.g. 2242, 2244, 2246",
                                     geiger_drop_ids_.data(), geiger_drop_ids_.size())) {
            cfg.params["geiger_drop_ids"] = trimmed(geiger_drop_ids_.data());
            changed = true;
        }

        ImGui::TextUnformatted("Geiger webhook (Discord) - fleet summary, a single message is edited");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##gwh", "https://discord.com/api/webhooks/...",
                                     geiger_webhook_url_.data(), geiger_webhook_url_.size())) {
            cfg.params["geiger_webhook_url"] = trimmed(geiger_webhook_url_.data());
            changed = true;
        }

        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save shared settings",
                          ImVec2(compact ? -FLT_MIN : 220.0f, 0.0f))) {
            nxrth::automation::save_automation_config(cfg);
            auto_save_status_ = "Saved -> data/automation_config.json";
        }
        if (!auto_save_status_.empty()) {
            if (!compact) ImGui::SameLine();
            ImGui::TextColored(kAccent, "%s", auto_save_status_.c_str());
        }
        ImGui::Unindent();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Other shared modules");
    bool collect = cfg.is_on(kModCollect);
    if (ImGui::Checkbox("Auto-collect (fleet)", &collect)) {
        cfg.enabled[kModCollect] = collect;
        changed = true;
    }
    if (collect) {
        ImGui::Indent();
        ImGui::SetNextItemWidth(compact ? -FLT_MIN : 220.0f);
        if (ImGui::SliderInt("Collect radius", &auto_collect_radius_, 1, 8)) {
            cfg.params["collect_radius"] = std::to_string(auto_collect_radius_);
            changed = true;
        }
        ImGui::Unindent();
    }

    bool coord = cfg.is_on(kModCoordinate);
    if (ImGui::Checkbox("Fleet coordination (shared targets)", &coord)) {
        cfg.enabled[kModCoordinate] = coord;
        changed = true;
    }
    if (coord) {
        ImGui::Indent();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##worlds", "Worlds: WORLDA|door, WORLDB",
                                     auto_worlds_.data(), auto_worlds_.size())) {
            cfg.params["coordinate_worlds"] = trimmed(auto_worlds_.data());
            changed = true;
        }
        ImGui::Unindent();
    }

    return changed;
}

void AppUi::DrawAutomationSection() {
    // The Executor tab is the Lua 5.4 script editor only. Fleet automation is
    // configured per bot from the bot-detail "Automation" sub-tab.
    DrawLuaExecutor();
}

void AppUi::DrawLuaExecutor() {
    if (FontBold()) ImGui::PushFont(FontBold());
    ImGui::TextColored(kAccent, "Lua 5.4 executor");
    if (FontBold()) ImGui::PopFont();

    // "Run on" target list: fleet (run once), one entry per bot, or all bots. In
    // "all bots" mode the script runs once per bot with the global SELECTED_BOT set
    // to that bot's id, guaranteeing it executes on every bot.
    const auto& blist = CachedBotList();
    std::vector<std::string> tlabels;
    std::vector<long> tcodes;
    tlabels.emplace_back("Fleet (run once)");
    tcodes.push_back(-1);
    for (const auto& b : blist) {
        tlabels.push_back("Bot " + std::to_string(b.id) + "  " +
                          (b.username.empty() ? std::string("(token)") : b.username));
        tcodes.push_back(static_cast<long>(b.id));
    }
    if (!blist.empty()) {
        tlabels.emplace_back("All bots (run per bot)");
        tcodes.push_back(-2);
    }
    int tcur = 0;
    for (std::size_t i = 0; i < tcodes.size(); ++i)
        if (tcodes[i] == lua_run_target_) { tcur = static_cast<int>(i); break; }
    lua_run_target_ = tcodes[tcur];  // snap to a valid code if a bot vanished

    if (ImGui::Button(ICON_FA_PLAY " Run", ImVec2(90.0f, 0.0f))) {
        const std::size_t length = std::strlen(lua_source_.data());
        const std::string source(lua_source_.data(), length);
        std::vector<std::optional<std::uint32_t>> targets;
        if (lua_run_target_ == -1) {
            targets.emplace_back(std::nullopt);            // fleet: run once
        } else if (lua_run_target_ == -2) {
            for (const auto& b : blist) targets.emplace_back(b.id);  // once per bot
            if (targets.empty()) targets.emplace_back(std::nullopt);
        } else {
            targets.emplace_back(static_cast<std::uint32_t>(lua_run_target_));  // one bot
        }
        std::string agg;
        bool all_ok = true;
        std::size_t added = 0;
        std::string fail_line;
        const bool multi = targets.size() > 1;
        for (const auto& t : targets) {
            nxrth::lua::LuaExecutionOptions opt;
            opt.selected_bot = t;
            auto result = lua_engine_->execute(source, opt);
            all_ok = all_ok && result.ok;
            added += result.added_bot_ids.size();
            if (multi)
                agg += "=== bot " + (t ? std::to_string(*t) : std::string("fleet")) + " ===\n";
            agg += result.output;
            if (!agg.empty() && agg.back() != '\n') agg.push_back('\n');
            if (!result.error.empty()) {
                agg += ">> Error: " + result.error + "\n";
                if (fail_line.empty()) {
                    const std::size_t lp = result.error.find("line ");
                    if (lp != std::string::npos) {
                        std::size_t e = lp + 5;
                        std::string num;
                        while (e < result.error.size() &&
                               std::isdigit(static_cast<unsigned char>(result.error[e])))
                            num.push_back(result.error[e++]);
                        if (!num.empty()) fail_line = " - line " + num;
                    }
                }
            }
        }
        lua_last_ok_ = all_ok;
        lua_output_ = std::move(agg);
        lua_status_ = all_ok ? "Completed" : ("Failed" + fail_line);
        if (added)
            lua_status_ += " - " + std::to_string(added) + " bot(s) accepted";
        bot_list_refresh_ = {};
    }
    ImGui::SameLine();
    {
        std::vector<const char*> citems;
        citems.reserve(tlabels.size());
        for (auto& s : tlabels) citems.push_back(s.c_str());
        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::Combo("##lua_target", &tcur, citems.data(), static_cast<int>(citems.size())))
            lua_run_target_ = tcodes[tcur];
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Run on: which bot(s) the script targets.\n"
                "Scripts see a global SELECTED_BOT (this bot's id, or false for fleet).\n"
                "Use it as a selector, e.g. bot.warp(SELECTED_BOT, \"WORLD\").\n"
                "\"All bots\" runs the whole script once per bot.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputTextWithHint("##lua_script_name", "script.lua", lua_script_name_.data(),
                             lua_script_name_.size());
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save")) {
        std::string error;
        const std::size_t length = std::strlen(lua_source_.data());
        lua_last_ok_ = script_store_->write(trimmed(lua_script_name_.data()),
                                            std::string_view(lua_source_.data(), length), &error);
        lua_status_ = lua_last_ok_ ? "Script saved" : ("Save failed: " + error);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Load")) {
        std::string error;
        auto source = script_store_->read_exact(trimmed(lua_script_name_.data()), &error);
        lua_last_ok_ = source.has_value() && source->size() < lua_source_.size();
        if (lua_last_ok_) {
            std::fill(lua_source_.begin(), lua_source_.end(), '\0');
            std::memcpy(lua_source_.data(), source->data(), source->size());
            lua_status_ = "Script loaded";
        } else {
            lua_status_ = "Load failed: " +
                          (source ? std::string("script is too large for the editor") : error);
        }
    }

    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Import")) {
        const std::string path = open_file_dialog("Lua script (*.lua)\0*.lua\0All files\0*.*\0");
        if (!path.empty()) {
            std::string error;
            lua_last_ok_ = script_store_->import_file(
                std::filesystem::path(path), trimmed(lua_script_name_.data()), &error);
            lua_status_ = lua_last_ok_ ? "Script imported" : ("Import failed: " + error);
            if (lua_last_ok_) {
                auto source = script_store_->read_exact(trimmed(lua_script_name_.data()), &error);
                if (source && source->size() < lua_source_.size()) {
                    std::fill(lua_source_.begin(), lua_source_.end(), '\0');
                    std::memcpy(lua_source_.data(), source->data(), source->size());
                }
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Export")) {
        const std::string path = save_file_dialog(
            "Lua script (*.lua)\0*.lua\0All files\0*.*\0", "lua", lua_script_name_.data());
        if (!path.empty()) {
            std::string error;
            lua_last_ok_ = script_store_->export_file(
                trimmed(lua_script_name_.data()), std::filesystem::path(path), &error);
            lua_status_ = lua_last_ok_ ? "Script exported" : ("Export failed: " + error);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Template")) {
        std::fill(lua_source_.begin(), lua_source_.end(), '\0');
        std::memcpy(lua_source_.data(), kLuaAddBotTemplate.data(), kLuaAddBotTemplate.size());
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Clear output")) {
        lua_output_.clear();
        lua_status_.clear();
    }
    if (!lua_status_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(lua_last_ok_ ? ImVec4(0.30f, 0.82f, 0.48f, 1.0f) : kError,
                           "%s", lua_status_.c_str());
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float output_h = std::clamp(avail.y * 0.32f, 120.0f, 210.0f);
    const float editor_h = std::max(180.0f, avail.y - output_h - 34.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextMultiline("##lua_source", lua_source_.data(), lua_source_.size(),
                              ImVec2(0.0f, editor_h), ImGuiInputTextFlags_AllowTabInput);

    ImGui::SeparatorText("Output");
    ImGui::BeginChild("##lua_output", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (lua_output_.empty())
        ImGui::TextDisabled("(no output)");
    else
        ImGui::TextUnformatted(lua_output_.c_str());
    ImGui::EndChild();
}

void AppUi::DrawAiSection() {
    const bool busy = ai_controller_ && ai_controller_->busy();

    // Provider presets. `wire` selects the request/response format the controller
    // speaks: 1 = Anthropic Messages API, 0 = OpenAI Chat Completions. Every entry
    // except Anthropic uses the OpenAI wire — Gemini and the rest all expose an
    // OpenAI-compatible /chat/completions endpoint, so a Bearer key + the right URL
    // is all they need. Selecting a preset prefills its endpoint + a default model.
    struct AiPreset {
        const char* label;
        int wire;              // 0 = OpenAI-compatible, 1 = Anthropic
        const char* endpoint;  // "" = official provider default (OpenAI/Anthropic)
        const char* model;     // default model prefilled into the Model box
        const char* key_hint;  // where to obtain an API key
    };
    static constexpr AiPreset kPresets[] = {
        {"OpenAI", 0, "", "gpt-5.4-mini", "platform.openai.com/api-keys"},
        {"Anthropic (Claude)", 1, "", "claude-sonnet-4-5-20250929",
         "console.anthropic.com/settings/keys"},
        {"Google Gemini", 0,
         "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions",
         "gemini-2.5-flash", "aistudio.google.com/apikey"},
        {"OpenRouter", 0, "https://openrouter.ai/api/v1/chat/completions",
         "openai/gpt-4o-mini", "openrouter.ai/keys"},
        {"Groq", 0, "https://api.groq.com/openai/v1/chat/completions",
         "llama-3.3-70b-versatile", "console.groq.com/keys"},
        {"DeepSeek", 0, "https://api.deepseek.com/v1/chat/completions", "deepseek-chat",
         "platform.deepseek.com/api_keys"},
        {"xAI (Grok)", 0, "https://api.x.ai/v1/chat/completions", "grok-2-latest",
         "console.x.ai"},
        {"Mistral", 0, "https://api.mistral.ai/v1/chat/completions", "mistral-large-latest",
         "console.mistral.ai/api-keys"},
        {"Together", 0, "https://api.together.xyz/v1/chat/completions",
         "meta-llama/Llama-3.3-70B-Instruct-Turbo", "api.together.ai/settings/api-keys"},
        {"Ollama (local)", 0, "http://localhost:11434/v1/chat/completions", "llama3.1",
         "local - type any non-empty value as the key"},
        {"Custom (OpenAI-compatible)", 0, "", "", "set the URL via the Endpoint button"},
    };
    constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
    if (ai_provider_ < 0 || ai_provider_ >= kPresetCount) ai_provider_ = 0;

    ImGui::SetNextItemWidth(200.0f);
    const int previous_provider = ai_provider_;
    const char* labels[kPresetCount];
    for (int i = 0; i < kPresetCount; ++i) labels[i] = kPresets[i].label;
    ImGui::Combo("##ai_provider", &ai_provider_, labels, kPresetCount);
    if (previous_provider != ai_provider_) {
        set_buf(ai_endpoint_, kPresets[ai_provider_].endpoint);
        set_buf(ai_model_, kPresets[ai_provider_].model);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint("##ai_model", "Model", ai_model_.data(), ai_model_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##ai_key", "API key (session only)", ai_api_key_.data(),
                             ai_api_key_.size(), ImGuiInputTextFlags_Password);
    ImGui::TextDisabled("Get a key: %s", kPresets[ai_provider_].key_hint);

    ImGui::SetNextItemWidth(155.0f);
    ImGui::Combo("##ai_autonomy", &ai_autonomy_, "Ask for approval\0Autonomous\0");
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &ai_follow_output_);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_GEAR " Endpoint")) ImGui::OpenPopup("AI endpoint");
    if (ImGui::BeginPopup("AI endpoint")) {
        ImGui::SetNextItemWidth(430.0f);
        ImGui::InputTextWithHint("##ai_endpoint", "Blank = provider default",
                                 ai_endpoint_.data(), ai_endpoint_.size());
        ImGui::EndPopup();
    }
    if (busy) {
        ImGui::SameLine();
        ImGui::TextColored(kAccent, "Working...");
    }

    // Example prompts — "Use" loads one into the input box below.
    if (ImGui::CollapsingHeader("Example prompts")) {
        auto example = [&](const char* ex) {
            ImGui::PushID(ex);
            if (ImGui::SmallButton("Use"))
                std::snprintf(ai_prompt_.data(), ai_prompt_.size(), "%s", ex);
            ImGui::SameLine();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(ex);
            ImGui::PopTextWrapPos();
            ImGui::PopID();
        };
        example("What is the current status of all bots?");
        example("Remove offline bots.");
        example("Add bots from a file and connect each one through the proxy pool.");
        example("Enable AutoGeiger on all bots and start the geiger farm.");
        example("Start the crash watcher and, if the app crashes, load the last fleet and "
                "run the automation on all of the bots.");
        example("Save the current fleet as a protected backup named 'main'.");
        ImGui::Separator();
    }

    const auto transcript = ai_controller_->transcript_snapshot();
    const auto approval = ai_controller_->pending_approval();
    float transcript_height = approval ? 147.0f : 240.0f;
    ImGui::BeginChild("##ai_transcript", ImVec2(0.0f, transcript_height), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (transcript.empty()) {
        ImGui::TextDisabled("(empty)");
    } else {
        for (const auto& entry : transcript) {
            const char* label = "Status";
            ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            switch (entry.role) {
                case nxrth::ai::TranscriptRole::User:
                    label = "You";
                    color = ImVec4(0.45f, 0.72f, 1.0f, 1.0f);
                    break;
                case nxrth::ai::TranscriptRole::Assistant:
                    label = "AI";
                    color = ImVec4(0.36f, 0.88f, 0.58f, 1.0f);
                    break;
                case nxrth::ai::TranscriptRole::Tool:
                    label = entry.tool_name.empty() ? "Tool" : entry.tool_name.c_str();
                    color = ImVec4(0.92f, 0.68f, 0.28f, 1.0f);
                    break;
                case nxrth::ai::TranscriptRole::Error:
                    label = "Error";
                    color = kError;
                    break;
                case nxrth::ai::TranscriptRole::Status: break;
            }
            ImGui::TextColored(color, "%s", label);
            ImGui::SameLine();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                   ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(entry.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }
        if (ai_follow_output_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 30.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    if (approval) {
        ImGui::BeginChild("##ai_approval", ImVec2(0.0f, 100.0f), true);
        ImGui::TextColored(ImVec4(0.92f, 0.68f, 0.28f, 1.0f), "Approval required");
        ImGui::BeginChild("##ai_call_previews", ImVec2(0.0f, 43.0f), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (std::size_t i = 0; i < approval->calls.size(); ++i) {
            const auto& call = approval->calls[i];
            std::string arguments = call.arguments.dump();
            if (arguments.size() > 180) arguments = arguments.substr(0, 177) + "...";
            ImGui::TextColored(ImVec4(0.72f, 0.82f, 0.96f, 1.0f), "%s",
                               call.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", arguments.c_str());
        }
        ImGui::EndChild();
        if (ImGui::Button(ICON_FA_CIRCLE_CHECK " Allow", ImVec2(100.0f, 0.0f))) {
            std::string error;
            if (!ai_controller_->approve(approval->id, true, &error)) ai_status_ = error;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CIRCLE_XMARK " Deny", ImVec2(100.0f, 0.0f))) {
            std::string error;
            if (!ai_controller_->approve(approval->id, false, &error)) ai_status_ = error;
        }
        ImGui::EndChild();
    }

    ImGui::InputTextMultiline("##ai_prompt", ai_prompt_.data(), ai_prompt_.size(),
                              ImVec2(-FLT_MIN, 65.0f));
    const bool can_send = !busy && ai_prompt_[0] != '\0' && ai_api_key_[0] != '\0' &&
                          ai_model_[0] != '\0';
    ImGui::BeginDisabled(!can_send);
    if (ImGui::Button(ICON_FA_PLAY " Send", ImVec2(100.0f, 0.0f))) {
        nxrth::ai::AiSettings settings;
        settings.provider = kPresets[ai_provider_].wire == 1
                                ? nxrth::ai::Provider::Anthropic
                                : nxrth::ai::Provider::OpenAICompatible;
        settings.endpoint = trimmed(ai_endpoint_.data());
        settings.model = trimmed(ai_model_.data());
        settings.api_key.assign(ai_api_key_.data());
        settings.autonomy = ai_autonomy_ == 0 ? nxrth::ai::Autonomy::Ask
                                              : nxrth::ai::Autonomy::Autonomous;
        std::string error;
        if (ai_controller_->submit(std::string(ai_prompt_.data()), std::move(settings), &error)) {
            std::fill(ai_prompt_.begin(), ai_prompt_.end(), '\0');
            ai_status_.clear();
        } else {
            ai_status_ = error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!busy);
    if (ImGui::Button(ICON_FA_CIRCLE_STOP " Stop", ImVec2(100.0f, 0.0f)))
        ai_controller_->cancel();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(ICON_FA_TRASH " Clear", ImVec2(100.0f, 0.0f))) {
        ai_controller_->clear();
        ai_status_.clear();
    }
    ImGui::EndDisabled();
    if (!ai_status_.empty()) {
        ImGui::SameLine();
        std::string visible = ai_status_;
        if (visible.size() > 36) visible = visible.substr(0, 33) + "...";
        ImGui::TextColored(kError, "%s", visible.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ai_status_.c_str());
    }
}

// ---------------------------------------------------------------------------
// Console: shared ring buffer from core/logger.h, optional per-bot filter.
// ---------------------------------------------------------------------------
void AppUi::DrawConsoleSection() {
    static std::vector<nxrth::LogLine> log_cache;
    static std::chrono::steady_clock::time_point log_refresh{};

    ImGui::Checkbox("Auto-scroll", &console_autoscroll_);
    ImGui::SameLine();
    ImGui::Checkbox("Only selected bot", &console_only_selected_);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Clear")) {
        nxrth::Logger::Instance().Clear();
        log_cache.clear();
    }
    ImGui::Separator();

    const auto now = std::chrono::steady_clock::now();
    if (now >= log_refresh) {
        log_cache = nxrth::Logger::Instance().Snapshot();
        log_refresh = now + std::chrono::milliseconds(250);
    }

    ImGui::BeginChild("log", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : log_cache) {
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
void AppUi::DrawBotDetail() {
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    const nxrth::bot::BotState* snap = CachedBotState(id);
    if (!snap) {
        ImGui::TextDisabled("bot #%u not found (removed).", id);
        return;
    }
    const nxrth::bot::BotState& st = *snap;

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
        if (ImGui::BeginTabItem("Traffic")) { DrawBotTrafficTab(st); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Automation")) { DrawDetailAutomation(st); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Rotation")) { DrawDetailRotation(st); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Logs")) { DrawBotLogsTab(st); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

// Main sub-tab: 2x2 panels - Bot Information | Movement / Preferences | Intervals.
void AppUi::DrawDetailMain(const nxrth::bot::BotState& st) {
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 6.0f;
    const float colW = (avail.x - gap) * 0.5f;
    const float rowH = (avail.y - gap) * 0.5f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));

    // ---------- Bot Information ----------
    PanelBegin("##pinfo", ICON_FA_USERS, "Bot Information", ImVec2(colW, rowH));
    {
        const std::string status = nxrth::bot::to_string(st.status);
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
            manager_.send_cmd(id, nxrth::bot::cmd::LeaveWorld{});
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::SmallButton("Warp")) {
            std::string w = trimmed(warp_name_.data());
            if (!w.empty()) manager_.send_cmd(id, nxrth::bot::cmd::Warp{w, ""});
        }
        const float hw = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (ImGui::Button("Connect", ImVec2(hw, 0.0f)))
            manager_.send_cmd(id, nxrth::bot::cmd::Reconnect{});
        ImGui::SameLine();
        if (ImGui::Button("Disconnect", ImVec2(-FLT_MIN, 0.0f)))
            manager_.send_cmd(id, nxrth::bot::cmd::Disconnect{});
    }
    PanelEnd();

    ImGui::SameLine();

    // ---------- Movement ----------
    PanelBegin("##pmove", ICON_FA_UP_DOWN_LEFT_RIGHT, "Movement", ImVec2(colW, rowH));
    {
        auto mv = [&](int dx, int dy) {
            manager_.send_cmd(id, nxrth::bot::cmd::Move{dx * move_steps_, dy * move_steps_});
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
            manager_.send_cmd(id, nxrth::bot::cmd::Respawn{});
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
        ImGui::TextDisabled("steps");
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
            manager_.send_cmd(id, nxrth::bot::cmd::SetAutoReconnect{r != 0});
        if (int r = pref_row("acol", "Auto Collect", st.auto_collect, true); r >= 0)
            manager_.send_cmd(id, nxrth::bot::cmd::SetAutoCollect{r != 0});
        if (int r = pref_row("aacc", "Auto Accept", pref_auto_accept_, false); r >= 0) {
            pref_auto_accept_ = (r != 0);
            if (r) manager_.send_cmd(id, nxrth::bot::cmd::AcceptAccess{});
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
            nxrth::bot::BotDelays d = st.delays;
            d.walk_ms = static_cast<std::uint64_t>(mvms);
            manager_.send_cmd(id, nxrth::bot::cmd::SetDelays{d});
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##ivcol", &iv_collect_ms_, 0, 2000, "Collect  -  %d ms");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##ivhit", &iv_hit_ms_, 0, 2000, "Hit  -  %d ms");
        int plms = static_cast<int>(st.delays.place_ms);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##ivplace", &plms, 0, 2000, "Place  -  %d ms")) {
            nxrth::bot::BotDelays d = st.delays;
            d.place_ms = static_cast<std::uint64_t>(plms);
            manager_.send_cmd(id, nxrth::bot::cmd::SetDelays{d});
        }
    }
    PanelEnd();

    ImGui::PopStyleVar();
}

// Automation sub-tab: the same shared editor as Executor plus this bot's scope
// and one-shot controls. World lists and parameters never live on BotState.
void AppUi::DrawDetailAutomation(const nxrth::bot::BotState& st) {
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    const float col = 210.0f;
    ImGui::Spacing();

    if (FontBold()) ImGui::PushFont(FontBold());
    ImGui::TextColored(kAccent, "Shared fleet automation");
    if (FontBold()) ImGui::PopFont();
    ImGui::TextWrapped(
        "These worlds and settings are shared with Executor and every bot.");
    ImGui::Spacing();

    const auto cfg_source = manager_.fleet()->config_handle();
    SyncAutomationEditor(cfg_source);
    auto cfg = *cfg_source;
    bool changed = DrawSharedAutomationEditor(cfg, true);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Per-bot settings ----
    if (FontBold()) ImGui::PushFont(FontBold());
    ImGui::TextColored(kAccent, "This bot");
    if (FontBold()) ImGui::PopFont();
    ImGui::Spacing();

    bool geiger = cfg.is_on_for(kModGeiger, id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("AutoGeiger");
    ImGui::SameLine(col);
    if (ToggleSwitch("##dgeiger", &geiger)) {
        auto scope_it = cfg.module_bot_ids.find(kModGeiger);
        if (geiger) {
            if (scope_it == cfg.module_bot_ids.end()) {
                if (auto group = cfg.module_groups.find(kModGeiger);
                    group != cfg.module_groups.end()) {
                    if (auto members = cfg.groups.find(group->second); members != cfg.groups.end())
                        cfg.module_bot_ids[kModGeiger] = members->second;
                }
            }
            auto& scope = cfg.module_bot_ids[kModGeiger];
            if (std::find(scope.begin(), scope.end(), id) == scope.end()) scope.push_back(id);
            std::sort(scope.begin(), scope.end());
            cfg.module_groups.erase(kModGeiger);
            cfg.enabled[kModGeiger] = true;
        } else if (scope_it != cfg.module_bot_ids.end()) {
            auto& scope = scope_it->second;
            scope.erase(std::remove(scope.begin(), scope.end(), id), scope.end());
            cfg.module_groups.erase(kModGeiger);
            if (scope.empty()) cfg.enabled[kModGeiger] = false;
        } else {
            // Convert a legacy fleet-wide scope to an explicit list containing
            // every current bot except this one.
            std::vector<std::uint32_t> scope;
            for (const auto& bot : manager_.list())
                if (bot.id != id) scope.push_back(bot.id);
            cfg.module_bot_ids[kModGeiger] = std::move(scope);
            cfg.module_groups.erase(kModGeiger);
            if (cfg.module_bot_ids[kModGeiger].empty()) cfg.enabled[kModGeiger] = false;
        }
        changed = true;
    }

    bool ac = st.auto_collect;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Auto-collect (this bot)");
    ImGui::SameLine(col);
    if (ToggleSwitch("##dac", &ac)) manager_.send_cmd(id, nxrth::bot::cmd::SetAutoCollect{ac});

    bool ar = st.auto_reconnect;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Auto-reconnect");
    ImGui::SameLine(col);
    if (ToggleSwitch("##dar2", &ar)) manager_.send_cmd(id, nxrth::bot::cmd::SetAutoReconnect{ar});

    int radius = st.collect_radius_tiles;
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::SliderInt("Collect radius (tiles)", &radius, 1, 5))
        manager_.send_cmd(id, nxrth::bot::cmd::SetCollectConfig{
                                  static_cast<std::uint8_t>(radius), st.collect_blacklist});

    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_CIRCLE_CHECK " Accept access"))
        manager_.send_cmd(id, nxrth::bot::cmd::AcceptAccess{});

    if (changed) {
        auto_save_status_.clear();
        manager_.fleet()->set_config(std::move(cfg));
        auto_params_source_ = manager_.fleet()->config_handle();
    }
}

// Rotation sub-tab: which proxy the bot uses + reconnect (pool-managed rotation).
void AppUi::DrawDetailRotation(const nxrth::bot::BotState& st) {
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    ImGui::Spacing();

    std::string proxy = "direct";
    auto it = manager_.bots.find(id);
    if (it != manager_.bots.end() && it->second.proxy_key)
        proxy = *it->second.proxy_key;

    ImGui::Text("Proxy: %s", proxy.c_str());
    ImGui::Text("Auto reconnect: %s", st.auto_reconnect ? "on" : "off");
    ImGui::Text("Ping: %u ms", st.ping_ms);
    ImGui::Spacing();
    ImGui::TextWrapped("Proxy rotation is managed by the pool (Proxy tab); "
                       "the next proxy is assigned on each reconnect.");
    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Reconnect"))
        manager_.send_cmd(id, nxrth::bot::cmd::Reconnect{});
}

// Logs sub-tab: the shared logger stream filtered to this bot.
void AppUi::DrawBotLoggerTab(const nxrth::bot::BotState& st) {
    (void)st;
    ImGui::BeginChild("##botlogger", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : nxrth::Logger::Instance().Snapshot()) {
        if (line.bot_id != selected_bot_) continue;
        ImGui::TextUnformatted(line.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// Inventory: one row per slot. Wear/Unwear (by is_active), Drop (1), Drop All
// (amount), Trash (1) -> manager_.send_cmd (mirrors Mori bot-detail buttons).
void AppUi::DrawInventoryTab(const nxrth::bot::BotState& st) {
    if (st.inventory.empty()) {
        ImGui::TextDisabled("inventory empty (bot is not in a world yet).");
        return;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    std::shared_ptr<const nxrth::world::ItemsDat> items = manager_.items_dat();

    // Search box: filter rows by item name or id number (case-insensitive).
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##invsearch", ICON_FA_MAGNIFYING_GLASS " Search item (name or id)",
                             inv_search_.data(), inv_search_.size());
    std::string needle = inv_search_.data();
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::size_t shown = 0;
    ImGui::TextDisabled("%zu slots", st.inventory.size());
    ImGui::Spacing();

    if (ImGui::BeginTable("inv", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, 52);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 288);
        ImGui::TableHeadersRow();

        for (const auto& slot : st.inventory) {
            const nxrth::world::ItemInfo* info =
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
                    manager_.send_cmd(id, nxrth::bot::cmd::Unwear{slot.item_id});
            } else {
                if (ImGui::SmallButton(ICON_FA_SHIRT " Wear"))
                    manager_.send_cmd(id, nxrth::bot::cmd::Wear{slot.item_id});
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_DOWN_LONG " Drop"))
                manager_.send_cmd(id, nxrth::bot::cmd::Drop{slot.item_id, 1});
            ImGui::SameLine();
            if (ImGui::SmallButton("Drop All"))
                manager_.send_cmd(id, nxrth::bot::cmd::Drop{slot.item_id, slot.amount});
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_TRASH " Trash"))
                manager_.send_cmd(id, nxrth::bot::cmd::Trash{slot.item_id, 1});

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!needle.empty() && shown == 0)
        ImGui::TextDisabled("no matches.");
}

// Colored-block minimap (fg=item-hashed color, bg=dim) + self (green) + others
// (yellow). Coords are in TILES; scaled to fit the content region.
void AppUi::DrawMinimapTab(const nxrth::bot::BotState& st) {
    if (st.world_width == 0 || st.world_height == 0 || st.tiles.empty()) {
        ImGui::TextDisabled("no world data (bot is not in a world yet).");
        return;
    }
    const int W = static_cast<int>(st.world_width);
    const int H = static_cast<int>(st.world_height);

    // Item-id -> real tile colour LUT (items.dat base_color), built once when
    // items are available. Avoids a per-tile linear find_by_id every frame.
    static std::vector<ImU32> lut;
    static bool lut_built = false;
    if (!lut_built) {
        std::shared_ptr<const nxrth::world::ItemsDat> items = manager_.items_dat();
        if (items && !items->items.empty()) {
            std::uint32_t maxId = 0;
            for (const auto& it : items->items)
                if (it.id > maxId) maxId = it.id;
            lut.assign(static_cast<std::size_t>(maxId) + 1, 0);
            for (const auto& it : items->items) {
                if (it.base_color != 0) {
                    const std::uint32_t rgb = nxrth::world::bgra_to_rgb(it.base_color);
                    lut[it.id] = IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
                }
            }
            lut_built = true;
        }
    }
    auto tile_col = [&](std::uint16_t id) -> ImU32 {
        return (id < lut.size()) ? lut[id] : 0;
    };

    ImGui::TextDisabled("%dx%d  -  " ICON_FA_CIRCLE_CHECK " green=bot   yellow=players (%zu)",
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
            const nxrth::bot::TileInfo& t = st.tiles[idx];
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
void DrawRingView(const char* id, const std::deque<std::string>& lines,
                  bool& autoscroll, const char* empty_hint) {
    ImGui::Checkbox("Auto-scroll", &autoscroll);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lines", lines.size());
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
void AppUi::DrawBotLogsTab(const nxrth::bot::BotState& st) {
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    bool en = st.logs_enabled;
    if (ImGui::Checkbox("Enable logs", &en))
        manager_.send_cmd(id, nxrth::bot::cmd::SetLogging{en});
    ImGui::Separator();
    if (!st.logs_enabled) {
        ImGui::TextDisabled(
            "Per-bot logging is off (opt-in). Enable to record this bot's system log.");
        return;
    }
    DrawRingView("##botlogs", st.console, detail_logs_autoscroll_, "(no logs)");
}

// Console tab = this bot's IN-GAME chat/console ring (BotState.chat), distinct
// from the system Logs.
void AppUi::DrawBotChatTab(const nxrth::bot::BotState& st) {
    DrawRingView("##botchat", st.chat, detail_chat_autoscroll_,
                 "(in-game messages appear here once the bot is in a world)");
}

// Traffic tab = incoming/outgoing packet log. Capture is opt-in (Enable, off by
// default) so it never auto-runs; Show Incoming / Show Outgoing filter the view.
void AppUi::DrawBotTrafficTab(const nxrth::bot::BotState& st) {
    const std::uint32_t id = static_cast<std::uint32_t>(selected_bot_);
    bool en = st.traffic_enabled;
    if (ImGui::Checkbox("Enable", &en))
        manager_.send_cmd(id, nxrth::bot::cmd::SetTrafficLog{en});
    ImGui::SameLine();
    ImGui::Checkbox("Show Incoming", &show_traffic_in_);
    ImGui::SameLine();
    ImGui::Checkbox("Show Outgoing", &show_traffic_out_);
    ImGui::Separator();
    if (!st.traffic_enabled) {
        ImGui::TextDisabled("Packet logging is disabled. Enable to capture traffic.");
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanelBg);
    ImGui::BeginChild("##bottraffic", ImVec2(0.0f, 0.0f), true);
    for (const auto& line : st.traffic) {
        const bool out = line.rfind("OUT ", 0) == 0;
        if ((out && !show_traffic_out_) || (!out && !show_traffic_in_)) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, out ? ImVec4(0.55f, 0.78f, 1.0f, 1.0f)
                                                 : ImVec4(0.62f, 0.88f, 0.62f, 1.0f));
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopStyleColor();
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
    if (ImGui::Checkbox("Always on top", &aot)) host_.set_always_on_top(aot);
    bool lk = host_.is_locked();
    if (ImGui::Checkbox("Window lock (drag disabled)", &lk)) host_.set_locked(lk);

    ImGui::Spacing();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderInt("FPS limit", &target_fps_, 15, 144);
    if (target_fps_ < 5) target_fps_ = 5;
    if (target_fps_ > 240) target_fps_ = 240;
    ImGui::SameLine();
    ImGui::TextDisabled("(UI refresh cap; default 30)");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (FontBold()) ImGui::PushFont(FontBold());
    ImGui::TextColored(kAccent, "Console");
    if (FontBold()) ImGui::PopFont();
    ImGui::Spacing();
    DrawConsoleSection();
}

}  // namespace nxrth::ui
