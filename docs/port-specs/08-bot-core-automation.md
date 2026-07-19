# Port Spec 08 — Bot Core Automation (in-world handlers, collect, geiger, pathfind, script bridge)

Source of truth for the C++ (Nxrth) reimplementation of the in-world automation layer of the Rust bot (Mori).
Derived from: `src/bot/core.rs` (~lines 1514–1635, 1826–1927, 2286–2393, 2396–3230, 3232–4970), `src/script_channel.rs`, `src/events.rs`, `src/bot/shared.rs`, `src/player.rs`.

This module owns everything that happens **after** a bot is logged in and connected: parsing world/tile/object/inventory/bux packets, tracking self + other players, auto-collecting drops, decoding Geiger signals, pathfinding/warping, and the request/reply bridge that the (now-removed) Lua layer used to drive the bot. In Nxrth the Lua seam becomes a **native in-engine automation module** — this spec describes the seams precisely so native code plugs into the same points.

Types like `GameUpdatePacket`, `GamePacketType`, `PacketFlags`, `VariantList`, `World`/`WorldObject`/`TileType`, `Inventory`, and `BotState`/`BotDelays`/`BotCommand`/`InvSlot` are defined by the protocol / world / inventory / bot-state port specs. Here they are described **only as used**; cross-reference those specs for their exact wire/on-disk layout.

---

## 0. RENAME RULES (apply globally before anything else)

Apply these substitutions to every identifier, string literal, comment, file path, log line, window title, user-agent, and config filename produced from this module:

| From | To |
|------|-----|
| `Mori` | `Nxrth` |
| `mori` | `nxrth` |
| `Cloei` | `North` |
| `cloei` | `north` |

Concrete occurrences spotted in the files covered by this spec:

- `src/bot/core.rs:167` — comment on `LoginMethod::Newly`: *"Original **Mori**-style GrowID login without HAR fallbacks."* → *"Original **Nxrth**-style GrowID login..."*.
- No literal `Cloei`/`cloei` appears in these five files. The upstream-author rename still applies to any that surface in sibling modules (auth, server_data, constants — the user-agent and dashboard strings live there, covered by other specs).
- The console/log tag emitted by this module is **`[Bot]`** (rewritten to `[Bot#<id>]` at log time — see `log_console`). This is *not* a "Mori" string; keep it as `[Bot]` / `[Bot#<id>]`. Do **not** invent `[Mori]`.
- File-log/window-title/user-agent/config-filename `Mori` references are produced by `logger.rs`, the UI shell, and `constants.rs` (not in this module) — rename them there.

---

## 1. DEPENDENCY MAPPING (Rust crate → Nxrth C++)

This module itself pulls in relatively few external crates; most of its work is pure logic over already-parsed types. Mapping for what it touches:

| Rust (Mori) | Nxrth C++ | Notes for this module |
|---|---|---|
| `crossbeam_channel` (`Sender`/`Receiver`, `unbounded`, `bounded(256)`, `try_recv`, `try_send`) | `std::mutex + std::condition_variable` bounded/unbounded queue (project's `Channel<T>`) | Used for the script request/reply/event bridge. `event_tx` is **bounded at 256** (drop-on-full via `try_send(...).ok()`); the two script channels are **unbounded**. Preserve: request/reply are 1:1 synchronous-style; events are fire-and-forget best-effort. |
| `mlua` (Lua VM, `run_script_threaded`) | **Native C++ automation module — NO Lua in Nxrth** | The Lua thread was the sole consumer of `ScriptRequest`/`ScriptReply`/`BotEventRaw`. Replace with a native automation object running on its own `std::thread`, driven by the same three queues. See §8. |
| `rusty_enet` (`Host`, `Peer`, `Packet::reliable/unreliable`, `EventNoRef`) | vendored C ENet patched for SOCKS5-UDP | `send_game_packet`/`send_text`/`send_game_message` push on channel 0. `Packet::reliable` vs `Packet::unreliable` maps to ENET_PACKET_FLAG_RELIABLE / 0. |
| `tokio::sync::broadcast::Sender<WsEvent>` (`WsTx`) | **NO web server.** In-process observer/event bus feeding Dear ImGui + the fleet shared-state manager | Every `emit(WsEvent::…)` is a UI/telemetry event. Fan-out becomes a `std::mutex`-guarded subscriber list (or direct writes into shared UI state). Keep the same event variants and fields (§4.6). |
| `serde` / `serde_json` (`#[derive(Serialize)]` on `WsEvent`/`WsTile`/`WsObject`/`WsInvItem`) | `nlohmann/json` only where JSON is still needed (e.g. persisting/logging); UI can read structs directly | These derives exist only because events were JSON-serialized to the browser. Native ImGui reads the structs; JSON optional. |
| `std::sync::{Arc, RwLock, Mutex, OnceLock, atomic}` | `std::shared_ptr`, `std::shared_mutex`, `std::mutex`, function-local `static` + `std::once_flag`, `std::atomic` | `state: Arc<RwLock<BotState>>` → `shared_ptr<BotState>` guarded by `shared_mutex`. `OnceLock<Mutex<Instant>>` fleet gates → function-local `static std::mutex`-guarded `steady_clock::time_point` (§3). Rust `unwrap_or_else(PoisonError::into_inner)` = "ignore lock poisoning"; C++ has no poisoning, just lock normally. |
| `std::time::{Instant, Duration, SystemTime}` | `std::chrono::steady_clock` (durations/deadlines), `system_clock` (`now_millis` epoch ms) | `now_millis()` = ms since UNIX epoch (used for Geiger timestamps + traffic timestamps). |

Not used by this module (handled elsewhere): `ureq`/`reqwest`→libcurl, `scraper`→regex, `md5`/`argon2`, `axum`/`tower`.

---

## 2. THREADING & SHARED STATE

**Per-bot worker thread.** Each `Bot` runs its whole life on one dedicated thread via `Bot::run(stop_flag)`. That thread owns the ENet host, all world/inventory/player state, and calls all handlers in this spec. Nothing here is internally parallel — the handlers assume single-threaded access to `self`.

**`run()` main loop** (each iteration, `~10 ms` cadence — `sleep(10ms)` at the tail):
1. Check `stop_flag` (shared `Arc<AtomicBool>`) and internal `stop_requested`; break → `shutdown()`.
2. If `reconnect_after` deadline reached and `auto_reconnect`, call `reconnect_main(refresh)`.
3. Drain the **UI command channel** `cmd_rx` (`while try_recv → handle_command(cmd)`).
4. If a peer exists, read RTT → write `state.ping_ms`; emit `BotPing` only when it changed (`last_ping` dedup).
5. `service_once()` — drain **all** pending ENet events (see §5), running handlers.
6. **Login watchdog**: if `connected_since` set, `world` is `None`, and elapsed ≥ 30 s → rotate game proxy, clear redirect, force clean gateway reconnect (login path — see spec 07; included here only because it shares the loop).
7. `drain_script_requests()` — service the automation/script bridge (§6).
8. **Auto-collect tick**: if `auto_collect` and `collect_timer` elapsed ≥ **500 ms** → reset timer, `collect()`.
9. `sleep(10ms)`.

**Shared state written by the worker, read by UI / other threads:**
- `state: Arc<RwLock<BotState>>` — canonical mirror of position (in **tiles** = px/32), players, tiles, objects, inventory, gems, ping, world_name, status, console ring buffer (last 100 lines), track_info, geiger_signal, delays, auto_collect/auto_reconnect flags, collect config, mac, username. Every handler that mutates bot state writes the relevant slice here under a short write-lock, then releases before doing I/O.
- `ws_tx: Option<WsTx>` — broadcast/observer bus for real-time UI events (`emit`).
- `event_tx`/`script_req_rx`/`script_reply_tx` — the automation bridge (see §6, §8).
- `script_stop: Arc<AtomicBool>` — interrupts the running automation.

**Fleet-wide shared state (bots must be aware of each other — critical for Nxrth):**
- **Global pacing gates** are `static` singletons shared by *every* bot thread (§3). This module uses `WARP_GATE` (via `warp()`). The gate ensures the whole fleet warps in single file. Nxrth must keep these as process-global statics, not per-bot.
- `next_object_uid` lives on the per-bot `World` (not fleet-global); object UIDs are per-world, monotonically assigned client-side on each drop.
- Auto-collect and geiger state are per-bot, but the Geiger automation (spec elsewhere) reads each bot's `state.geiger_signal` across the fleet to coordinate — so `GeigerSignal` in `BotState` is the fleet coordination surface for radiation hunting.

---

## 3. GLOBAL PACING GATES (fleet-wide)

Declared as `static OnceLock<Mutex<Instant>>`; in C++ use a function-local `static std::mutex`-guarded `steady_clock::time_point` per gate.

Relevant to this module:
- `WARP_GATE` with `WARP_STAGGER_MS = 1200` — used by `warp()`.
- `GATE_CONNECTED_MAX_AHEAD_MS = 2500` — cap passed to `reserve_gate_slot` when the bot is *connected* (don't reserve a slot more than 2.5 s in the future; beyond that, proceed rather than starve ENet).

`wait_for_global_gate(gate, spacing_ms, label)` behavior (must be reproduced for `warp`):
1. `slot = reserve_gate_slot(gate, spacing_ms, GATE_CONNECTED_MAX_AHEAD_MS)` — atomically reserve the next fleet slot spaced `spacing_ms` after the previous reservation (capped `max_ahead` in the future).
2. If `slot <= now` → return immediately.
3. Compute `waited_ms`; if ≥ 50 ms, log `"[Bot] login pacing: waiting ~{waited_ms}ms before {label} (keeping ENet serviced)"`.
4. **Critical at scale**: this wait happens while the bot is CONNECTED. Do **not** dead-sleep the whole time or the server times out the ENet connection. Re-entrancy guard `in_gate_wait`:
   - If already inside a gate wait → plain `sleep(waited_ms)` (no nested servicing).
   - Else set `in_gate_wait=true`; loop until `now >= slot`: bail if `stop_requested`/`stop`; else `service_once()` + `sleep(10ms)`. Clear the guard.

Other gates (`LOGIN_PACKET_GATE`, `ENTER_GAME_GATE`, `HTTP_LOGIN_GATE`, `GATEWAY_LOGON_GATE`, `DASHBOARD_GATE`) belong to the login/connect spec; keep them process-global.

---

## 4. TYPES

All integers little-endian on the wire (GT convention). Positions are stored in **pixels** internally; `BotState` and all UI events use **tiles** (`pixels / 32.0`). The pixel↔tile constant `32.0` is load-bearing everywhere.

### 4.1 `Socks5Config` (`bot/shared.rs`)
```
struct Socks5Config {                 // #[derive(Clone, Debug)]
    proxy_addr: SocketAddr,           // host:port of the SOCKS5 proxy
    username: Option<String>,
    password: Option<String>,
}
```
- `to_url() -> String`: if both `username` and `password` are `Some` → `"socks5://{u}:{p}@{proxy_addr}"`; otherwise → `"socks5://{proxy_addr}"`. (Note: partial creds — only user or only pass — fall through to the no-auth form.)
- Free function `login_http_proxy_url(game_proxy: Option<&Socks5Config>, login_proxy_url: Option<&str>) -> Option<String>`:
  1. Take `login_proxy_url`, `trim()`, drop if empty → use it as-is if non-empty.
  2. Else fall back to `game_proxy.map(to_url)`.
  - i.e. an explicit HTTP/login proxy URL wins; otherwise reuse the game SOCKS5 proxy as the login proxy.

C++: `struct Socks5Config { std::string host; uint16_t port; std::optional<std::string> username, password; std::string to_url() const; };` For libcurl use the `socks5h://` scheme (remote DNS) for the login proxy per project convention.

### 4.2 `BotEventRaw` (`bot/shared.rs`)
Raw event pushed onto the automation event queue (`event_tx`) by packet handlers; drained by the automation loop to fire registered callbacks. Was consumed by Lua's `listenEvents`; in Nxrth consumed by native automation (§8).
```
enum BotEventRaw {
    VariantList { vl: VariantList, net_id: u32 },   // from every CallFunction packet
    GameUpdate  { pkt: GameUpdatePacket },          // from every GameUpdate packet
    GameMessage { text: String },                   // from every inbound GameMessage packet
}
```
C++: `std::variant`/tagged struct. `net_id` is the ENet peer id cast to u32 (`id.0 as u32`).

### 4.3 `TemporaryData` + dialog callback (`bot/shared.rs`)
```
type DialogCallback = Box<dyn FnOnce(&mut Bot) + Send>;   // one-shot
struct TemporaryData { dialog_callback: Mutex<Option<DialogCallback>> }   // Default = None
```
- A single pending one-shot callback fired on the **next** `OnDialogRequest`, then cleared. Used to implement multi-step server dialog flows (drop/trash/accept-access).
- C++: `std::mutex` + `std::optional<std::function<void(Bot&)>>`. On `OnDialogRequest`, atomically `take()` the callback (move out, leaving `nullopt`) and invoke it with `*this`. Callbacks may install the *next* callback (chained dialogs — see `accept_access`).

### 4.4 `Player` and `LocalPlayer` (`player.rs`)
```
struct LocalPlayer {                  // #[derive(Debug, Default, Clone)] — the bot's own identity
    net_id: u32,
    user_id: u32,
    hack_type: u32,      // SetCharacterState.value — net_id to echo in ping reply
    build_length: u8,    // SetCharacterState.jump_count - 126  (tile X-ish; used in ping reply)
    punch_length: u8,    // SetCharacterState.animation_type - 126
    gravity: f32,        // SetCharacterState.vector_x2
    velocity: f32,       // SetCharacterState.vector_y2
}

struct Player {                        // #[derive(Debug, Clone)] — another player in the world
    net_id: u32,
    user_id: u32,
    name: String,
    country: String,
    position: (f32, f32),  // PIXELS
    avatar: String,
    online_id: String,
    e_id: String,          // from key "eid"
    ip: String,
    col_rect: String,      // from key "colrect"
    title_icon: String,    // from key "titleIcon"
    m_state: u32,          // from key "mstate"
    invisible: bool,       // key "invis" != 0
}
```
- `parse_pipe_map(s: &str) -> HashMap<String,String>`: split into lines; for each line `splitn(2,'|')` → `key = parts[0].trim()`, `val = parts[1]` (or `""`). Skip lines whose trimmed key is empty. Collect into a map (later duplicate keys overwrite earlier). This parses GT's `key|value\n…` blobs used by `OnSpawn`/`OnRemove`. C++: split on `'\n'`, then split each line at the first `'|'`.

### 4.5 Script bridge types (`script_channel.rs`)

`WorldSnapshot` — deep copy handed to the automation thread on `GetWorld`:
```
struct WorldSnapshot { world: World, players: Vec<Player>, local_net_id: u32,
                       local_user_id: u32, local_name: String, local_pos: (f32,f32) /*px*/ }
```
`LocalSnapshot` — identity snapshot on `GetLocal`:
```
struct LocalSnapshot { net_id: u32, user_id: u32, pos_x: f32, pos_y: f32 /*px*/, username: String, mac: String }
```

`ScriptRequest` (automation → bot). Full variant list (exact names + payloads):
```
// Network / connection
Reconnect
Disconnect
SendRaw { pkt: GameUpdatePacket }
SendPacket { ptype: u8, text: String }        // ptype 2 = text packet, 3 = game message
// World actions
Say { text: String }
Warp { name: String, id: String }
LeaveWorld
Respawn
Active { tile_x: i32, tile_y: i32 }
Enter { pass: Option<String> }
// Tile actions
Place { x: i32, y: i32, item: u32 }           // x,y are OFFSETS from bot tile
Hit { x: i32, y: i32 }                         // punch offset
Wrench { x: i32, y: i32 }                       // ABSOLUTE tile (wrench_at)
WrenchPlayer { net_id: u32 }
// Inventory
Wear { item_id: u32 }
Unwear { item_id: u32 }
Drop { item_id: u32, count: u32 }
Trash { item_id: u32, count: u32 }
FastDrop { item_id: u32, count: u32 }
FastTrash { item_id: u32, count: u32 }
// Movement
Walk { tile_x: i32, tile_y: i32 }
SetDirection { facing_left: bool }
FindPath { x: u32, y: u32 }                     // absolute tile; blocking walk
// Object collection
CollectObject { uid: u32, range: f32 }          // range in TILES
Collect { range: f32, interval_ms: u64 }        // range in TILES; circular
// State mutation
SetMac { mac: String }
SetAutoCollect { enabled: bool }
SetPlaceDelay { ms: u64 }
SetWalkDelay { ms: u64 }
// Queries — bot replies with data
GetWorld
GetInventory
IsWearing { item_id: u16 }
GetLocal
GetPath { x: u32, y: u32 }
IsInWorld { name: Option<String> }
IsInTile { x: u32, y: u32 }
GetAutoCollect
GetPing
GetGems
GetPlaceDelay
GetWalkDelay
```

`ScriptReply` (bot → automation):
```
Ack
Bool(bool)
U32(u32)
I32(i32)
World(Option<WorldSnapshot>)
Inventory(Inventory)
Local(LocalSnapshot)
Path(Vec<(u32,u32)>)
CollectCount(usize)
```
The request→reply mapping is fixed and total (§6). C++: two `std::variant`s (or tagged unions); the automation call site sends a request then blocks on the reply queue for the matching reply.

### 4.6 UI event types (`events.rs`)

These were `serde`-serialized to the browser. In Nxrth they feed the ImGui panels + fleet manager; keep field names/semantics, drop the JSON transport.

```
struct WsTile   { fg: u16, bg: u16, flags: u16, tile_type: TileType }
struct WsObject { uid: u32, item_id: u16, x: f32, y: f32, count: u8 }   // x,y PIXELS
struct WsInvItem{ item_id: u16, amount: u8, is_active: bool, action_type: u8 }
```

`WsEvent` (tagged enum; JSON shape was `{event, data}`). Variants + exact fields (all carry `bot_id: u32`):
- `BotAdded { bot_id, username }`
- `BotRemoved { bot_id }`
- `BotStatus { bot_id, status: String }` — status strings used by this module: `"in_game"`, `"connected"`, `"connecting"`, `"two_factor_auth"`, `"server_overloaded"`, `"too_many_logins"`, `"maintenance"`.
- `BotWorld { bot_id, world_name }` — empty string = left world.
- `BotMove { bot_id, x: f32, y: f32 }` — TILE coords.
- `BotGems { bot_id, gems: i32 }`
- `BotPing { bot_id, ping_ms: u32 }` — emitted only on change.
- `PlayerSpawn { bot_id, net_id, name, country, x, y }` — x,y TILES.
- `PlayerMove { bot_id, net_id, x, y }` — TILES.
- `PlayerLeave { bot_id, net_id }`
- `WorldLoaded { bot_id, name, width: u32, height: u32, tiles: Vec<WsTile> }`
- `TileUpdate { bot_id, x: u32, y: u32, fg: u16, bg: u16 }`
- `ObjectsUpdate { bot_id, objects: Vec<WsObject> }` — full object set each time.
- `InventoryUpdate { bot_id, gems: i32, inventory_size: u32, items: Vec<WsInvItem> }`
- `Console { bot_id, message }`
- `BotTrackInfo { bot_id, level: u32, grow_id: u64, install_date: u64, global_playtime: u64, awesomeness: u32 }`
- `BotUsername { bot_id, username }` — from `SetHasGrowID`.
- `BotAutoCollect { bot_id, enabled: bool }`
- `BotDelays { bot_id, place_ms: u64, walk_ms: u64, twofa_secs: u64, server_overload_secs: u64, too_many_logins_secs: u64, maintenance_secs: u64 }`
- `Traffic { bot_id, direction: String, kind: String, size: usize, summary: String, detail: String, timestamp_ms: u64 }` — sanitized packet log for the traffic panel.

### 4.7 Bot fields relevant to this module (subset of `Bot`)

Reproduce these on the C++ `Bot` (full struct in the login/core spec):
```
pos_x: f32, pos_y: f32                     // PIXELS (self position)
local: LocalPlayer
players: HashMap<u32, Player>              // keyed by net_id
inventory: Inventory
equipped_items: HashSet<u16>               // locally cached "active" item ids
world: Option<World>                       // current world; None when out of world
peer_id: Option<PeerID>
state: Arc<RwLock<BotState>>
cmd_rx: CmdReceiver                        // UI command channel
temporary_data: TemporaryData
auto_collect: bool, auto_reconnect: bool
collect_radius_tiles: u8                    // clamped 1..=5
collect_blacklist: HashSet<u16>
collect_timer: Instant
astar: AStar                                // reused pathfinder
pathfind_target: Option<(u32,u32)>          // Some while routing
pathfind_recalc: bool                       // set by OnSetPos to force replan
delays: BotDelays                           // place_ms, walk_ms, twofa_secs, ...
items_dat: Arc<ItemsDat>                    // item db (collision_type, action_type)
event_tx: Option<Channel<BotEventRaw>::Sender>          // bounded 256
script_req_rx: Option<Channel<ScriptRequest>::Receiver> // unbounded
script_reply_tx: Option<Channel<ScriptReply>::Sender>   // unbounded
script_stop: Arc<AtomicBool>
bot_id: u32
ws_tx: Option<WsTx>
last_ping: u32
geiger_green_repeat: Option<GeigerGreenRepeat>
```
`GeigerGreenRepeat { x: u32, y: u32, last_seen_ms: u64, count: u8 }` (`#[derive(Clone,Copy)]`).

`GameUpdatePacket` fields referenced by this module (see protocol spec for exact wire layout/types):
`packet_type: GamePacketType`, `net_id: u32`, `target_net_id: u32`, `value: u32`, `int_x: i32`, `int_y: i32`, `vector_x: f32`, `vector_y: f32`, `vector_x2: f32`, `vector_y2: f32`, `float_variable: f32`, `object_type: u16`, `jump_count: u8`, `animation_type: u8`, `flags: PacketFlags`, `extra_data: Vec<u8>`. All non-set fields come from `Default::default()`.

`PacketFlags` bits used: `WALK`, `STANDING`, `PLACE`, `PUNCH`, `FACING_LEFT` (bitflags; `.set(flag, bool)` toggles). Exact bit values are in the protocol spec.

---

## 5. PACKET DISPATCH ENTRY POINT — `service_once()`

Single most important entry point. Drains **all** queued ENet events (`while let Some(event) = host.next_event()`), each of `Connect | Disconnect | Receive`. `Connect`/`Disconnect` handling is login/reconnect logic (spec 07); the in-world work is under `Receive`.

**Receive** → `IncomingPacket::parse(packet.data())` yields one of:
- `ServerHello` → `emit_traffic("in","server_hello",…)`, `on_server_hello()` (login).
- `Text(s)` → traffic + `log_console("[Bot] Text: {s}")`.
- `GameMessage(s)` → traffic; **push `BotEventRaw::GameMessage{ text }` to `event_tx` (try_send, best-effort)**; then substring-scan for login-failure/token triggers (spec 07: `action|request_token`, `Advanced Account Protection`, `SERVER OVERLOADED`, `Too many people logging in`, `Please try again in`/`Fail to login`, `Server couldn't prepare a place`, `Server requesting that you re-logon`, `UPDATE REQUIRED`, `undergoing maintenance`, `action|logon_fail`).
- `Track(s)` → traffic; if `Authentication_error|23` set `pending_place_prepare`; parse `Key|Value` lines into a map and read: `Level`(u32), `GrowId`(u64), `installDate`(u64), `Global_Playtime`(u64), `Awesomeness`(u32); store `state.track_info` and emit `BotTrackInfo`.
- `ClientLogRequest` → traffic + log.
- `GameUpdate(pkt)` → **the in-world core** (see below).
- `Unknown{msg_type,data}` → traffic + log.
- `None` (parse failure) → hex-dump traffic + log.

**GameUpdate(pkt) arm** (order matters):
1. `emit_traffic("in", "game_update:{packet_type:?}", size, format_game_packet_detail(pkt))`.
2. **Push `BotEventRaw::GameUpdate{ pkt: pkt.clone() }` to `event_tx`** (try_send best-effort) — the automation sees *every* game-update packet.
3. **`update_geiger_signal(&pkt)`** — geiger hook runs on **every** GameUpdate before type dispatch (§7).
4. `match pkt.packet_type`:
   - `SetCharacterState` → set `local.hack_type = value`; `local.build_length = jump_count.saturating_sub(126)`; `local.punch_length = animation_type.saturating_sub(126)`; `local.gravity = vector_x2`; `local.velocity = vector_y2`.
   - `CallFunction` → clone `extra_data`; `net_id = peer_id.0 as u32`; if `VariantList::deserialize(extra)` OK, **push `BotEventRaw::VariantList{ vl, net_id }` to `event_tx`**; then `on_call_function(id, &extra)` (§5.1).
   - `PingRequest` → `on_ping_request(pkt.value)` (§5.9).
   - `SendInventoryState` → `Inventory::parse(&extra_data)`: on OK, recompute `equipped_items` = ids whose `flag & 1 != 0`, replace `inventory`, `emit_inventory_update()`; on Err, log.
   - `SendMapData` → **world load** (§5.2).
   - `State` → `on_state(&pkt)` (§5.3).
   - `TileChangeRequest` → `on_tile_change(&pkt)` (§5.4).
   - `SendTileUpdateData` → `on_send_tile_update_data(&pkt)` (§5.5).
   - `SendTileUpdateDataMultiple` → `on_send_tile_update_data_multiple(&pkt)` (§5.6).
   - `SendTileTreeState` → `on_send_tile_tree_state(&pkt)` (§5.7).
   - `ModifyItemInventory` → `on_modify_item_inventory(&pkt)` (§5.8).
   - `ItemChangeObject` → `on_item_change_object(&pkt)` (§5.10).
   - `SendLock` → `on_send_lock(&pkt)` (§5.11).
   - default → `log_console("[Bot] {pkt}")`.

### 5.1 `on_call_function(id, extra_data)` — CallFunction / VariantList dispatch

Deserialize `VariantList::deserialize(extra_data)`; on error log `"[Bot] VariantList parse error: {e}"` and return. `fn_name = vl.get(0).as_string()` (default `""`); log `"[Bot] CallFunction: {fn_name}"`. Variant accessors used: `as_string()`, `as_vec2() -> (f32,f32)`, `as_int32() -> i32`.

Handled `fn_name`s (exact strings):

- **`"OnSendToServer"`** (subserver redirect — consumed by login layer, but parsed here):
  - `port = vl[1].as_string().parse::<u16>()` or 0.
  - `raw_token = vl[2].as_string()` (default `"0"`).
  - `user_id = vl[3].as_string()` (default `"0"`).
  - `server_str = vl[4].as_string()`.
  - `lmode = vl[5].as_string().parse::<i32>()` or 0.
  - `tank_id_name = vl[6].as_string()`.
  - Split `server_str` by `'|'` into ≤3 parts: `server = parts[0].trim_end()`; `door_id = parts[1].trim_end()` or `"0"` (empty→"0"); `raw_uuid = parts[2].trim_end()`.
  - **Token caching**: parse `raw_token.trim()` as i64. If `< 0` (marker) → use `last_redirect_token` if cached (log "using cached redirect token for marker …"), else keep `raw_token`. Otherwise (≥0/parse-ok/non-numeric) → if `raw_token` non-empty, store `last_redirect_token = raw_token`; token = raw_token.
  - **UUID caching**: if `raw_uuid` empty or `"-1"` → use `last_redirect_uuid` if cached else keep. Else store `last_redirect_uuid = raw_uuid`.
  - Build `RedirectData { server, port, token, user:user_id, door_id, uuid, lmode: lmode.to_string(), tank_id_name }`, set `redirect = Some(...)`, `redirect_attempts = 0`, clear `login_reject_streak`/`login_throttle_streak` (gateway accepted the logon), then `host.peer_disconnect(id, 0)` (the redirect happens on reconnect).

- **`"OnSpawn"`** — `message = vl[1].as_string()`; `data = parse_pipe_map(message)`.
  - **Self** (when `data` contains key `"type"`): set `local.net_id = data["netID"]`, `local.user_id = data["userID"]` (parse u32, default 0); `redirect=None`, `redirect_attempts=0`, `refresh_token_on_reconnect=false`; `clear_login_state_flags()`; `connected_since=None`; `was_in_world=true`; log net/user id + the ltoken string `"{ltoken}|{rid}|{mac}|{wk}"`; set `state.status = InGame`; emit `BotStatus{"in_game"}`.
  - **Other player** (no `"type"` key): parse `posXY` (split `'|'`, parse f32 → `(x,y)` px, default `(0,0)`), `netID`(u32), `userID`(u32), `mstate`(u32), `invis`(u32 != 0 → bool), `name`, `country`, `avatar`, `onlineID`, `eid`, `ip`, `colrect`, `titleIcon`. Build `Player`, insert into `players[net_id]`, rebuild `state.players` (`PlayerInfo{ net_id, name, pos_x=px/32, pos_y=px/32, country }`), emit `PlayerSpawn{…, x/32, y/32}`. Log `"[Bot] OnSpawn player=…"`.

- **`"OnSetPos"`** — `(x,y) = vl[1].as_vec2()` (px, default (0,0)); set `pos_x/pos_y`. **If `pathfind_target.is_some()` → set `pathfind_recalc = true`** (server corrected our position → replan). Write `state.pos_x/y = x/32, y/32`; emit `BotMove{x/32,y/32}`; log.

- **`"OnSuperMainStartAcceptLogonHrdxs47254722215a"`** (exact string) — set `state.status = Connected`; `wait_for_global_gate(ENTER_GAME_GATE, ENTER_GAME_STAGGER_MS=2000, "enter_game")`; `send_text("action|enter_game\n")`; emit `BotStatus{"connected"}`.

- **`"OnRemove"`** — `data = parse_pipe_map(vl[1])`; `net_id = data["netID"]`; `players.remove(net_id)`; rebuild `state.players`; log; emit `PlayerLeave{net_id}`.

- **`"OnSetBux"`** (gems/bux) — `gems = vl[1].as_int32()` (default 0); `inventory.add_gems(gems)` (sets balance); `state.gems = gems`; emit `BotGems{gems}`.

- **`"OnConsoleMessage"`** — `message = vl[1].as_string()`; `sync_geiger_state_from_console(&message)` (§7.5); `log_console(message)`.

- **`"OnDialogRequest"`** — `message = vl[1].as_string()`; log `"[Bot] Dialog: {message}"`; `take()` the one-shot `temporary_data.dialog_callback`; if present, invoke `cb(self)`.

- **`"SetHasGrowID"`** — `growid = vl[2].as_string()`; set `username = growid`; `state.username = growid`; emit `BotUsername{username}`. (GrowID resolved when logged in via ltoken.)

- **`"OnRequestWorldSelectMenu"`** (left world → world-select) — `world=None`; `pathfind_target=None`; `pathfind_recalc=false`; under write-lock set `state.world_name="EXIT"`, `state.status=InGame`, and `inventory.remove_temp_items()` (returns whether any removed). If removed → `emit_inventory_update()`. Emit `BotStatus{"in_game"}`, `BotWorld{"EXIT"}`, `WorldLoaded{name:"EXIT", width:0, height:0, tiles:[]}`; log.

- default `_` → ignored.

### 5.2 World load (`SendMapData` case, inline in `service_once`)
1. `players.clear()`, `local = LocalPlayer::default()`, `geiger_green_repeat = None`.
2. `World::parse(&pkt.extra_data)`; on OK: log `"[Bot] World: {w}x{h} tiles, {n} objects"`; store `world = Some(world.clone())`.
3. Build `state.tiles: Vec<TileInfo>` from `world.tile_map.tiles` (`fg_item_id, bg_item_id, flags = flags_raw, tile_type.clone()`); set `state.world_name/world_width/world_height`; build `state.objects: Vec<WorldObjectInfo>` (`uid, item_id, x, y, count`); `state.players = []`; `state.status = InGame`; `state.geiger_signal = None`.
4. Emit `BotStatus{"in_game"}`, `BotWorld{world_name}`, `WorldLoaded{name,width,height, tiles: Vec<WsTile>}`, `ObjectsUpdate{objects: Vec<WsObject>}`.
5. On parse error → log `"[Bot] World parse error: {e}"`.

### 5.3 `on_state(pkt)` — position updates
- If `pkt.net_id == local.net_id` (self): `pos_x/y = pkt.vector_x/vector_y`; write `state.pos_x/y = /32`; emit `BotMove`.
- Else if `players[pkt.net_id]` exists: update its `position = (vector_x, vector_y)`; update matching `state.players[*].pos_x/y = /32`; emit `PlayerMove`.
- Else: ignore.

### 5.4 `on_tile_change(pkt)` — TileChangeRequest
- `x = int_x as u32`, `y = int_y as u32`, `item_id = value as u16`. If no world → return. `idx = y*width + x`.
- `world.get_tile_mut(x,y)`:
  - If `item_id == 18` (punch/fist): if `tile.fg_item_id != 0` → set `fg_item_id = 0`, `tile_type = Basic` (broke foreground); else `bg_item_id = 0` (broke background).
  - Else → `tile.fg_item_id = item_id` (placed).
  - Return `(fg_item_id, bg_item_id)`.
- If some: update `state.tiles[idx].fg/bg`; emit `TileUpdate{x,y,fg,bg}`. Log `"[Bot] TileChange ({x},{y}) item={item_id}"`.

### 5.5 `on_send_tile_update_data(pkt)` — single-tile binary update
- `x=int_x as u32`, `y=int_y as u32`; no world → return; `idx = y*width+x`.
- `result = world.update_tile_from_bytes(x, y, &pkt.extra_data)` → `Option<(fg,bg)>` (parser owned by world spec).
- If some → update `state.tiles[idx]`, emit `TileUpdate`. Log `"[Bot] TileUpdateData ({x},{y})"`.

### 5.6 `on_send_tile_update_data_multiple(pkt)` — batched binary tile updates
**Binary format of `pkt.extra_data` (little-endian):**
| offset | size | field |
|---|---|---|
| 0 | 4 | `count` (u32 LE) |
| 4 | — | start of entries |

Per entry (fixed stride **12 bytes**):
| rel offset | size | field |
|---|---|---|
| +0 | 4 | `x` (u32 LE from `i32` slot) |
| +4 | 4 | `y` (u32 LE) |
| +8 | (var) | tile payload → passed to `update_tile_from_bytes` |

Steps:
1. If `data.len() < 4` → return. Read `count`, `offset = 4`.
2. No world → return.
3. Loop `count` times: if `offset + 12 > len` → break. Read `x`, `y`; `tile_data = &data[offset+8 ..]`; `idx = y*width + x` (computed in u64 to avoid overflow). `update_tile_from_bytes(x,y,tile_data)` → if `(fg,bg)` update `state.tiles[idx]` + emit `TileUpdate`. **`offset += 12`** regardless.
   - **Known limitation to preserve**: the loop advances a *fixed* 12 bytes/entry (x=4,y=4,fg=2,bg=2). `update_tile_from_bytes` is handed the remainder of the buffer from `+8` and only its first tile record is used; any per-tile extra bytes beyond 12 are **not** parsed and will desync a batch that carries variable-length tile data. Port as-is (do not "fix") unless the world spec says otherwise.
4. Log `"[Bot] TileUpdateDataMultiple count={count}"`.

### 5.7 `on_send_tile_tree_state(pkt)` — tree harvested
- `x=int_x`, `y=int_y`; no world → return; `idx=y*width+x`. `get_tile_mut`: set `fg_item_id=0`, `tile_type=Basic`, read `bg`. Update `state.tiles[idx].fg_item_id=0`; emit `TileUpdate{fg:0,bg}`; log `"[Bot] TileTreeState ({x},{y}) harvested"`.

### 5.8 `on_modify_item_inventory(pkt)` + `emit_inventory_update()`
`on_modify_item_inventory`: `item_id = value as u16`.
- If `pkt.jump_count != 0` → **remove**: `amount = jump_count`; `inventory.sub_item(item_id, amount)`; if item no longer present, `equipped_items.remove(item_id)`; log `"… item={id} -{amount}"`.
- Else `amount = animation_type`: if `!= 0` → `inventory.add_item(item_id, amount)`, log `"+{amount}"`; else log `"+0 ignored"`.
- Then `emit_inventory_update()`.

`emit_inventory_update()`:
- Build `Vec<InvSlot>` from `inventory.items.values()`: `{ item_id: i.id, amount: i.amount, is_active: i.flag & 1 != 0, action_type: items_dat.find_by_id(i.id).action_type or 0 }`. Write `state.inventory = slots`, `state.inventory_size = inventory.size`.
- Build parallel `Vec<WsInvItem>` (same fields), emit `InventoryUpdate{ gems: inventory.gems, inventory_size, items }`.

### 5.9 `on_ping_request(challenge)` / `on_server_hello()` (context)
`on_ping_request(challenge: u32)`:
- `time_val = start_time.elapsed().as_millis() as u32`.
- `bx = build_length==0 ? 2.0 : build_length as f32`; `by = punch_length==0 ? 2.0 : punch_length as f32`.
- `in_world = world.is_some()`.
- Build `PingReply { target_net_id = hash_string(challenge.to_string()), value = time_val, vector_x = bx*32, vector_y = by*32 }`.
- If `in_world`: `net_id = local.hack_type`, `vector_x2 = local.velocity`, `vector_y2 = local.gravity`.
- `send_game_packet(&reply, reliable=true)`. (`hash_string` is the GT string hash from the crypto spec.)

`on_server_hello()` is login glue: sets `saw_server_hello=true`; if a redirect is pending build+send the redirect login packet, else gate on `LOGIN_PACKET_GATE` (1000 ms) and send the login packet. Included for context; full detail in spec 07.

### 5.10 `on_item_change_object(pkt)` — dropped-object lifecycle (auto-pickup backbone)
No world → return. Dispatch on `pkt.net_id` sentinels:
- **`u32::MAX` (0xFFFFFFFF) — new drop**: `uid = world.next_object_uid; world.next_object_uid += 1`. Build `WorldObject { item_id: value as u16, x: vector_x.ceil(), y: vector_y.ceil(), count: float_variable as u8, flags: object_type, uid }`. Push to `world.objects`. Rebuild `state.objects` (`WorldObjectInfo`) and emit `ObjectsUpdate` with full `Vec<WsObject>`.
- **`u32::MAX - 2` (0xFFFFFFFD) — count update**: find the object matching `item_id == value as u16 && x == vector_x.ceil() && y == vector_y.ceil()`; set `obj.count = float_variable as u8`. Rebuild `state.objects` + emit `ObjectsUpdate`.
- **`net_id > 0` — collected**: find object index by `uid == pkt.value` and `remove` it. If removed:
  - Rebuild `state.objects` + emit `ObjectsUpdate`.
  - **If `pkt.net_id == local.net_id`** (we collected it): `current = inventory.items[item.item_id].amount or 0`; `to_add = min(item.count, 200u8.saturating_sub(current))` (per-item cap 200); `inventory.add_item(item.item_id, to_add)`; log `"[Bot] ItemCollect id={} count={}"`; `emit_inventory_update()`.
- default `_` → ignore.

> Note the object identity model: **new drops key off a client-assigned monotonic `uid`; count updates key off (item_id, ceil(x), ceil(y)); collection keys off `uid` == `pkt.value`.** Positions are stored ceil'd. Preserve exactly.

### 5.11 `on_send_lock(pkt)` — lock placed
- `x=int_x`, `y=int_y`, `fg=value as u16`; no world → return. `get_tile_mut`: set `fg_item_id = fg`, read `bg`. Update `state.tiles[idx].fg_item_id = fg`; emit `TileUpdate{fg,bg}`; log `"[Bot] SendLock tile=({x},{y}) item={fg}"`.

---

## 6. SCRIPT/AUTOMATION REQUEST↔REPLY BRIDGE

### 6.1 `drain_script_requests()`
Called once per `run()` iteration. Loop:
- If `script_req_rx` is `None` → break.
- `try_recv`:
  - `Empty` → break.
  - `Disconnected` → the automation thread exited: set `script_req_rx=None`, `script_reply_tx=None`, `event_tx=None`; break.
  - `Ok(req)` → `reply = handle_script_request(req)`; if `script_reply_tx` present, `send(reply).ok()` (best-effort).
- C++: same loop over the request queue; on producer-closed, drop all three channel handles.

### 6.2 `handle_script_request(req) -> ScriptReply`
Total mapping (every request produces exactly one reply):

| Request | Action | Reply |
|---|---|---|
| `Reconnect` | `reconnect()` | `Ack` |
| `Disconnect` | `disconnect()` | `Ack` |
| `SendRaw{pkt}` | `send_game_packet(&pkt, reliable=true)` | `Ack` |
| `SendPacket{ptype,text}` | `2`→`send_text(text)`, `3`→`send_game_message(text)`, else no-op | `Ack` |
| `Say{text}` | `say(text)` | `Ack` |
| `Warp{name,id}` | `warp(name,id)` | `Ack` |
| `LeaveWorld` | `leave_world()` | `Ack` |
| `Respawn` | `respawn()` | `Ack` |
| `Active{tile_x,tile_y}` | `active_tile(tile_x,tile_y)` | `Ack` |
| `Enter{pass}` | `cx=pos_x/32,cy=pos_y/32`; if `pass` → `send_text("action|input\n|text|{pw}\n")` else `active_tile(cx,cy)` | `Ack` |
| `Place{x,y,item}` | `place(x,y,item,is_punch=false)` | `Ack` |
| `Hit{x,y}` | `punch(x,y)` | `Ack` |
| `Wrench{x,y}` | `wrench_at(x,y)` (absolute tile) | `Ack` |
| `WrenchPlayer{net_id}` | `wrench_player(net_id)` | `Ack` |
| `Wear{item_id}` | `wear(item_id)` | `Ack` |
| `Unwear{item_id}` | `unwear(item_id)` | `Ack` |
| `Drop{item_id,count}` | `drop_item(item_id,count)` | `Ack` |
| `Trash{item_id,count}` | `trash_item(item_id,count)` | `Ack` |
| `FastDrop{item_id,count}` | `fast_drop(item_id,count)` | `Ack` |
| `FastTrash{item_id,count}` | `fast_trash(item_id,count)` | `Ack` |
| `Walk{tile_x,tile_y}` | `walk(tile_x,tile_y)` | `Ack` |
| `SetDirection{facing_left}` | `set_direction(facing_left)` | `Ack` |
| `FindPath{x,y}` | `find_path(x,y)` (blocking walk) | `Ack` |
| `CollectObject{uid,range}` | `collect_object_at(uid,range)` | `Ack` |
| `Collect{range,interval_ms}` | if world: gather uids within circle `dx²+dy² <= (range*32)²`; for each `collect_object_at(uid,range)` + `sleep_ms(interval_ms)`; count = n | `CollectCount(n)` |
| `SetMac{mac}` | `mac=mac`; `state.mac=mac` | `Ack` |
| `SetAutoCollect{enabled}` | `auto_collect=enabled`; `state.auto_collect=enabled`; emit `BotAutoCollect` | `Ack` |
| `GetWorld` | clone `world`→`WorldSnapshot{world,players(clone),local_net_id,local_user_id,local_name=username,local_pos=(pos_x,pos_y)}` (None if no world) | `World(Option<…>)` |
| `GetInventory` | clone inventory | `Inventory(inv)` |
| `IsWearing{item_id}` | `is_item_equipped(item_id)` | `Bool` |
| `GetLocal` | `LocalSnapshot{net_id,user_id,pos_x,pos_y,username,mac}` | `Local(…)` |
| `GetPath{x,y}` | `compute_path(x,y)` | `Path(Vec<(u32,u32)>)` |
| `IsInWorld{name}` | `(Some world, Some n)`→`world_name.upper()==n.upper()`; `(Some,None)`→true; `(None,_)`→false | `Bool` |
| `IsInTile{x,y}` | `(pos_x/32==x) && (pos_y/32==y)` | `Bool` |
| `GetAutoCollect` | `auto_collect` | `Bool` |
| `GetPing` | `state.ping_ms` | `U32` |
| `GetGems` | `inventory.gems` | `I32` |
| `SetPlaceDelay{ms}` | `delays.place_ms=ms`; `state.delays.place_ms=ms`; emit `BotDelays{all fields}` | `Ack` |
| `SetWalkDelay{ms}` | `delays.walk_ms=ms`; `state.delays.walk_ms=ms`; emit `BotDelays{all fields}` | `Ack` |
| `GetPlaceDelay` | `delays.place_ms as u32` | `U32` |
| `GetWalkDelay` | `delays.walk_ms as u32` | `U32` |

> **Blocking semantics**: `Walk`, `FindPath`, `Collect`, and any action that calls `sleep_ms`/`place`/`walk` execute on the **bot worker thread** synchronously (they call `service_once` internally via `sleep_ms`, keeping ENet alive). The automation thread is blocked on its reply queue meanwhile. Preserve this: the bot thread must keep servicing ENet during long automation actions, never dead-sleep.

---

## 7. GEIGER HOOKS

The geiger detector maps GT particle-effect packets to radiation-area signals used by the fleet's radiation-hunting automation. Runs on **every** GameUpdate (`update_geiger_signal` in step 3 of §5's GameUpdate arm) plus console-message syncing.

Constants: `GEIGER_COUNTER_ITEM_ID = 2204`, `DEAD_GEIGER_COUNTER_ITEM_ID = 2286`.

### 7.1 `decode_geiger_signal_packet(pkt) -> Option<(x:u32, y:u32, raw_area:u8)>` (pure/static)
Returns `None` unless **all** hold:
- `packet_type` ∈ { `SendParticleEffect`, `SendParticleEffectV2` }.
- `animation_type == 114`.
- `f32_close(vector_y2, 114.0)` (see §7.4).
- `raw_area = vector_x2.round()`, and `f32_close(vector_x2, raw_area)` (must be integral).
- `raw_area as i32` ∈ `0..=3`.
Then `x = geiger_tile_coord(vector_x, 10.0)?`, `y = geiger_tile_coord(vector_y, 17.0)?`; return `(x, y, raw_area as u8)`.

### 7.2 `geiger_tile_coord(pixel: f32, offset: f32) -> Option<u32>`
- If `!pixel.is_finite() || pixel < offset` → `None`.
- Else `((pixel - offset) / 32.0).round().max(0.0) as u32`.
- The X offset is `10.0`, the Y offset is `17.0` (particle spawn is offset within the tile).

### 7.3 `update_geiger_signal(pkt)`
1. `decode_geiger_signal_packet(pkt)` → `(x,y,raw_area)`; return if None.
2. `timestamp_ms = now_millis()`.
3. `area_type` from `raw_area`: `0→Red, 1→Yellow, 2→Green, 3→Prize`, else return.
4. **Rapid-green detection** (only when `raw_area == 2`):
   - If `geiger_green_repeat` exists AND same `(x,y)` AND `timestamp_ms - last_seen_ms <= 2500` → increment `count` (saturating, capped at 10), refresh `last_seen_ms`.
   - Else start fresh `{x,y,last_seen_ms=ts,count=1}`.
   - If resulting `count >= 2` → override `area_type = RapidGreen`.
   - Store `geiger_green_repeat = Some(repeat)`.
   - For any non-green raw_area → `geiger_green_repeat = None`.
5. Write `state.geiger_signal = Some(GeigerSignal { x, y, area_type, timestamp_ms })`.

`GeigerArea` enum values used: `Red, Yellow, Green, Prize, RapidGreen` (defined in bot_state spec). `GeigerSignal { x:u32, y:u32, area_type:GeigerArea, timestamp_ms:u64 }`.

### 7.4 `f32_close(a,b) -> bool` = `(a-b).abs() <= 0.01`.

### 7.5 `sync_geiger_state_from_console(message)` (called from `OnConsoleMessage`)
Substring-driven equip-state sync for the two geiger counters:
- contains `"You are detecting radiation"` → `mark_item_equipped(2204, true)`, `mark_item_equipped(2286, false)`; log "charged counter is active".
- contains `"Charging Geiger Counter"` AND `"mod added"` → `mark_item_equipped(2286, true)`, `mark_item_equipped(2204, false)`; log "dead counter is charging/active".
- contains `"Geiger Counter removed"` → both `false`; log "counter removed/inactive".

---

## 8. THE LUA SEAM → NATIVE AUTOMATION (how Nxrth wires in-engine automation)

In Mori, the only consumer of the automation bridge was a Lua VM spawned per bot. The three seams are:

1. **Inbound events** (`event_tx: Channel<BotEventRaw>`, bounded 256): the bot thread pushes `VariantList`, `GameUpdate`, and `GameMessage` for *every* corresponding inbound packet (best-effort `try_send`). This is the "listen to what the world is doing" feed.
2. **Outbound requests** (`script_req_rx: Channel<ScriptRequest>`, unbounded): the automation sends actions/queries; the bot executes them on its own thread and returns a `ScriptReply`.
3. **Replies** (`script_reply_tx: Channel<ScriptReply>`, unbounded): one reply per request.

**Lifecycle** (`handle_command(BotCommand::RunScript{content})`):
1. Signal `script_stop = true` (interrupt any running automation), drop the three channel handles (previous automation sees Disconnected and exits), then reset `script_stop = false`.
2. Create fresh channels: `req` (unbounded), `reply` (unbounded), `event` (bounded **256**). Store `script_req_rx = Some(req_rx)`, `script_reply_tx = Some(reply_tx)`, `event_tx = Some(event_tx)`.
3. Clone shared handles (`items_dat`, `state`, `script_stop`, `username`) and **spawn a new thread** running the automation with `(req_tx, reply_rx, event_rx, items, state, stop_flag, username, content)`.
`BotCommand::StopScript` → `script_stop = true`.

**Nxrth native mapping**:
- Replace `run_script_threaded(...)` with a native `AutomationRunner` object on its own `std::thread`. It receives the same `(req_tx, reply_rx, event_rx, items_dat, state, stop_flag, username, <task descriptor>)`.
- The runner reads `event_rx` to observe world/packets, and drives the bot by sending `ScriptRequest`s and awaiting `ScriptReply`s — **byte-for-byte the same protocol**, no Lua.
- `script_stop` (atomic) is polled by the runner to abort long tasks.
- Because requests are executed synchronously on the bot thread (which keeps ENet serviced during sleeps), the runner can treat `Walk`/`FindPath`/`Collect` as blocking calls.
- Keep the `event_tx` bound at **256** with drop-on-full: automation that falls behind must not back-pressure the network thread.
- The task input `content` (a Lua script string in Mori) becomes an Nxrth automation task descriptor (enum/struct/graph selected in the ImGui UI). The `BotCommand::RunScript`/`StopScript` command surface stays the same shape.

---

## 9. ACTION HELPERS (exact packet/string formats)

All `send_text`/`send_game_message` payloads are `\n`-terminated GT action strings. `send_game_packet` builds a binary `GameUpdatePacket`.

### 9.1 Movement
`walk(tile_x, tile_y)` (i32 tiles):
- `target = tile*32` px; `facing_left = target_x < pos_x`; set `pos_x/pos_y = target`; write `state.pos_x/y = /32`; emit `BotMove`.
- flags = `WALK | STANDING`, then `.set(FACING_LEFT, facing_left)`.
- Packet: `State { vector_x = target_x, vector_y = target_y + 2.0, int_x = -1, int_y = -1, flags }`, **unreliable**.
- `sleep_ms(delays.walk_ms)`.

`set_direction(facing_left)`: `State { net_id = local.net_id, vector_x = pos_x, vector_y = pos_y, int_x = -1, int_y = -1, flags = STANDING.set(FACING_LEFT) }`, **reliable**.

### 9.2 Pathfinding — `find_path` / `compute_path`
Grid build (shared pattern): `tiles: Vec<(fg_item_id, collision_type)>` from `world.tile_map.tiles`, where `collision_type` is:
- `find_path`: `Door → 0`; else `items_dat.find_by_id(fg).collision_type` (fallback `fg==0 ? 0 : 1`). **Does NOT special-case Lock.**
- `compute_path` **and** `collect`: `Lock → 3`; `Door → 0`; else items_dat lookup (fallback `fg==0?0:1`). (This asymmetry is intentional in the source — preserve it, or unify only if the world/astar spec dictates.)

`find_path(to_x, to_y)` (u32 tiles) — **blocking, walks the path**:
1. `pathfind_target = Some((to_x,to_y))`, `pathfind_recalc = false`.
2. While `pathfind_target.is_some()`:
   - No world → clear target, return.
   - Build grid; `astar.update_from_tiles(w,h,&tiles)`.
   - `from = pos/32`; `has_access = has_access()`.
   - If `from == goal` → clear target, break.
   - `path = astar.find_path(from_x,from_y,goal_x,goal_y,has_access)`; if `None` → clear target, break.
   - `pathfind_recalc = false`; for each node: `(nx,ny)=node`, skip if equal to current tile (`pos/32`); else `walk(node.x, node.y)`; if `pathfind_recalc` (set by an inbound `OnSetPos`) → break inner loop.
   - If `pathfind_recalc` → `continue` outer (replan from new server position); else clear `pathfind_target`.

`compute_path(to_x,to_y) -> Vec<(u32,u32)>` — **non-blocking, returns nodes**: build grid (Lock→3/Door→0 variant); `astar.update_from_tiles`; `from=pos/32`; `astar.find_path(...).unwrap_or_default()` → map nodes to `(x,y)` tiles. Empty vec if no world/no path.

### 9.3 Tile actions
`place(offset_x, offset_y, item_id, is_punch)`:
1. If `!is_punch && !inventory.has_item(item_id as u16, 1)` → return (nothing to place).
2. `base = floor(pos/32)`; `tile = base + offset`. If `|tile - base| > 4` on either axis → return (max reach ±4 tiles).
3. Send `TileChangeRequest { vector_x = pos_x, vector_y = pos_y, int_x = tile_x, int_y = tile_y, value = item_id }`, **reliable**.
4. flags = `(is_punch ? PUNCH : PLACE) | STANDING`, `.set(FACING_LEFT, base_x > tile_x)`. Reuse the packet with `packet_type = State`, `flags`; send **reliable**.
5. `sleep_ms(delays.place_ms)`.
6. If `!is_punch && item_id != 18 && item_id != 32` → `inventory.sub_item(item_id,1)` + `emit_inventory_update()` (don't decrement for fist=18 or wrench=32).

`punch(ox,oy)` = `place(ox,oy,18,is_punch=true)`. `wrench(ox,oy)` = `place(ox,oy,32,is_punch=false)`.
`wrench_at(tile_x,tile_y)` = `wrench(tile_x - base_x, tile_y - base_y)` where `base = floor(pos/32)` (converts absolute tile → offset).
`active_tile(tile_x,tile_y)`: `TileActivateRequest { vector_x=pos_x, vector_y=pos_y, int_x=tile_x, int_y=tile_y }`, reliable.

### 9.4 Inventory / equip
`wear(item_id)`: if `is_item_equipped(item_id as u16)` → log skip, return. Else `ItemActivateRequest { value = item_id }` reliable; `mark_item_equipped(item, true)`.
`unwear(item_id)`: `ItemActivateRequest { value = item_id }` reliable; `mark_item_equipped(item, false)`. (Same packet toggles.)
`wrench_player(net_id)`: `send_text("action|wrench\n|netid|{net_id}\n")`.

`drop_item(item_id, amount)`:
- `send_text("action|drop\n|itemID|{item_id}\n")`.
- Install one-shot `dialog_callback`: on next `OnDialogRequest` → `send_text("action|dialog_return\ndialog_name|drop_item\nitemID|{item_id}|\ncount|{amount}\n")`, then clear callback.

`trash_item(item_id, amount)`: same as drop with `action|trash` and `dialog_name|trash_item`.
`fast_drop(item_id, count)`: send the dialog_return **directly** (no dialog wait): `"action|dialog_return\ndialog_name|drop_item\nitemID|{item_id}|\ncount|{count}\n"`.
`fast_trash(item_id, count)`: same with `dialog_name|trash_item`.

`accept_access()` — chained dialog flow:
1. `wrench_player(local.net_id)`.
2. Install callback #1: on `OnDialogRequest` → `send_text("action|dialog_return\ndialog_name|popup\nnetID|{net_id}|\nbuttonClicked|acceptlock\n")`, then install callback #2.
3. Callback #2: on next `OnDialogRequest` → `send_text("action|dialog_return\ndialog_name|acceptaccess\n")`, then clear.

### 9.5 World / session
`say(text)`: `send_text("action|input\n|text|{text}\n")`.
`warp(name, id)`: `wait_for_global_gate(WARP_GATE, 1200ms, "warp")` (fleet-serialized); `redirect=None`, `redirect_attempts=0`; `send_game_message("action|join_request\nname|{name}|{id}\ninvitedWorld|0\n")`.
`leave_world()`: `send_game_message("action|quit_to_exit\n")`.
`respawn()`: `send_text("action|respawn\n")`.
`disconnect()`: if peer, `host.peer_disconnect(peer, 0)`.
`reconnect()`: `reconnect_main(refresh_token=false)` (login spec).

### 9.6 Object collection
`collect() -> usize` (auto-pickup; called every 500 ms when `auto_collect`, and standalone):
1. No world → 0. If `inventory.item_count >= inventory.size` (full) → 0.
2. `radius_tiles = collect_radius_tiles.clamp(1,5)`; `r_px = radius*32`. `MAX_PER_TICK = 32` (packet cap/call).
3. Build `nearby`: for each object not in `collect_blacklist`, with `|pos_x - obj.x| <= r_px && |pos_y - obj.y| <= r_px` (Chebyshev box), compute `ring = max(|dx|,|dy|)`; collect `(ring, uid, x, y, item_id)`; **sort ascending by `ring`** (nearest first); map to `(uid,x,y,item_id)`. If empty → 0.
4. Build A* grid (Lock→3, Door→0, else items_dat collision, fallback `fg==0?0:1`); `astar.update_from_tiles`. `from = pos/32`; `has_access = has_access()`.
5. For each of the first `MAX_PER_TICK` nearby: `tile = obj/32`; if `astar.find_path(from, tile, has_access)` is `None` → skip (unreachable). `can_collect`: item `112` (gems) always true; else if in inventory `existing.amount < 200`; else `inv_count < inv_size`. If collectable → send `ItemActivateObjectRequest { vector_x = x, vector_y = y, value = uid }` reliable; `sent += 1`.
6. Return `sent`.

`collect_object_at(uid, range_tiles: f32)`: find object by `uid` (clone); if within `dx²+dy² <= (range*32)²` → `ItemActivateObjectRequest { vector_x=obj.x, vector_y=obj.y, value=obj.uid }` reliable.

`has_access() -> bool`: `LOCK_ITEM_IDS = [242, 1796, 2408, 7188, 10410]`. No world → false. For each tile whose `fg_item_id ∈ LOCK_ITEM_IDS` and `tile_type == Lock{ access_uids, .. }`: return true if `access_uids.contains(local.user_id)`. Else false.

`set_auto_collect(enabled)`: sets `auto_collect` only (the request/command variants also mirror `state` and emit `BotAutoCollect`).

---

## 10. UI COMMAND PATH — `handle_command(BotCommand)`

`cmd_rx` is the UI→bot command channel (drained each `run()` iteration). In Nxrth this is the Dear ImGui control surface. Variants and effects:

| BotCommand | Effect |
|---|---|
| `Move{x,y}` | `walk(pos_tile_x + x, pos_tile_y + y)` (relative step) |
| `WalkTo{x,y}` | `find_path(x,y)` (absolute) |
| `RunScript{content}` | (re)start automation thread — see §8 |
| `StopScript` | `script_stop = true` |
| `Say{text}` | `say(text)` |
| `Warp{name,id}` | `warp(name,id)` |
| `Disconnect` | `disconnect()` |
| `Reconnect` | if peer present → `disconnect()` (Disconnect handler reconnects); else `reconnect_main(false)` |
| `Place{x,y,item}` | `place(x,y,item,false)` |
| `Hit{x,y}` | `punch(x,y)` |
| `Wrench{x,y}` | `wrench_at(x,y)` |
| `Wear{item_id}` / `Unwear{item_id}` | `wear/unwear` |
| `Drop{item_id,count}` / `Trash{item_id,count}` | `drop_item/trash_item` |
| `LeaveWorld` / `Respawn` | `leave_world/respawn` |
| `FindPath{x,y}` | `find_path(x,y)` |
| `SetDelays(d)` | `delays=d`; `state.delays=d`; emit `BotDelays{all fields}` |
| `SetAutoCollect{enabled}` | mirror to `auto_collect` + `state` + emit `BotAutoCollect` |
| `SetAutoReconnect{enabled}` | mirror to `auto_reconnect` + `state` |
| `SetCollectConfig{radius_tiles,blacklist}` | `collect_radius_tiles = radius.clamp(1,5)`; `collect_blacklist = set(blacklist)`; mirror to `state.collect_radius_tiles` + `state.collect_blacklist = sorted(blacklist)` |
| `AcceptAccess` | `accept_access()` |

---

## 11. SEND / EMIT / LOG PRIMITIVES

`send_text(text)`: if peer → `raw = make_text_packet(text)` (msg-type-2 text packet, protocol spec); `emit_traffic("out","text",len, redact_packet_text(text))`; `peer_send(ch 0, Packet::reliable(raw))`.
`send_game_message(text)`: `make_game_message_packet(text)` (msg-type-3); traffic kind `"game_message"`; reliable, ch 0.
`send_game_packet(pkt, reliable)`: `make_game_packet(pkt)` (msg-type-4 binary); traffic kind `"game_update:{packet_type:?}"`, detail `format_game_packet_detail(pkt)`; `Packet::reliable` or `Packet::unreliable`; ch 0. All are no-ops if no `peer_id`.

`emit(event)`: if `ws_tx` set → `send(event)` (best-effort). C++: push to observer bus / write UI state.

`log_console(msg)`: rewrite a leading `"[Bot] "` prefix to `"[Bot#{bot_id}] "`; write to file logger. If `is_high_frequency_noise(msg)` → **stop** (file-log only; don't buffer/broadcast). Else push to `state.console` (ring buffer, cap 100 — drop oldest), and `emit(Console{message})`.

`emit_traffic(direction, kind, size, detail)`: `detail = truncate_text(detail, 6000)`; `summary = summarize_detail(detail)`; emit `Traffic{ direction, kind, size, summary, detail, timestamp_ms = now_millis() }`. (Redaction of tokens/passwords happens via `redact_packet_text` before this is called for text/game-message packets.)

`is_item_equipped(item_id) -> bool` = `equipped_items.contains(item_id) || inventory.is_active(item_id)`.
`mark_item_equipped(item_id, active)`: insert/remove in `equipped_items`; `inventory.set_active(item_id, active)`; `emit_inventory_update()`.

`sleep_ms(ms)`: **service-while-sleeping** — loop until `now >= deadline`: `service_once()` + `sleep(10ms)`. Never dead-sleep; this is what keeps the bot responsive to the network during automation actions.

`now_millis()` = ms since UNIX epoch (`SystemTime::now().duration_since(UNIX_EPOCH)`).

---

## 12. CONSTANTS SUMMARY (exact values — do not change)

| Name | Value | Where |
|---|---|---|
| pixels per tile | `32.0` | everywhere (pos/tile conversion) |
| `WARP_STAGGER_MS` | `1200` | `warp()` fleet gate |
| `ENTER_GAME_STAGGER_MS` | `2000` | `OnSuperMainStartAcceptLogon…` |
| `LOGIN_PACKET_STAGGER_MS` | `1000` | `on_server_hello` |
| `GATE_CONNECTED_MAX_AHEAD_MS` | `2500` | gate reservation cap (connected) |
| auto-collect tick | `500 ms` | `run()` loop |
| main loop cadence / service sleep | `10 ms` | `run()`, `sleep_ms`, gate wait |
| login watchdog timeout | `30 s` | `run()` (no world while connected) |
| ENet peer timeout (limit,min,max) | `(0, 12000, 30000)` ms | on Connect |
| place max reach | `±4` tiles | `place()` |
| walk `vector_y` bias | `+2.0` px | `walk()` |
| per-item inventory cap | `200` | `on_item_change_object`, `collect` |
| gems item id | `112` | `collect` (always collectable) |
| fist/punch item id | `18` | tile-change break; no inv decrement |
| wrench item id | `32` | no inv decrement |
| collect radius clamp | `1..=5` tiles | `collect`, `SetCollectConfig` |
| `MAX_PER_TICK` (collect) | `32` | `collect` |
| `GEIGER_COUNTER_ITEM_ID` | `2204` | geiger |
| `DEAD_GEIGER_COUNTER_ITEM_ID` | `2286` | geiger |
| geiger particle `animation_type` | `114` | `decode_geiger_signal_packet` |
| geiger `vector_y2` marker | `114.0` (±0.01) | decode |
| geiger area range | `0..=3` (Red/Yellow/Green/Prize) | decode |
| geiger tile X offset | `10.0` px | `geiger_tile_coord` |
| geiger tile Y offset | `17.0` px | `geiger_tile_coord` |
| geiger rapid-green window | `2500 ms` | `update_geiger_signal` |
| geiger rapid-green threshold / cap | `count >= 2` → RapidGreen; cap `10` | `update_geiger_signal` |
| `f32_close` epsilon | `0.01` | `f32_close` |
| lock item ids (`has_access`) | `[242, 1796, 2408, 7188, 10410]` | `has_access` |
| `event_tx` bound | `256` (drop on full) | `RunScript` |
| console ring buffer cap | `100` lines | `log_console` |
| traffic detail truncation | `6000` chars | `emit_traffic` |
| `ItemChangeObject` sentinels | new=`0xFFFFFFFF`; count=`0xFFFFFFFD`; collect=`net_id>0` | `on_item_change_object` |

### Magic action strings (exact, `\n`-delimited)
```
action|enter_game\n
action|input\n|text|{text}\n                                     (say / Enter password)
action|join_request\nname|{name}|{id}\ninvitedWorld|0\n           (warp)
action|quit_to_exit\n                                            (leave_world)
action|respawn\n
action|wrench\n|netid|{net_id}\n                                 (wrench_player)
action|drop\n|itemID|{item_id}\n                                 (drop_item step 1)
action|trash\n|itemID|{item_id}\n                                (trash_item step 1)
action|dialog_return\ndialog_name|drop_item\nitemID|{id}|\ncount|{n}\n
action|dialog_return\ndialog_name|trash_item\nitemID|{id}|\ncount|{n}\n
action|dialog_return\ndialog_name|popup\nnetID|{net_id}|\nbuttonClicked|acceptlock\n
action|dialog_return\ndialog_name|acceptaccess\n
```
CallFunction names handled (exact): `OnSendToServer`, `OnSpawn`, `OnSetPos`, `OnSuperMainStartAcceptLogonHrdxs47254722215a`, `OnRemove`, `OnSetBux`, `OnConsoleMessage`, `OnDialogRequest`, `SetHasGrowID`, `OnRequestWorldSelectMenu`.

Console/text substrings scanned by geiger + login: `"You are detecting radiation"`, `"Charging Geiger Counter"`+`"mod added"`, `"Geiger Counter removed"`; (login triggers listed in §5). Track fields: `Level`, `GrowId`, `installDate`, `Global_Playtime`, `Awesomeness`, plus `Authentication_error|23`.

---

## 13. PORTING GOTCHAS / INVARIANTS

- **Positions are pixels internally, tiles in state/UI/events.** Every `state.*`/`Ws*` position is `px/32`. Object positions are stored **ceil'd** (`vector_*.ceil()`).
- **`sleep_ms` and gate waits MUST keep servicing ENet.** A dead-sleep during placing/walking/warping starves the connection at fleet scale → the server times out the peer. This is the single biggest scaling bug the Rust code guards against.
- **`event_tx` is best-effort, bounded 256.** Never block the network thread on the automation queue.
- **Object identity**: new drops assign a client-side monotonic `uid` (`world.next_object_uid`); collection matches on `uid == pkt.value`; count updates match on `(item_id, ceil x, ceil y)`. Local-collect only adds to inventory when `pkt.net_id == local.net_id`, capped so no item exceeds 200.
- **`find_path` vs `compute_path`/`collect` differ** on Lock collision (find_path ignores Lock; the others map Lock→3). Preserve unless the astar spec unifies them.
- **`pathfind_recalc`** is set by inbound `OnSetPos` while a `find_path` walk is in progress → the current segment is abandoned and the route replanned from the server-authoritative position.
- **Dialog callbacks are one-shot and chainable** (`accept_access` installs two in sequence). Fire on `OnDialogRequest`, `take()` before invoking.
- **`place` won't act without the item** (unless punch) and clamps to ±4 tiles; fist(18)/wrench(32) never decrement inventory.
- **`OnSpawn` self vs other** is distinguished purely by presence of the `"type"` key in the pipe-map.
- **World reset** (`SendMapData`) clears `players`, resets `local` to default, clears `geiger_green_repeat`, and repopulates `state` wholesale — treat each world load as a full state reset.
