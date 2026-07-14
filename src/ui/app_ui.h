// Adonai — top-level ImGui application UI. ONE fixed-layout borderless window
// (700x650): a custom title bar, a left icon sidebar, and a content pane that
// renders only the selected section. Each panel is wired directly (in-process,
// no HTTP) to the engine: a BotManager (fleet supervisor, shared FleetState) and
// a ProxyPool. Panels only READ snapshots and ENQUEUE commands — they never
// block the UI thread on network work. The custom chrome drives the OS window
// through an IWindowHost (implemented over GLFW in main.cpp).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "bot/bot_manager.h"   // adonai::bot::{BotManager, BotInfo, Socks5Config, cmd}
#include "core/accounts.h"     // adonai::core::Account
#include "proxy/proxy_pool.h"  // adonai::proxy::{ProxyPool, RotatingLoginProxy}
#include "ui/window_host.h"    // adonai::ui::IWindowHost

namespace adonai::ui {

class AppUi {
public:
    // Non-owning: the engine objects + window host outlive the UI (main()).
    AppUi(adonai::bot::BotManager& manager, adonai::proxy::ProxyPool& proxy_pool,
          adonai::ui::IWindowHost& host);

    // Called once per frame between ImGui::NewFrame() and ImGui::Render().
    void Draw();

private:
    // --- chrome -------------------------------------------------------------
    void DrawTitleBar();   // centered title + dropdown + red move/lock/min/close
    void DrawTabBar();     // horizontal top tab strip (Lucifer-style)
    void DrawContent();    // routes the active tab into its section body

    // --- tab sections (draw into the current window; no ImGui::Begin/End) ----
    void DrawBotsTab();            // BOTS: left list column + right per-bot detail pane
    void DrawListSection();        // LIST: full-width bot table (Id/Bot/Status/...)
    void DrawAutomationSection();  // EXECUTOR: fleet AutomationConfig flags + toggles
    void DrawProxySection();       // PROXY: game pool + bypass pool editor
    void DrawSwitchSection();      // SWITCH: world/warp controls for the selected bot
    void DrawAccountsSection();    // DATABASE: bulk add accounts_stats.json -> spawn all
    void DrawSettingsSection();    // SETTINGS: shared console + app toggles
    void DrawConsoleSection();     // shared log stream (embedded in Settings)
    void DrawAddBotSection();      // add-bot form (rendered inside the "Add Bot" popup)

    // --- per-bot detail (right pane of the Bots tab; sub-tab notebook) ------
    void DrawBotDetail();          // Main/World/Inventory/Console/Automation/Rotation/Logs
    void DrawDetailMain(const adonai::bot::BotState& st);       // 2x2 info/move/pref/intervals
    void DrawInventoryTab(const adonai::bot::BotState& st);     // items + drop/trash/wear
    void DrawMinimapTab(const adonai::bot::BotState& st);       // tile/player minimap (World)
    void DrawBotLogsTab(const adonai::bot::BotState& st);       // per-bot SYSTEM log (Logs)
    void DrawBotChatTab(const adonai::bot::BotState& st);       // in-game chat/console (Console)
    void DrawDetailAutomation(const adonai::bot::BotState& st); // per-bot + fleet automation
    void DrawDetailRotation(const adonai::bot::BotState& st);   // proxy / reconnect info
    void DrawBotLoggerTab(const adonai::bot::BotState& st);     // (unused) global logger view

    // §2.9 proxy-resolution glue used before every spawn. Fills `out` from the
    // Add-bot proxy block (manual proxy | pool pick | none). Returns false and
    // sets add_error_ on a validation / pool error (no throw escapes).
    bool ResolveSpawnProxy(std::optional<adonai::bot::Socks5Config>& out);
    // §2.9 rotating login proxy (bypass pool). false + add_error_ on error.
    bool ResolveLoginProxy(std::optional<adonai::proxy::RotatingLoginProxy>& out);

    void LoadProxyEditor();   // pull ProxyPool state into the editor buffers (once)
    void ApplyProxyConfig();  // commit editor buffers to ProxyPool + disk (auto-save)

    adonai::bot::BotManager& manager_;
    adonai::proxy::ProxyPool& proxy_pool_;
    adonai::ui::IWindowHost& host_;

    int section_ = 0;        // active top tab (0 Bots .. 6 Settings)
    int selected_bot_ = -1;  // BotInfo.id of the focused bot, or -1

    // --- Bots-tab left column state -----------------------------------------
    std::array<char, 64> bot_search_{};  // "Search for bots.." filter
    bool multi_select_ = false;          // "Enable Multi-Select" toggle
    bool select_all_ = false;            // "Select All" toggle
    bool open_add_popup_ = false;        // set by "Add Bot" -> OpenPopup next frame

    // --- Bots-tab detail (Main sub-tab) scratch -----------------------------
    int move_steps_ = 1;                 // Movement: tiles per arrow press
    bool pref_bypass_ = false;           // Preferences: cosmetic-only toggles
    bool pref_auto_accept_ = false;
    bool pref_dyn_delay_ = false;        // Intervals: "Dynamic Delay"
    int iv_collect_ms_ = 100;            // Intervals: collect/hit are UI-side only
    int iv_hit_ms_ = 150;                // (no per-bot backend field for these)
    bool detail_logs_autoscroll_ = true; // Logs tab auto-scroll toggle
    bool detail_chat_autoscroll_ = true; // Console tab auto-scroll toggle
    std::array<char, 64> inv_search_{};  // Inventory tab: item name/id filter

    // --- fleet automation param editor (FleetState AutomationConfig.params) ---
    bool auto_params_loaded_ = false;
    std::string auto_save_status_;  // "Ayarlar kaydedildi" feedback
    int auto_collect_radius_ = 3;        // params["collect_radius"]
    bool auto_geiger_dig_ = true;        // params["geiger_dig"]
    std::array<char, 256> auto_webhook_url_{};  // params["webhook_url"]
    std::array<char, 256> auto_worlds_{};       // params["coordinate_worlds"]
    // geiger farm config (params["geiger_*"])
    std::array<char, 512> geiger_hunt_{};    // geiger_hunt_worlds
    std::array<char, 512> geiger_depot_{};   // geiger_depot_worlds
    std::array<char, 512> geiger_pickup_{};  // geiger_pickup_worlds
    std::array<char, 256> geiger_webhook_url_{};  // geiger_webhook_url (Discord)
    std::array<char, 512> geiger_drop_ids_{};     // geiger_drop_ids (extra prize ids to drop)
    int geiger_item_ = 2204;                 // geiger_item
    int geiger_recharge_min_ = 30;           // geiger_recharge_min
    int geiger_signal_wait_ms_ = 4200;       // geiger_signal_wait_ms
    int geiger_settle_ms_ = 700;             // geiger_settle_ms
    int geiger_max_steps_ = 70;               // geiger_max_steps
    int geiger_pickup_scan_ms_ = 3000;        // geiger_pickup_scan_ms
    int geiger_pickup_empty_scans_ = 12;      // geiger_pickup_empty_scans
    bool geiger_wear_ = true;                // geiger_wear
    // Executor: which bots AutoGeiger is armed on (multi-select; Ctrl+click). This
    // is the pending UI selection — committed to cfg.module_bot_ids["geiger"] only
    // when the user presses "Secili botlarda AC". New bots are never auto-armed.
    std::unordered_set<std::uint32_t> exec_sel_;

    // --- Accounts (bulk) state ----------------------------------------------
    std::array<char, 16384> acc_text_{};  // pasted accounts_stats.json / user:pass
    std::array<char, 260> acc_path_{};    // optional file path to load
    std::vector<adonai::core::Account> acc_parsed_;
    std::string acc_status_;
    int acc_mode_ = 1;         // 0 Standard (legacy), 1 Newly
    bool acc_use_pool_ = true;  // assign a pool proxy per account
    int acc_count_ = 10;              // "X" for First X / Random X / Add X buttons
    std::size_t acc_added_offset_ = 0;  // cursor for "Add X" (next batch of X)

    // --- Add-bot form state --------------------------------------------------
    std::array<char, 64> add_user_{};
    std::array<char, 128> add_pass_{};
    std::array<char, 512> add_ltoken_{};
    int add_mode_ = 0;  // 0 Standard (legacy), 1 Newly, 2 Ltoken
    std::array<char, 128> px_host_{};
    int px_port_ = 0;
    std::array<char, 64> px_user_{};
    std::array<char, 64> px_pass_{};
    bool use_proxy_pool_ = true;
    std::string add_error_;

    // --- Proxy pool editor state (mirrors ProxyPool, saved on demand) --------
    bool proxy_loaded_ = false;
    bool pp_enabled_ = false;
    int pp_max_per_ip_ = 1;
    int pp_spread_ = 0;  // 0 least_loaded, 1 round_robin
    std::array<char, 8192> pp_proxies_{};
    bool pp_rot_enabled_ = false;
    std::array<char, 32> pp_rot_scheme_{};
    int pp_rot_port_span_ = 2000;
    std::array<char, 8192> pp_rot_proxies_{};
    std::string proxy_status_;

    // --- Console filter ------------------------------------------------------
    bool console_only_selected_ = false;
    bool console_autoscroll_ = true;

    // --- World/control panel scratch -----------------------------------------
    std::array<char, 64> warp_name_{};
    std::array<char, 32> warp_id_{};
    int move_x_ = 0, move_y_ = 0;
    int walk_x_ = 0, walk_y_ = 0;
};

}  // namespace adonai::ui
