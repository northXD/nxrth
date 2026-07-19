// Nxrth — BotManager: the fleet supervisor (ported from Mori bot_manager.rs;
// port spec 09 §1.13-§3.17 + §5).
//
// Owns the whole fleet. Each running bot lives on its OWN OS thread (Bot::run is
// the thread body); the manager keeps a BotEntry per bot in an unordered_map
// keyed by a monotonic, never-reused u32 id. Each bot exposes three shared
// handles back: a stop flag (atomic bool), a live snapshot (SharedBotState) the
// bot writes / the UI reads, and a command queue the UI pushes actions into.
//
// The manager NEVER touches ENet/game logic — it only supervises lifecycle
// (spawn / stop / reap), fans out commands, exposes read views, and counts proxy
// occupancy. A crashing bot is caught per-thread so it never takes the fleet.
//
// Bots coordinate ONLY through the shared FleetState (handed to every bot at
// spawn, exactly like items_dat / sink) — never bot-to-bot directly.
//
// This is the object the ImGui UI drives. The BotManager has NO internal lock
// (faithful to Mori, which held it behind an outer axum lock): own it on the UI
// thread OR wrap it in a std::mutex — every public method mutates/iterates the
// manager-owned fields and must be serialized by the owner (spec §5.4). The
// per-bot SharedBotState / CommandQueue / FleetState carry their OWN finer locks.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bot/bot.h"           // nxrth::bot::{Bot, BotState, SharedBotState, CmdSender,
                               //   BotCommand, EventSinkPtr, FleetHandle, FleetState}
#include "proxy/proxy_pool.h"  // nxrth::proxy::RotatingLoginProxy

namespace nxrth::bot {

// §3.17 free function: proxy.map(|p| "<ip>:<port>"). The Socks5Config carried by
// a bot already holds the RESOLVED numeric IP in `host` (proxy_pool resolves it),
// so this yields the same capacity key used everywhere. None -> nullopt.
std::optional<std::string> proxy_key(const std::optional<Socks5Config>& proxy);

// ---------------------------------------------------------------------------
// §1.13 BotEntry — the manager's per-bot record (one per live bot).
// ---------------------------------------------------------------------------
struct BotEntry {
    std::string username;                            // "" for ltoken, "HAR_TOKEN_BOT" for HAR
    std::optional<std::string> proxy_key;            // "ip:port" capacity key, or nullopt
    std::shared_ptr<std::atomic<bool>> stop_flag;    // set true to ask the bot to stop
    std::shared_ptr<SharedBotState> state;           // shared live snapshot
    CmdSender cmd_tx;                                 // push commands to this bot
    // std::thread has no is_finished(): the thread body sets this true as its
    // very last action; the manager treats done->load() as JoinHandle::is_finished.
    std::shared_ptr<std::atomic<bool>> done;
    std::optional<std::thread> handle;               // the OS thread; take()n out to join
};

// ---------------------------------------------------------------------------
// §1.15 BotInfo — a flattened read-only view row for the UI table (from list()).
// ---------------------------------------------------------------------------
struct BotInfo {
    std::uint32_t id = 0;
    std::string username;   // BotEntry.username (NOT BotState.username)
    std::string status;     // to_string(BotState.status), e.g. "in_game"
    std::string world;      // BotState.world_name
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    std::int32_t gems = 0;
    std::uint32_t ping_ms = 0;
    std::optional<std::string> proxy_key;  // resolved game-proxy "ip:port", or nullopt
};

// How a bot's game connection proxy must be reconstructed when a trusted
// local fleet backup is restored. Pool deliberately stores no selected
// endpoint: restore asks the current ProxyPool for a fresh assignment.
enum class ProxyPolicy { Direct, Pool, Custom };

enum class LaunchCredentialKind { GrowId, Ltoken };

// Secret-bearing launch intent. This is intentionally separate from BotInfo:
// UI/MCP read models must never expose it. Only trusted local persistence code
// should call BotManager::launch_records().
struct LaunchRecord {
    std::uint32_t id = 0;
    LaunchCredentialKind credential_kind = LaunchCredentialKind::GrowId;
    std::string credential;  // exact GrowID/mail or exact provider record
    std::string password;    // exact password; empty for provider records
    ProxyPolicy proxy_policy = ProxyPolicy::Direct;
    std::optional<Socks5Config> custom_proxy;
    bool rotating_login_requested = false;
};

// ---------------------------------------------------------------------------
// §1.14 BotManager
// ---------------------------------------------------------------------------
class BotManager {
public:
    // §3.1 — loads ItemsDat ONCE (shared by all bots), constructs one shared
    // FleetState. sink may be null for a headless fleet.
    explicit BotManager(EventSinkPtr sink);
    ~BotManager();

    // Non-copyable, non-movable (owns threads + a FleetState).
    BotManager(const BotManager&) = delete;
    BotManager& operator=(const BotManager&) = delete;

    // Spawn variants. Standard GrowID and OAuth/provider ltoken are explicit;
    // credential strings are never auto-detected as another login method.
    std::uint32_t spawn(const std::string& username, const std::string& password,
                        std::optional<Socks5Config> proxy,
                        std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy,
                        ProxyPolicy proxy_policy = ProxyPolicy::Direct);
    // No dedup; keyed provider records may supply the entry's display name.
    std::uint32_t spawn_ltoken(const std::string& ltoken_str,
                               std::optional<Socks5Config> proxy,
                               std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy =
                                   std::nullopt,
                               ProxyPolicy proxy_policy = ProxyPolicy::Direct);

    // §3.8 — non-blocking stop: Disconnect + set atomic + park the handle in
    // retired_threads_ + emit BotRemoved immediately. Returns false if id absent.
    bool stop(std::uint32_t id);
    // §3.16 — stop the first bot whose username matches (ASCII case-insensitive).
    bool stop_by_name(const std::string& name);

    // §3.9 — snapshot rows for the UI table. Order unspecified (sort by id in UI).
    std::vector<BotInfo> list();

    // §3.11 — deep copy of one bot's whole BotState. nullopt if id absent.
    std::optional<BotState> get_state(std::uint32_t id) const;

    // §3.12 — enqueue a command. false if id unknown OR the consumer is gone.
    bool send_cmd(std::uint32_t id, BotCommand cmd) const;
    // §3.13 — thin wrapper: send_cmd(id, RunScript{content}).
    bool run_script(std::uint32_t id, std::string content) const;

    // §3.14 — first entry whose username == name (ASCII case-insensitive). Empty
    // name -> nullopt.
    std::optional<std::uint32_t> find_id_by_name(const std::string& name) const;
    // §3.15 — clones of (state handle, cmd_tx) so the caller can read/command a
    // bot without holding the manager. Does NOT early-return on empty name.
    std::optional<std::pair<std::shared_ptr<SharedBotState>, CmdSender>>
    find_by_name(const std::string& name) const;

    // §3.10 — proxy_key -> count of live bots behind it (nullopt keys skipped).
    // const: no reap (a just-stopped bot is already removed from `bots`).
    std::unordered_map<std::string, std::size_t> proxy_key_counts() const;

    // Trusted persistence surface. Values contain exact credentials and proxy
    // authentication data; never serialize them into logs or MCP responses.
    std::vector<LaunchRecord> launch_records() const;
    std::uint64_t launch_generation() const noexcept { return launch_generation_; }

    // The shared fleet registry (§5.3). Handed to every bot; also the UI's
    // bot-to-bot / proxy-occupancy source.
    const FleetHandle& fleet() const { return fleet_; }

    // Shared read-only item metadata (loaded once).
    const std::shared_ptr<const nxrth::world::ItemsDat>& items_dat() const { return items_dat_; }

    // Live bot registry, keyed by monotonic id (spec: pub). The owner already
    // serializes access to the manager, so this is a plain map.
    std::unordered_map<std::uint32_t, BotEntry> bots;

private:
    // A stopped-by-user thread awaiting a non-blocking join (§3.2 Phase B).
    struct RetiredThread {
        std::thread handle;
        std::shared_ptr<std::atomic<bool>> done;
    };

    // The factory the spawn core calls on the new thread to build the Bot. Fixed
    // signature so all five Bot::create_* variants funnel through one core.
    using BotFactory = std::function<std::unique_ptr<Bot>(
        std::optional<Socks5Config>, std::optional<nxrth::proxy::RotatingLoginProxy>,
        std::shared_ptr<std::atomic<bool>>, std::shared_ptr<SharedBotState>, CmdReceiver,
        std::shared_ptr<const nxrth::world::ItemsDat>, std::uint32_t, EventSinkPtr, FleetHandle)>;

    // §3.2 — the two-phase non-blocking garbage collector (called at the top of
    // nearly every public method).
    void reap_finished();

    // The shared spawn body (§3.3 steps 1,3-12 minus the factory selection).
    // do_dedup + dedup_name control the find_id_by_name skip; entry_username is
    // both the BotEntry.username and the BotAdded username.
    std::uint32_t spawn_core(std::string entry_username, bool do_dedup,
                             const std::string& dedup_name,
                             std::optional<Socks5Config> proxy,
                             std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy,
                             LaunchRecord launch_record, BotFactory make_bot);

    std::uint32_t next_id_ = 0;                       // monotonic, never reused
    std::vector<RetiredThread> retired_threads_;      // awaiting non-blocking join
    std::shared_ptr<const nxrth::world::ItemsDat> items_dat_;  // loaded once
    EventSinkPtr sink_;                               // ws_tx -> in-process UI notifier
    FleetHandle fleet_;                               // shared fleet registry
    std::unordered_map<std::uint32_t, LaunchRecord> launch_records_;
    std::uint64_t launch_generation_ = 0;
};

}  // namespace nxrth::bot
