// Nxrth — the per-bot engine: FULL class declaration (port specs 06/07/08).
// The three impl units (bot_core_state.cpp = connect/reconnect/gates,
// bot_core_handlers.cpp = service_once/packet dispatch/login packets,
// bot_core_automation.cpp = in-world handlers/collect/geiger/actions) implement
// against THIS single header — every field and every cross-file method is
// declared here so nothing is left to guess.
//
// One Bot lives on one OS worker thread (Bot::run is the thread body, ~10 ms
// tick). It owns its ENet host + all world/player state and coordinates with the
// rest of the fleet ONLY through the process-global gates (gates.h) and the
// shared FleetState (fleet_state.h). Automation is NATIVE + fleet-aware: each
// tick run_automation() drives attached AutomationModules against the FleetState.
// NO Lua, NO web server. GT wire data is little-endian.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bot/bot_state.h"
#include "bot/fleet_state.h"
#include "bot/gates.h"
#include "login/login.h"       // nxrth::login::{Credentials, LoginIdentity, LoginMethod, LoginMethodKind}
#include "net/enet_host.h"     // nxrth::net::{BotHost, PeerId, HostEvent}
#include "net/socks5_udp.h"    // nxrth::net::Socks5Config
#include "protocol/packet.h"   // nxrth::protocol::{GameUpdatePacket, IncomingPacket, GamePacketType}
#include "protocol/variant.h"  // nxrth::protocol::VariantList
#include "proxy/proxy_pool.h"  // nxrth::proxy::{RotatingLoginProxy, next_game_proxy}
#include "world/inventory.h"
#include "world/items.h"
#include "world/pathfind.h"
#include "world/world.h"

namespace nxrth::bot {

using Socks5Config = nxrth::net::Socks5Config;

// ---------------------------------------------------------------------------
// Bot-core constants NOT owned by gates.h (spec 06 §1.1, 08 §12).
// ---------------------------------------------------------------------------
inline constexpr std::uint8_t REDIRECT_MAX_GAME_PROXY_TRIES = 6;
inline constexpr std::uint16_t GEIGER_COUNTER_ITEM_ID = 2204;
inline constexpr std::uint16_t DEAD_GEIGER_COUNTER_ITEM_ID = 2286;
inline constexpr std::size_t CONSOLE_RING_CAP = 300;
inline constexpr std::size_t COLLECT_MAX_PER_TICK = 32;
inline constexpr std::uint8_t PER_ITEM_INV_CAP = 200;
inline constexpr std::uint16_t GEMS_ITEM_ID = 112;
inline constexpr std::uint16_t FIST_ITEM_ID = 18;
inline constexpr std::uint16_t WRENCH_ITEM_ID = 32;

// ---------------------------------------------------------------------------
// §1.5 RedirectData — captured from OnSendToServer; consumed once.
// ---------------------------------------------------------------------------
struct RedirectData {
    std::string server;
    std::uint16_t port = 0;
    std::string token;
    std::string user;
    std::string door_id;      // "0" if the redirect gave none
    std::string uuid;
    std::string lmode;        // stringified i32
    std::string tank_id_name;
};

// §1.6 GeigerGreenRepeat — rapid-green particle tracker.
struct GeigerGreenRepeat {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint64_t last_seen_ms = 0;
    std::uint8_t count = 0;
};

// Resolved gateway/subserver endpoint (Rust SocketAddr). Stored so connect() can
// be re-issued across reconnects/redirects without re-resolving.
struct SockEndpoint {
    sockaddr_storage ss{};
    socklen_t len = 0;
    const sockaddr* addr() const { return reinterpret_cast<const sockaddr*>(&ss); }
};

// §4.3 One-shot dialog callback holder (fired on the next OnDialogRequest, then
// cleared). Callbacks may chain-install the next one (accept_access). Single-
// thread (bot thread) access — the callback captures & runs on that thread.
class Bot;  // fwd
struct TemporaryData {
    std::optional<std::function<void(Bot&)>> dialog_callback;
};

// ---------------------------------------------------------------------------
// Native fleet-aware automation seam (ARCHITECTURE). A module ticks each bot
// worker iteration with that bot's live context (the Bot itself) + the shared
// FleetState, and drives the bot by calling its action helpers directly (same
// thread, so blocking actions keep ENet serviced via sleep_ms). Replaces Nxrth's
// Lua script_channel entirely.
// ---------------------------------------------------------------------------
using BotContext = Bot;
struct AutomationModule {
    virtual ~AutomationModule() = default;
    virtual const char* name() const = 0;
    virtual void on_enabled(BotContext&, FleetState&) {}
    virtual void on_disabled(BotContext&, FleetState&) {}
    virtual void tick(BotContext& self, FleetState& fleet,
                      const AutomationConfig& config) = 0;
};

// ===========================================================================
// Bot
// ===========================================================================
class Bot {
public:
    // --- §2.4 factories (return nullptr on failed spawn; never throw) --------
    // Nxrth returned Option<Bot>; a std::unique_ptr keeps Bot non-movable (it
    // holds a std::mutex-guarded state / non-relocatable members).
    static std::unique_ptr<Bot> create(
        const std::string& username, const std::string& password,
        std::optional<Socks5Config> proxy,
        std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy,
        std::shared_ptr<std::atomic<bool>> stop, std::shared_ptr<SharedBotState> state,
        CmdReceiver cmd_rx, std::shared_ptr<const nxrth::world::ItemsDat> items_dat,
        std::uint32_t bot_id, EventSinkPtr sink, FleetHandle fleet);

    static std::unique_ptr<Bot> create_ltoken(
        const std::string& ltoken_str, std::optional<Socks5Config> proxy,
        std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy,
        std::shared_ptr<std::atomic<bool>> stop, std::shared_ptr<SharedBotState> state,
        CmdReceiver cmd_rx, std::shared_ptr<const nxrth::world::ItemsDat> items_dat,
        std::uint32_t bot_id, EventSinkPtr sink, FleetHandle fleet);

    ~Bot();
    Bot(const Bot&) = delete;
    Bot& operator=(const Bot&) = delete;
    Bot(Bot&&) = delete;
    Bot& operator=(Bot&&) = delete;

    // --- §2.7 the driver loop (thread body). Runs until stop/disconnect. ------
    void run(std::shared_ptr<std::atomic<bool>> stop_flag);

    // --- automation attachment / hook ----------------------------------------
    void add_automation_module(std::unique_ptr<AutomationModule> mod);
    // Called once per run() tick (replaces drain_script_requests): ticks each
    // enabled module against the shared fleet. No-op if no fleet/modules.
    void run_automation(FleetState& fleet);

    // ==== ACTION HELPERS (public — commands + automation modules call these) ==
    // All keep ENet serviced during their sleeps (sleep_ms), never dead-sleep.
    void say(const std::string& text);
    void warp(const std::string& name, const std::string& id);  // WARP_GATE paced
    void leave_world();
    void respawn();
    void disconnect();
    void reconnect();
    void place(std::int32_t offset_x, std::int32_t offset_y, std::uint32_t item_id, bool is_punch);
    // A tile update arrived at (x,y): resolve any pending place there. If the tile
    // now shows our item (fg or bg) the server accepted it -> consume 1 from the
    // inventory; otherwise the place is dropped without consuming.
    void confirm_place(std::uint32_t x, std::uint32_t y, std::uint16_t fg, std::uint16_t bg);
    // Drop pending places whose confirmation deadline passed (lag / no access):
    // release the reservation, never consume. Called each run() tick.
    void expire_pending_places();
    void punch(std::int32_t ox, std::int32_t oy);                 // place(.,.,18,true)
    void wrench(std::int32_t ox, std::int32_t oy);                // place(.,.,32,false)
    void wrench_at(std::int32_t tile_x, std::int32_t tile_y);     // absolute tile
    void wrench_player(std::uint32_t net_id);
    void active_tile(std::int32_t tile_x, std::int32_t tile_y);
    void enter(const std::optional<std::string>& pass);
    void wear(std::uint32_t item_id);
    void activate_item(std::uint32_t item_id);
    void unwear(std::uint32_t item_id);
    void drop_item(std::uint32_t item_id, std::uint32_t amount);
    void trash_item(std::uint32_t item_id, std::uint32_t amount);
    void fast_drop(std::uint32_t item_id, std::uint32_t count);
    void fast_trash(std::uint32_t item_id, std::uint32_t count);
    void walk(std::int32_t tile_x, std::int32_t tile_y);
    void set_direction(bool facing_left);
    void find_path(std::uint32_t x, std::uint32_t y);             // blocking walk
    std::vector<std::pair<std::uint32_t, std::uint32_t>> compute_path(std::uint32_t x,
                                                                      std::uint32_t y);
    void collect_object_at(std::uint32_t uid, float range_tiles);
    std::size_t collect();  // auto-pickup; also the 500ms tick target
    void idle(std::uint64_t ms);  // service-while-sleeping pause (automation deposits/dwell)
    void log(const std::string& msg);  // public log passthrough for automation modules
    bool has_access() const;
    bool is_item_equipped(std::uint16_t item_id) const;
    void mark_item_equipped(std::uint16_t item_id, bool active);
    void accept_access();
    void set_auto_collect(bool enabled);

    // --- read-only accessors automation/queries use directly -----------------
    std::uint32_t bot_id() const { return bot_id_; }
    const std::optional<nxrth::world::World>& world() const { return world_; }
    const std::unordered_map<std::uint32_t, Player>& players() const { return players_; }
    const nxrth::world::Inventory& inventory() const { return inventory_; }
    // Shared item DB (names for ids); may be null. Used by automation (e.g. the
    // geiger webhook) to print item names instead of raw ids.
    const nxrth::world::ItemsDat* items_dat() const { return items_dat_.get(); }
    // Current auto-collect flag (so automation can save/restore it around a deposit).
    bool auto_collect() const { return auto_collect_; }
    float pos_x() const { return pos_x_; }
    float pos_y() const { return pos_y_; }
    const LocalPlayer& local() const { return local_; }
    bool in_world() const { return world_.has_value(); }
    // Current geiger reading (from the shared state the geiger handlers publish).
    // Used by the native GeigerModule to drive fleet-coordinated farming.
    std::optional<GeigerSignal> geiger_signal() const {
        return state_ ? state_->read([](const BotState& s) { return s.geiger_signal; })
                      : std::nullopt;
    }
    // Block (KEEPING ENET SERVICED) until a geiger particle newer than
    // `newer_than_ms` (compare GeigerSignal.timestamp_ms) is published, or
    // `timeout_ms` elapses / the bot is asked to stop. Returns the fresh reading
    // or std::nullopt on timeout/stop. Lets the GeigerModule sequence
    // probe->read reliably (the particle arrives asynchronously after a move).
    std::optional<GeigerSignal> wait_for_geiger(std::uint64_t newer_than_ms,
                                                std::uint64_t timeout_ms);

private:
    // Private full constructor (all constructors funnel here after fetch).
    Bot(std::string username, nxrth::login::LoginMethod login_method,
        nxrth::login::Credentials creds, std::optional<Socks5Config> proxy,
        std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy,
        std::shared_ptr<std::atomic<bool>> stop, std::shared_ptr<SharedBotState> state,
        CmdReceiver cmd_rx, std::shared_ptr<const nxrth::world::ItemsDat> items_dat,
        std::uint32_t bot_id, EventSinkPtr sink, FleetHandle fleet);

    // ==== §2.7-§2.8 loop internals ==========================================
    void service_once();                 // drain ALL pending ENet events
    void sleep_ms(std::uint64_t ms);     // service-while-sleeping
    void shutdown();                     // disconnect + flush + tear-down
    void drain_commands();               // cmd_rx try_recv loop -> handle_command
    void handle_command(const BotCommand& cmd);
    void publish_fleet_view();           // upsert this bot's BotView each tick

    // ==== ENet host + connect/reconnect (bot_core_state.cpp) =================
    nxrth::net::BotHost create_host(const Socks5Config* proxy);  // logs + BotHost::create
    void reconnect_main(bool refresh_token);
    // Quarantine the current post-logon exit IP (rate-limited by GT's game servers:
    // it can bypass-logon but the game connection keeps failing) so no bot picks it,
    // and rotate to a fresh one. For a bypass bot the forced refresh-login re-pins a
    // new bypass IP (login_session skips quarantined); for a game-proxy bot it
    // rotates proxy_ in place. Mirrors the 403 quarantine, for the game connection.
    void quarantine_logon_ip(const std::string& reason);
    void schedule_reconnect(const std::string& reason, bool refresh_token,
                            std::uint64_t base_ms);
    void refresh_token();
    void apply_credentials(const nxrth::login::Credentials& creds);
    void apply_login_identity(const nxrth::login::LoginIdentity& identity);
    // server_data fetch loop shared by ltoken/har constructors + reconnect_main.
    std::optional<nxrth::net::ServerData> fetch_server_data_loop();

    // Connected-phase gate wait that KEEPS ENET SERVICED (in_gate_wait guarded).
    void wait_for_global_gate(Gate& gate, std::uint64_t spacing_ms, const char* label);

    // ==== event handlers (bot_core_handlers.cpp) ============================
    void on_connect(nxrth::net::PeerId id);
    void on_disconnect(nxrth::net::PeerId id, std::uint32_t data);
    void on_receive(nxrth::net::PeerId id, std::uint8_t channel,
                    const std::vector<std::uint8_t>& data);
    void on_server_hello();
    void handle_game_message(const std::string& s, nxrth::net::PeerId id);  // scans + logon_fail
    void handle_track(const std::string& s);
    void on_ping_request(std::uint32_t challenge);
    void on_call_function(nxrth::net::PeerId id, const std::vector<std::uint8_t>& extra_data);
    void clear_login_state_flags();

    // login/redirect packet builders (bot_core_handlers.cpp)
    std::string build_login_packet() const;                   // 3 forms + login_token_field
    nxrth::login::LoginIdentity login_identity_view() const;  // members -> LoginIdentity
    std::string build_provider_login_packet() const;          // full-identity validate-ltoken body
    std::string build_redirect_packet(const RedirectData& r) const;  // protocol|211
    std::string build_login_data() const;                     // Google checktoken clientData

    // send primitives (all no-op without peer_id; channel 0)
    void send_text(const std::string& text);                  // reliable
    void send_game_message(const std::string& text);          // reliable
    void send_game_packet(const nxrth::protocol::GameUpdatePacket& pkt, bool reliable);

    // ==== in-world GameUpdate handlers (bot_core_automation.cpp) =============
    void on_state(const nxrth::protocol::GameUpdatePacket& pkt);
    void on_tile_change(const nxrth::protocol::GameUpdatePacket& pkt);
    void on_send_tile_update_data(const nxrth::protocol::GameUpdatePacket& pkt);
    void on_send_tile_update_data_multiple(const nxrth::protocol::GameUpdatePacket& pkt);
    void on_send_tile_tree_state(const nxrth::protocol::GameUpdatePacket& pkt);
    void on_modify_item_inventory(const nxrth::protocol::GameUpdatePacket& pkt);
    void on_item_change_object(const nxrth::protocol::GameUpdatePacket& pkt);
    void on_send_lock(const nxrth::protocol::GameUpdatePacket& pkt);
    void load_world(const nxrth::protocol::GameUpdatePacket& pkt);  // SendMapData

    // ==== geiger (bot_core_automation.cpp §7) ===============================
    void update_geiger_signal(const nxrth::protocol::GameUpdatePacket& pkt);
    void sync_geiger_state_from_console(const std::string& message);
    static std::optional<std::tuple<std::uint32_t, std::uint32_t, std::uint8_t>>
    decode_geiger_signal_packet(const nxrth::protocol::GameUpdatePacket& pkt);

    // ==== emit / log / redaction primitives (§11) ===========================
    void log_console(const std::string& msg);  // per-bot SYSTEM log ring (Logs tab)
    void log_chat(const std::string& msg);      // in-game console/chat ring (Console tab)
    void emit_traffic(const std::string& direction, const std::string& kind, std::size_t size,
                      const std::string& detail);
    bool wants_traffic() const {
        return (sink_ && sink_->wants_traffic(bot_id_)) ||
               (state_ && state_->read([](const BotState& s) { return s.traffic_enabled; }));
    }
    void emit_inventory_update();
    void emit_status(BotStatus status);  // writes state.status + notifies dirty
    void notify_dirty();                  // UI should re-read this bot's BotState

    // grid/path helpers (§9.2)
    std::vector<std::pair<std::uint16_t, std::uint8_t>> build_collision_grid(bool item_collect) const;

    // ======================= FIELDS (spec 06 §1.7 order) ====================
    nxrth::net::BotHost host_;                                        // 1
    std::optional<Socks5Config> proxy_;                               // 2  game proxy
    std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy_;    // 3
    std::optional<Socks5Config> bypass_enet_;                         // 4  pinned logon IP
    std::shared_ptr<std::atomic<bool>> stop_;                        // 5
    std::string username_;                                            // 6
    nxrth::login::LoginMethod login_method_;                        // 7
    std::string ltoken_;                                             // 8
    std::string meta_;                                              // 9
    std::string mac_;                                              // 10
    std::string hash_;                                            // 11
    std::string hash2_;                                          // 12
    std::string klv_;                                          // 13
    std::string fz_;                                          // 14
    std::string game_version_;                              // 15
    std::string cbits_;                                    // 16
    std::string player_age_;                              // 17
    std::string gdpr_;                                   // 18
    std::string category_;                              // 19
    std::string total_playtime_;                       // 20
    std::string country_;                             // 21
    std::string zf_;                                 // 22
    std::string platform_id_;                       // 23
    std::string steam_token_;                      // 24
    std::string wk_;                              // 25
    std::string rid_;                            // 26
    std::string vid_;                            // 26b provider validate-token device id
    std::optional<RedirectData> redirect_;      // 27
    std::uint8_t redirect_attempts_ = 0;        // 28
    std::optional<std::string> last_redirect_token_;  // 29
    std::optional<std::string> last_redirect_uuid_;   // 30
    bool refresh_token_on_reconnect_ = false;   // 31
    std::optional<SockEndpoint> server_addr_;   // 32  gateway address
    bool saw_server_hello_ = false;             // 33
    std::optional<std::chrono::steady_clock::time_point> connected_since_;  // 34 (login watchdog)
    bool was_in_world_ = false;                 // 35
    std::uint8_t redirect_connect_fails_ = 0;   // 36
    std::uint8_t pre_hello_disconnects_ = 0;    // 37
    // World-join tracking: the subserver/world leg is carried by the GAME proxy
    // (not the bypass logon IP). A world connect that drops before ServerHello
    // means the game proxy can't reach the world -> rotate it (not the logon IP).
    bool on_subserver_connect_ = false;         // last-initiated connect is a world/subserver leg
    std::uint8_t subserver_connect_fails_ = 0;  // consecutive world-join drops before ServerHello
    std::uint32_t login_reject_streak_ = 0;     // 38
    std::uint32_t login_throttle_streak_ = 0;   // 39
    bool in_gate_wait_ = false;                 // 40  gate re-entrancy guard
    std::chrono::steady_clock::time_point start_time_{};  // 41  net-time base
    float pos_x_ = 0.0f;                        // 42  PIXELS
    float pos_y_ = 0.0f;                        // 43  PIXELS
    LocalPlayer local_;                         // 44
    std::unordered_map<std::uint32_t, Player> players_;  // 45 keyed by net_id
    nxrth::world::Inventory inventory_;        // 46
    std::unordered_set<std::uint16_t> equipped_items_;  // 47

    // Place confirmation: a block/seed/bg is only removed from the inventory once
    // the server echoes a tile update for the placed cell showing our item. Until
    // then the place is "pending" and its item is "reserved" so we never send more
    // places than we own; unconfirmed places (lag, or no build access) expire from
    // the run() loop and release their reservation WITHOUT consuming an item.
    struct PendingPlace {
        std::uint32_t x = 0, y = 0;
        std::uint16_t item_id = 0;
        std::chrono::steady_clock::time_point deadline;
    };
    std::vector<PendingPlace> pending_places_;
    std::unordered_map<std::uint16_t, std::uint32_t> place_reserved_;  // item_id -> in-flight
    std::optional<nxrth::world::World> world_;  // 48
    std::optional<nxrth::net::PeerId> peer_id_;  // 49
    std::shared_ptr<SharedBotState> state_;     // 50  UI/fleet mirror
    CmdReceiver cmd_rx_;                         // 51
    TemporaryData temporary_data_;              // 52
    bool auto_collect_ = true;                  // 53
    bool auto_reconnect_ = true;                // 54
    std::uint8_t collect_radius_tiles_ = 3;     // 55  1..5
    std::unordered_set<std::uint16_t> collect_blacklist_;  // 56
    std::chrono::steady_clock::time_point collect_timer_{};  // 57
    std::chrono::steady_clock::time_point ping_timer_{};
    std::chrono::steady_clock::time_point fleet_publish_timer_{};
    std::chrono::steady_clock::time_point automation_timer_{};
    nxrth::world::AStar astar_;                // 58
    std::optional<std::pair<std::uint32_t, std::uint32_t>> pathfind_target_;  // 59
    bool pathfind_recalc_ = false;              // 60
    BotDelays delays_;                          // 61
    std::shared_ptr<const nxrth::world::ItemsDat> items_dat_;  // 62
    // 63-66: Nxrth's Lua script channels -> native automation seam (NO Lua).
    std::shared_ptr<std::atomic<bool>> script_stop_ =
        std::make_shared<std::atomic<bool>>(false);            // 66 automation interrupt
    std::vector<std::unique_ptr<AutomationModule>> automation_modules_;
    std::unordered_map<std::string, bool> automation_module_enabled_;
    FleetHandle fleet_;                          // shared fleet registry
    std::optional<std::chrono::steady_clock::time_point> reconnect_after_;  // 67
    bool pending_2fa_ = false;                  // 68
    bool pending_relogon_ = false;              // 69
    bool pending_server_overload_ = false;      // 70
    bool pending_too_many_logins_ = false;      // 71
    bool pending_login_throttle_ = false;       // 72
    bool pending_place_prepare_ = false;        // 73
    bool pending_update_required_ = false;      // 74
    bool pending_maintenance_ = false;          // 75
    bool stop_requested_ = false;               // 76
    std::uint32_t bot_id_ = 0;                  // 77
    EventSinkPtr sink_;                          // 78  ws_tx -> ImGui event sink
    std::uint32_t last_ping_ = 0;               // 79
    std::optional<GeigerGreenRepeat> geiger_green_repeat_;  // 80
};

// ===========================================================================
// Free helper functions (spec 06 §2.1, 07 §1.10/§1.11). nxrth::bot namespace.
// ===========================================================================

// compute_klv(GAME_VER, PROTOCOL, rid, hash_as_i32) with DEFAULT_HASH fallback.
std::string default_klv(std::string_view rid, std::string_view hash);
// `def` if value.trim() is empty, else value (trim-empty check, not just empty).
std::string value_or_default(std::string value, std::string_view def);
// Fill every blank identity field with its default; klv from the resolved rid/hash.
nxrth::login::LoginIdentity resolve_login_identity(
    const std::optional<nxrth::login::LoginIdentity>& identity);
// Copy set to a vector, ascending sort (deterministic publish).
std::vector<std::uint16_t> sorted_blacklist_vec(const std::unordered_set<std::uint16_t>& set);
// Wall-clock UNIX epoch millis (saturating, 0 on error) — the ONE wall-clock use.
std::uint64_t now_millis();
// lowercase-contains "403" or "forbidden".
bool is_http_403_text(const std::string& message);
// Per line, redact the value of sensitive keys (token/ltoken/ubiticket/tankidpass/
// password/steamtok/steamtoken/fcmtoken) -> "{key}|<redacted>". Applied to EVERY
// logged/emitted packet body + the OnSpawn ltoken line.
std::string redact_packet_text(const std::string& text);
// True if the [Bot..]-stripped body starts with "GameUpdatePacket",
// "CallFunction: OnClearItemTransforms", or "PingReply sent" (file-log only).
bool is_high_frequency_noise(const std::string& msg);
// Strip a leading "[Bot..] " tag; return the body (or the whole msg).
std::string bot_msg_body(const std::string& msg);
// <= max chars -> as-is; else first max chars + "\n...<truncated>".
std::string truncate_text(const std::string& value, std::size_t max_chars);
// First non-blank line, trimmed, truncated to 140 chars.
std::string summarize_detail(const std::string& detail);
// "UbiTicket" if the token has >=3 dot-separated segments (>=2 dots), else "token".
const char* login_token_field(const std::string& token);
// Exact first gateway packet for Google OAuth/checktoken sessions.
std::string build_ltoken_gateway_packet(std::string_view token);
// The provider-ltoken protocol number for the first-gateway + redirect packets.
// Defaults to "225" (matches the current working reference client); overridable
// with NXRTH_PLTOKEN_PROTOCOL for empirical iteration.
std::string provider_login_protocol();
// PLAINTEXT clientData for /player/growid/checktoken: this device + the server_data
// meta. Load-bearing fields = rid/mac/vid + meta (klv/hash are accepted but not
// validated). Shared by the initial login and the reconnect re-exchange.
std::string build_checktoken_client_data(const nxrth::login::LoginIdentity& id,
                                         const std::string& meta, const std::string& protocol,
                                         bool oauth = false);
// First-gateway login packet for a provider validate-ltoken: the FULL client
// identity (game_version/meta/klv/hash/rid/mac/wk/vid + platformID|1) with the
// token in `ltoken|`. A bare/minimal packet (no game_version/meta/klv) is rejected
// by the gateway with "Fail to login. Please try again in 30 seconds."; the real
// client sends this full body and the gateway then issues an OnSendToServer redirect.
std::string build_provider_gateway_packet(const nxrth::login::LoginIdentity& id,
                                          std::string_view protocol, std::string_view meta,
                                          std::string_view ltoken);
// Subserver/redirect packet for a provider ltoken session — same identity body,
// carrying the redirect user/token/UUIDToken/doorID/aat instead of `ltoken|`.
std::string build_provider_redirect_packet(const nxrth::login::LoginIdentity& id,
                                           std::string_view protocol, std::string_view meta,
                                           const RedirectData& r);
struct LtokenRecord {
    enum class Kind { GoogleRefreshToken, ProviderToken };

    std::string token;
    std::string rid;
    std::string mac;
    std::string wk;
    std::optional<std::string> platform_id;
    std::optional<std::string> name;
    std::optional<std::string> cbits;
    std::optional<std::string> player_age;
    std::optional<std::string> vid;
    Kind kind = Kind::GoogleRefreshToken;
};
// Positional/refreshToken: and token: provider records are refresh tokens exchanged
// through checktoken. Callers choose whether that exchange uses the assigned game
// proxy or a pinned rotating-login exit.
std::optional<LtokenRecord> parse_ltoken_string(const std::string& s);
// "key|value\n..." -> map; split each line on the FIRST '|'; skip empty keys.
std::unordered_map<std::string, std::string> parse_pipe_map(const std::string& s);
// Resolve "host:port" to a SockEndpoint; nullopt on failure.
std::optional<SockEndpoint> resolve_endpoint(const std::string& host, std::uint16_t port);

}  // namespace nxrth::bot
