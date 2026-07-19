// Nxrth — top-level ImGui application UI. ONE fixed-layout borderless window
// (700x650): a custom title bar, a left icon sidebar, and a content pane that
// renders only the selected section. Each panel is wired directly (in-process,
// no HTTP) to the engine: a BotManager (fleet supervisor, shared FleetState) and
// a ProxyPool. Panels only READ snapshots and ENQUEUE commands — they never
// block the UI thread on network work. The custom chrome drives the OS window
// through an IWindowHost (implemented over GLFW in main.cpp).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ai/ai_controller.h"
#include "bot/bot_manager.h"   // nxrth::bot::{BotManager, BotInfo, Socks5Config, cmd}
#include "core/accounts.h"     // nxrth::core::Account
#include "lua/lua_engine.h"    // nxrth::lua::LuaEngine
#include "mcp/mcp_server.h"
#include "proxy/proxy_pool.h"  // nxrth::proxy::{ProxyPool, RotatingLoginProxy}
#include "script/script_store.h"
#include "ui/window_host.h"    // nxrth::ui::IWindowHost

namespace nxrth::ui {

class AppUi {
public:
    // Non-owning: the engine objects + window host outlive the UI (main()).
    AppUi(nxrth::bot::BotManager& manager, nxrth::proxy::ProxyPool& proxy_pool,
          nxrth::ui::IWindowHost& host);
    // Cancels + joins the background proxy-check thread.
    ~AppUi();

    AppUi(const AppUi&) = delete;
    AppUi& operator=(const AppUi&) = delete;

    // Called once per frame between ImGui::NewFrame() and ImGui::Render().
    void Draw();

    // UI frame-rate cap (frames/sec) the main loop paces itself to. Adjustable
    // from the Settings tab; default 30.
    int target_fps() const { return target_fps_; }

private:
    // --- chrome -------------------------------------------------------------
    void DrawTitleBar();   // centered title + dropdown + red move/lock/min/close
    void DrawTabBar();     // horizontal top tab strip (Lucifer-style)
    void DrawContent();    // routes the active tab into its section body

    // --- tab sections (draw into the current window; no ImGui::Begin/End) ----
    void DrawBotsTab();            // BOTS: left list column + right per-bot detail pane
    void DrawListSection();        // LIST: full-width bot table (Id/Bot/Status/...)
    void DrawAutomationSection();  // EXECUTOR: fleet AutomationConfig flags + toggles
    void DrawLuaExecutor();        // EXECUTOR: Lua 5.4 editor + redacted output
    void DrawAiSection();          // AI: provider chat + MCP tool approval loop
    void SyncAutomationEditor(
        const std::shared_ptr<const nxrth::bot::AutomationConfig>& source);
    bool DrawSharedAutomationEditor(nxrth::bot::AutomationConfig& cfg, bool compact);
    void DrawProxySection();       // PROXY: Socks5 / Logon Bypass sub-tabs (ui4)
    void DrawProxyTable(std::array<char, 8192>& buffer, bool game_pool);  // one sub-tab
    void StartProxyCheck(const std::string& buffer_text);  // async SOCKS5 reachability
    void DrawAccountsSection();    // DATABASE: items.dat viewer (ui5)
    void DrawBulkImport();         // bulk account import (folded into the Add-bot popup)
    void DrawSettingsSection();    // SETTINGS: shared console + app toggles
    void DrawConsoleSection();     // shared log stream (embedded in Settings)
    void DrawAddBotSection();      // add-bot form (rendered inside the "Add Bot" popup)
    void SpawnFromAddForm();       // spawn a bot from the Add-bot form fields
    void SaveBotRecords();         // write bot_records_ (pipe format) to bots_saved.txt
    void LoadBotRecords();         // read bots_saved.txt and spawn each pipe record

    // --- per-bot detail (right pane of the Bots tab; sub-tab notebook) ------
    void DrawBotDetail();          // Main/World/Inventory/Console/Automation/Rotation/Logs
    void DrawDetailMain(const nxrth::bot::BotState& st);       // 2x2 info/move/pref/intervals
    void DrawInventoryTab(const nxrth::bot::BotState& st);     // items + drop/trash/wear
    void DrawMinimapTab(const nxrth::bot::BotState& st);       // tile/player minimap (World)
    void DrawBotLogsTab(const nxrth::bot::BotState& st);       // per-bot SYSTEM log (Logs)
    void DrawBotChatTab(const nxrth::bot::BotState& st);       // in-game chat/console (Console)
    void DrawBotTrafficTab(const nxrth::bot::BotState& st);    // incoming/outgoing packets (Traffic)
    void DrawDetailAutomation(const nxrth::bot::BotState& st); // per-bot + fleet automation
    void DrawDetailRotation(const nxrth::bot::BotState& st);   // proxy / reconnect info
    void DrawBotLoggerTab(const nxrth::bot::BotState& st);     // (unused) global logger view

    const std::vector<nxrth::bot::BotInfo>& CachedBotList();
    const nxrth::bot::BotState* CachedBotState(std::uint32_t id);

    // §2.9 proxy-resolution glue used before every spawn. Fills `out` from the
    // Add-bot proxy block (manual proxy | pool pick | none). Returns false and
    // sets add_error_ on a validation / pool error (no throw escapes).
    bool ResolveSpawnProxy(std::optional<nxrth::bot::Socks5Config>& out);
    // §2.9 rotating login proxy (bypass pool). false + add_error_ on error.
    bool ResolveLoginProxy(std::optional<nxrth::proxy::RotatingLoginProxy>& out);

    void LoadProxyEditor();   // pull ProxyPool state into the editor buffers (once)
    void ApplyProxyConfig();  // commit editor buffers to ProxyPool + disk (auto-save)

    nxrth::bot::BotManager& manager_;
    nxrth::proxy::ProxyPool& proxy_pool_;
    nxrth::ui::IWindowHost& host_;
    std::unique_ptr<nxrth::lua::LuaEngine> lua_engine_;
    std::unique_ptr<nxrth::script::ScriptStore> script_store_;
    std::unique_ptr<nxrth::mcp::McpServer> ai_mcp_;
    std::unique_ptr<nxrth::ai::AiController> ai_controller_;

    int target_fps_ = 30;    // Settings: UI frame-rate cap (main loop paces to this)
    int section_ = 0;        // active top tab (0 Bots .. 6 AI)
    int selected_bot_ = -1;  // BotInfo.id of the focused bot, or -1
    std::vector<nxrth::bot::BotInfo> bot_list_cache_;
    std::chrono::steady_clock::time_point bot_list_refresh_{};
    std::optional<nxrth::bot::BotState> bot_state_cache_;
    std::uint32_t bot_state_cache_id_ = UINT32_MAX;
    std::chrono::steady_clock::time_point bot_state_refresh_{};

    // --- Bots-tab left column state -----------------------------------------
    std::array<char, 64> bot_search_{};  // "Search for bots.." filter
    bool multi_select_ = false;          // "Enable Multi-Select" toggle
    bool select_all_ = false;            // "Select All" toggle
    bool prev_select_all_ = false;       // edge-detect for Select All
    std::unordered_set<std::uint32_t> bots_sel_;  // multi-selected bot ids (Ctrl+click)
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
    bool show_traffic_in_ = true;        // Traffic tab: show incoming packets
    bool show_traffic_out_ = true;       // Traffic tab: show outgoing packets
    std::array<char, 64> inv_search_{};  // Inventory tab: item name/id filter

    // --- fleet automation param editor (FleetState AutomationConfig.params) ---
    bool auto_params_loaded_ = false;
    std::shared_ptr<const nxrth::bot::AutomationConfig> auto_params_source_;
    std::string auto_save_status_;  // "Settings saved" feedback
    int auto_collect_radius_ = 3;        // params["collect_radius"]
    bool auto_geiger_dig_ = true;        // params["geiger_dig"]
    std::array<char, 256> auto_worlds_{};       // params["coordinate_worlds"]
    // geiger farm config (params["geiger_*"])
    std::array<char, 4096> geiger_hunt_{};    // geiger_hunt_worlds
    std::array<char, 4096> geiger_depot_{};   // geiger_depot_worlds
    std::array<char, 4096> geiger_pickup_{};  // geiger_pickup_worlds
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
    // when the user presses "Enable on selected bots". New bots are never auto-armed.
    std::unordered_set<std::uint32_t> exec_sel_;

    // --- Executor Lua editor ------------------------------------------------
    std::vector<char> lua_source_;
    std::string lua_output_;
    std::string lua_status_;
    bool lua_last_ok_ = true;
    std::array<char, 72> lua_script_name_{};
    long lua_run_target_ = -1;  // Executor "Run on": -1 fleet(once), -2 all bots, >=0 bot id

    // --- Embedded AI controller --------------------------------------------
    int ai_provider_ = 0;    // index into the AI provider preset table (DrawAiSection)
    int ai_autonomy_ = 0;    // Ask, Autonomous
    std::array<char, 256> ai_model_{};
    std::array<char, 2048> ai_endpoint_{};
    std::array<char, 8193> ai_api_key_{};  // memory-only; explicitly zeroed on shutdown
    std::vector<char> ai_prompt_;
    std::string ai_status_;
    bool ai_follow_output_ = true;
    std::uint64_t ai_rpc_id_ = 1;

    // --- Accounts (bulk) state ----------------------------------------------
    std::array<char, 16384> acc_text_{};  // pasted accounts_stats.json / user:pass
    std::array<char, 260> acc_path_{};    // optional file path to load
    std::vector<nxrth::core::Account> acc_parsed_;
    std::string acc_status_;
    int acc_mode_ = 1;         // Standard, Google OAuth ltoken
    bool acc_use_pool_ = true;  // assign a pool proxy per account
    int acc_count_ = 10;              // "X" for First X / Random X / Add X buttons
    std::size_t acc_added_offset_ = 0;  // cursor for "Add X" (next batch of X)

    // --- Add-bot form state (ui3: per-field entry) --------------------------
    std::array<char, 4096> add_cred_{};   // GrowID / Mail / Token (or a full keyed record)
    std::array<char, 128> add_pass_{};    // password (GrowID / Mail login)
    std::array<char, 64> add_mac_{};      // [optional] device MAC
    std::array<char, 64> add_rid_{};      // [optional] device RID
    std::array<char, 64> add_hash_{};     // [optional] device hash
    std::array<char, 128> add_otp_{};     // [optional] OTP / 2FA secret
    std::array<char, 256> add_proxy_{};   // [optional] custom proxy host:port[:user:pass]
    int add_platform_ = 0;                // platform combo (0 Windows ..)
    std::string add_error_;
    std::string bots_status_;  // Save/Load feedback shown under the Load/Save buttons

    // --- Proxy pool editor state (mirrors ProxyPool, saved on demand) --------
    bool proxy_loaded_ = false;
    bool pp_enabled_ = false;
    int pp_max_per_ip_ = 1;
    int pp_spread_ = 0;  // 0 least_loaded, 1 round_robin
    bool pp_shuffle_ = false;
    std::array<char, 8192> pp_proxies_{};
    bool pp_rot_enabled_ = false;
    std::array<char, 32> pp_rot_scheme_{};
    int pp_rot_port_span_ = 2000;
    std::array<char, 8192> pp_rot_proxies_{};
    std::string proxy_status_;

    // --- Proxy tab (ui4): rows are parsed live from the pool buffers above ----
    int proxy_subtab_ = 0;                  // 0 Socks5 (game), 1 Logon Bypass
    std::string proxy_sel_key_;             // selected row ("host:port:user")
    std::array<char, 96> proxy_search_{};   // "Search for proxy.." filter
    std::array<char, 256> proxy_add_buf_{}; // Add-popup input line
    bool open_proxy_add_ = false;           // request: open the Add popup
    bool open_proxy_settings_ = false;      // request: open the Settings popup
    // Async SOCKS5 reachability results, keyed by row key. Value: 0 = checking,
    // 1 = valid, -1 = invalid (absent = never checked). Guarded by the mutex; the
    // background thread writes, the UI thread reads.
    std::mutex proxy_check_mu_;
    std::unordered_map<std::string, int> proxy_check_status_;
    std::atomic<bool> proxy_checking_{false};
    std::atomic<bool> proxy_check_cancel_{false};
    std::atomic<int> proxy_check_done_{0};
    std::atomic<int> proxy_check_total_{0};
    std::thread proxy_check_thread_;

    // --- Database tab (ui5): items.dat viewer -------------------------------
    int db_sel_item_ = -1;                 // selected item id, or -1
    std::array<char, 64> db_search_{};     // "Search for items." filter
    bool db_auto_update_ = false;          // Auto Update Items (live-jump to match)
    bool db_display_seeds_ = true;         // Display Seeds
    bool db_display_null_ = true;          // Display Null Items
    std::string db_copy_status_;           // "Copied item id" feedback

    // --- Console filter ------------------------------------------------------
    bool console_only_selected_ = false;
    bool console_autoscroll_ = true;

    // --- World/control panel scratch (warp_name_ used by the detail Main sub-tab) -
    std::array<char, 64> warp_name_{};
};

}  // namespace nxrth::ui
