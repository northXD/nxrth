// Nxrth — per-bot shared snapshot + command surface (ported from Nxrth
// bot_state.rs + player.rs; port spec 09 §1). This is the read-mostly mirror the
// bot's worker thread WRITES and the UI/fleet layer READS, plus the UI->bot
// command channel and the player value types.
//
// GT positions are stored in PIXELS on the Bot; everything in BotState / UI is in
// TILES (pixels / 32.0). No Growtopia wire format lives here.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "world/world.h"  // nxrth::world::TileType

namespace nxrth::bot {

// ---------------------------------------------------------------------------
// §1.1 BotStatus — the fleet status column. Display strings are load-bearing
// (the UI matches them verbatim). Default = Connecting.
// ---------------------------------------------------------------------------
enum class BotStatus {
    Connecting,       // "connecting" (default)
    Connected,        // "connected"  ENet up, not yet in a world
    InGame,           // "in_game"
    TwoFactorAuth,    // "two_factor_auth"
    ServerOverloaded, // "server_overloaded"
    TooManyLogins,    // "too_many_logins"
    UpdateRequired,   // "update_required" (bot stopped, no retry)
    Maintenance,      // "maintenance"
};
const char* to_string(BotStatus s);

// ---------------------------------------------------------------------------
// §1.2/§1.3 Geiger
// ---------------------------------------------------------------------------
enum class GeigerArea { Red, Yellow, Green, RapidGreen, Prize };
const char* as_str(GeigerArea a);

struct GeigerSignal {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    GeigerArea area_type = GeigerArea::Red;  // serialized JSON key "type"
    std::uint64_t timestamp_ms = 0;
};

// ---------------------------------------------------------------------------
// §1.4-§1.8 UI value snapshots
// ---------------------------------------------------------------------------
struct TileInfo {
    std::uint16_t fg_item_id = 0;
    std::uint16_t bg_item_id = 0;
    std::uint16_t flags = 0;
    nxrth::world::TileType tile_type;  // default Basic
};

struct PlayerInfo {
    std::uint32_t net_id = 0;
    std::string name;
    float pos_x = 0.0f;  // TILES
    float pos_y = 0.0f;  // TILES
    std::string country;
};

struct InvSlot {
    std::uint16_t item_id = 0;
    std::uint8_t amount = 0;
    bool is_active = false;
    std::uint8_t action_type = 0;
};

struct WorldObjectInfo {
    std::uint32_t uid = 0;
    std::uint16_t item_id = 0;
    float x = 0.0f;  // PIXELS
    float y = 0.0f;  // PIXELS
    std::uint8_t count = 0;
};

struct TrackInfo {
    std::uint32_t level = 0;
    std::uint64_t grow_id = 0;
    std::uint64_t install_date = 0;
    std::uint64_t global_playtime = 0;
    std::uint32_t awesomeness = 0;
};

// ---------------------------------------------------------------------------
// §1.9 BotDelays — the ONLY type (de)serialized to disk. Preserve defaults.
// ---------------------------------------------------------------------------
struct BotDelays {
    std::uint64_t place_ms = 500;
    std::uint64_t walk_ms = 500;
    std::uint64_t twofa_secs = 120;
    std::uint64_t server_overload_secs = 30;
    std::uint64_t too_many_logins_secs = 5;
    std::uint64_t maintenance_secs = 600;
};

// ---------------------------------------------------------------------------
// §1.10 BotState — the shared per-bot snapshot (wrapped in SharedBotState).
// Keep the three non-trivial defaults (auto_collect / collect_radius_tiles /
// auto_reconnect) — a zeroed struct would silently disable auto behaviours.
// ---------------------------------------------------------------------------
struct BotState {
    BotStatus status = BotStatus::Connecting;
    std::string username;
    std::string mac;
    std::string world_name;
    float pos_x = 0.0f;  // TILES
    float pos_y = 0.0f;  // TILES
    std::uint32_t world_width = 0;
    std::uint32_t world_height = 0;
    std::vector<TileInfo> tiles;  // row-major width*height when in-world
    std::vector<WorldObjectInfo> objects;
    std::vector<PlayerInfo> players;
    std::vector<InvSlot> inventory;
    std::uint32_t inventory_size = 0;
    std::int32_t gems = 0;
    std::deque<std::string> console;  // per-bot SYSTEM log ring (Logs tab), cap 300
    std::deque<std::string> chat;     // in-game console/chat ring (Console tab), cap 300
    std::uint64_t chat_base_index = 0; // absolute index of chat.front(); survives ring eviction
    std::deque<std::string> traffic;  // incoming/outgoing packet log (Traffic tab), cap 300
    bool traffic_enabled = false;     // gates packet capture; off by default (opt-in)
    bool logs_enabled = false;        // gates the per-bot SYSTEM log ring; off by default (opt-in)
    std::uint32_t ping_ms = 0;
    BotDelays delays;
    std::optional<TrackInfo> track_info;
    std::optional<GeigerSignal> geiger_signal;
    bool auto_collect = true;
    std::uint8_t collect_radius_tiles = 3;  // 1..5
    std::vector<std::uint16_t> collect_blacklist;  // sorted+unique
    bool auto_reconnect = true;
};

// SharedBotState — the shared_mutex-guarded BotState (Rust RwLock<BotState>).
// Many readers (UI/fleet), one writer (the bot thread). No poisoning in C++.
class SharedBotState {
public:
    template <class F>
    void write(F&& f) {
        std::unique_lock<std::shared_mutex> lk(mtx_);
        f(state_);
    }
    template <class F>
    auto read(F&& f) const {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        return f(state_);
    }
    BotState snapshot() const {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        return state_;
    }

private:
    mutable std::shared_mutex mtx_;
    BotState state_;
};

// ---------------------------------------------------------------------------
// player.rs — LocalPlayer (self) + Player (others). Positions in PIXELS.
// ---------------------------------------------------------------------------
struct LocalPlayer {
    std::uint32_t net_id = 0;
    std::uint32_t user_id = 0;
    std::uint32_t hack_type = 0;    // SetCharacterState.value; echoed as ping net_id in-world
    std::uint8_t build_length = 0;  // SetCharacterState.jump_count - 126 (saturating)
    std::uint8_t punch_length = 0;  // SetCharacterState.animation_type - 126 (saturating)
    float gravity = 0.0f;           // SetCharacterState.vector_x2
    float velocity = 0.0f;          // SetCharacterState.vector_y2
};

struct Player {
    std::uint32_t net_id = 0;
    std::uint32_t user_id = 0;
    std::string name;
    std::string country;
    std::pair<float, float> position{0.0f, 0.0f};  // PIXELS
    std::string avatar;
    std::string online_id;
    std::string e_id;       // key "eid"
    std::string ip;
    std::string col_rect;   // key "colrect"
    std::string title_icon; // key "titleIcon"
    std::uint32_t m_state = 0;  // key "mstate"
    bool invisible = false;     // key "invis" != 0
};

// ---------------------------------------------------------------------------
// §1.11 BotCommand — the UI -> bot message (internal only, no serde).
// ---------------------------------------------------------------------------
namespace cmd {
struct Move { std::int32_t x = 0, y = 0; };
struct WalkTo { std::uint32_t x = 0, y = 0; };
struct RunScript { std::string content; };
struct StopScript {};
struct Say { std::string text; };
struct Warp { std::string name, id; };
struct Disconnect {};
struct Reconnect {};
struct Place { std::int32_t x = 0, y = 0; std::uint32_t item = 0; };
struct Hit { std::int32_t x = 0, y = 0; };
struct Wrench { std::int32_t x = 0, y = 0; };
struct Wear { std::uint32_t item_id = 0; };
struct Unwear { std::uint32_t item_id = 0; };
struct Drop { std::uint32_t item_id = 0, count = 0; };
struct Trash { std::uint32_t item_id = 0, count = 0; };
struct LeaveWorld {};
struct Respawn {};
struct FindPath { std::uint32_t x = 0, y = 0; };
struct SetDelays { BotDelays delays; };
struct SetAutoCollect { bool enabled = false; };
struct SetCollectConfig { std::uint8_t radius_tiles = 3; std::vector<std::uint16_t> blacklist; };
struct SetAutoReconnect { bool enabled = false; };
struct AcceptAccess {};
struct CollectObject { std::uint32_t uid = 0; float range_tiles = 3.0f; };
struct CollectNearby {};
struct ActivateItem { std::uint32_t item_id = 0; };
struct ActivateTile { std::int32_t x = 0, y = 0; };
struct Enter { std::optional<std::string> password; };
struct Face { bool left = false; };
struct WrenchPlayer { std::uint32_t net_id = 0; };
struct SetTrafficLog { bool enabled = false; };  // per-bot incoming/outgoing packet capture
struct SetLogging { bool enabled = false; };     // per-bot SYSTEM log ring capture (Logs tab)
}  // namespace cmd

using BotCommand =
    std::variant<cmd::Move, cmd::WalkTo, cmd::RunScript, cmd::StopScript, cmd::Say, cmd::Warp,
                 cmd::Disconnect, cmd::Reconnect, cmd::Place, cmd::Hit, cmd::Wrench, cmd::Wear,
                 cmd::Unwear, cmd::Drop, cmd::Trash, cmd::LeaveWorld, cmd::Respawn, cmd::FindPath,
                 cmd::SetDelays, cmd::SetAutoCollect, cmd::SetCollectConfig, cmd::SetAutoReconnect,
                 cmd::AcceptAccess, cmd::CollectObject, cmd::CollectNearby, cmd::ActivateItem,
                 cmd::ActivateTile, cmd::Enter, cmd::Face, cmd::WrenchPlayer, cmd::SetTrafficLog,
                 cmd::SetLogging>;

// ---------------------------------------------------------------------------
// §3.14 CommandQueue<T> — MPSC, unbounded, closable. Replaces std::mpsc /
// crossbeam. Producers: UI/manager; consumer: the bot thread.
// ---------------------------------------------------------------------------
template <class T>
class CommandQueue {
public:
    // Producer. false == the consumer side closed (Rust send Err).
    bool try_send(T v) {
        std::lock_guard<std::mutex> lk(m_);
        if (consumer_closed_) return false;
        q_.push_back(std::move(v));
        cv_.notify_one();
        return true;
    }
    // Consumer: non-blocking drain (the per-tick try_recv loop).
    std::optional<T> try_recv() {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front());
        q_.pop_front();
        return v;
    }
    // Consumer: block up to `timeout` for one item.
    std::optional<T> recv_timeout(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, timeout, [&] { return !q_.empty(); });
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front());
        q_.pop_front();
        return v;
    }
    // Call when the bot thread exits so late try_sends fail rather than leak.
    void close_consumer() {
        std::lock_guard<std::mutex> lk(m_);
        consumer_closed_ = true;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<T> q_;
    bool consumer_closed_ = false;
};

using CmdSender = std::shared_ptr<CommandQueue<BotCommand>>;
using CmdReceiver = std::shared_ptr<CommandQueue<BotCommand>>;

// ---------------------------------------------------------------------------
// EventSink — the in-process notifier that replaces Nxrth's WsTx broadcaster
// (ARCHITECTURE: "not websocket frames"). The bot mirrors most state into
// BotState (UI polls it) and calls these for the explicit event surfaces.
// Every override is optional (default no-op) so a headless fleet needs no sink.
// ---------------------------------------------------------------------------
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void bot_added(std::uint32_t bot_id, const std::string& username) {}
    virtual void bot_removed(std::uint32_t bot_id) {}
    virtual void console(std::uint32_t bot_id, const std::string& message) {}
    virtual bool wants_traffic(std::uint32_t bot_id) const {
        (void)bot_id;
        return false;
    }
    virtual void traffic(std::uint32_t bot_id, const std::string& direction,
                         const std::string& kind, std::size_t size, const std::string& summary,
                         const std::string& detail, std::uint64_t timestamp_ms) {}
    // BotStatus/BotWorld/BotMove/BotGems/BotPing/Player*/Tile*/Inventory* are all
    // reflected in BotState; a dirty tick tells the UI to re-read that bot.
    virtual void dirty(std::uint32_t bot_id) {}
};
using EventSinkPtr = std::shared_ptr<EventSink>;

}  // namespace nxrth::bot
