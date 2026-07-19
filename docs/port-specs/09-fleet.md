# Nxrth Port Spec — Module 09: Fleet (BotManager + BotState)

**Source of truth (Rust):**
- `src/bot_manager.rs` (Mori 2.0.0) — the fleet supervisor: spawns one OS thread per bot, tracks them in a map, reaps finished threads, routes commands, counts proxy capacity.
- `src/bot_state.rs` (Mori 2.0.0) — the shared per-bot state snapshot (`BotState`), status enum, command enum, and supporting value types.

This document is the **single source of truth** for the C++ (Nxrth) reimplementation of this module. An engineer must be able to build it **without reading the Rust**. Cross-module types (`Socks5Config`, `RotatingLoginProxy`, `WsTx`/`WsEvent`, `ItemsDat`, `TileType`, `Bot`) are defined in *other* modules; here they are described only by the interface this module actually uses, and are cross-referenced to their own port specs.

> **Naming:** all `Mori`/`mori` → `Nxrth`/`nxrth`; all `Cloei`/`cloei` → `North`/`north`. Applied throughout this spec. See §7 for the concrete occurrences found in these two files. (No `Cloei`/`cloei` tokens exist in either file; the only literal `mori` token is the thread name prefix.)

---

## 0. Module role in one paragraph

`BotManager` owns the whole fleet. Each running bot lives on its **own OS thread**; the manager keeps a `BotEntry` per bot in a `HashMap<u32, BotEntry>` keyed by a **monotonic, never-reused `u32` id**. Each bot exposes exactly three shared handles back to the manager: a **stop flag** (`atomic bool`), a **live state snapshot** (`RwLock<BotState>`) that the bot writes and the UI reads, and a **command channel sender** (`mpsc`) the UI uses to push actions into the bot. The manager never touches ENet/game logic — it only supervises lifecycle (spawn / stop / reap), fans out commands, exposes read views (`list`, `get_state`), and counts how many bots sit behind each proxy endpoint (`proxy_key_counts`). Panics inside a bot are caught per-thread (`catch_unwind`) so one crashing bot never takes down the fleet.

There is **no binary/on-disk wire format in this module.** The structs derive `serde` for JSON (Mori's web UI); Nxrth has no web server, so these become plain C++ structs read directly by Dear ImGui. The one struct that is genuinely (de)serialized to disk is `BotDelays` (config) — its JSON field names are documented in §1.9 for config compatibility.

---

## 1. TYPES

### Serde/JSON conventions used in `bot_state.rs`
- All enums here use `#[serde(rename_all = "snake_case")]`, so serialized enum *values* are lower_snake_case (`in_game`, `rapid_green`, …).
- `BotState`, `BotInfo`, and all sub-structs use `#[derive(Serialize)]` — field names serialize verbatim (already snake_case) **except** `GeigerSignal.area_type` which is renamed to `"type"` on the wire.
- In Nxrth these are C++ structs; keep the JSON field/value names in §1.9 only if you retain any JSON (config load/save of `BotDelays`, or an optional tooling export). The UI reads the C++ fields directly.

---

### 1.1 `BotStatus` (enum) — `bot_state.rs`
Derives: `Default, Clone, Serialize, PartialEq`. `#[serde(rename_all="snake_case")]`.

| Variant | `#[default]`? | `Display` / serde string | Documented behavior (from doc comments) |
|---|---|---|---|
| `Connecting` | **yes (default)** | `connecting` | initial state |
| `Connected` | | `connected` | ENet connected, not yet in a world |
| `InGame` | | `in_game` | in a world |
| `TwoFactorAuth` | | `two_factor_auth` | Blocked by 2FA (Advanced Account Protection). **Retry after 120 s.** |
| `ServerOverloaded` | | `server_overloaded` | Server overloaded. **Retry after 30 s.** |
| `TooManyLogins` | | `too_many_logins` | Too many logins at once. **Retry after 5 s.** |
| `UpdateRequired` | | `update_required` | Client outdated; server requires update. **Bot stopped** (no retry). |
| `Maintenance` | | `maintenance` | Server under maintenance. **Retry after 600 s.** |

There is a hand-written `Display` impl producing exactly the snake_case strings above (identical to the serde values). `BotManager::list()` uses `Display` (`.to_string()`), so the UI status string must match this table exactly.

**C++:** `enum class BotStatus { Connecting, Connected, InGame, TwoFactorAuth, ServerOverloaded, TooManyLogins, UpdateRequired, Maintenance };` + a `const char* to_string(BotStatus)` returning the exact strings above. The retry-delay numbers are **not** hard-coded in the enum — they live in `BotDelays` (§1.9); the doc comments merely describe intent. Preserve the delay values in `BotDelays`, not here.

### 1.2 `GeigerArea` (enum) — `bot_state.rs`
Derives: `Clone, Serialize`, snake_case.
Variants + `as_str()`:
| Variant | `as_str()` |
|---|---|
| `Red` | `red` |
| `Yellow` | `yellow` |
| `Green` | `green` |
| `RapidGreen` | `rapid_green` |
| `Prize` | `prize` |

**C++:** `enum class GeigerArea { Red, Yellow, Green, RapidGreen, Prize };` + `const char* as_str(GeigerArea)`.

### 1.3 `GeigerSignal` (struct) — `bot_state.rs`
Derives: `Clone, Serialize`.
| Field | Type | Notes |
|---|---|---|
| `x` | `u32` | |
| `y` | `u32` | |
| `area_type` | `GeigerArea` | **serialized as JSON key `"type"`** |
| `timestamp_ms` | `u64` | epoch millis |

**C++:** `struct GeigerSignal { uint32_t x, y; GeigerArea area_type; uint64_t timestamp_ms; };`

### 1.4 `TileInfo` (struct) — `bot_state.rs`
Derives: `Clone, Serialize`. Has an explicit `Default`.
| Field | Type | Default |
|---|---|---|
| `fg_item_id` | `u16` | `0` |
| `bg_item_id` | `u16` | `0` |
| `flags` | `u16` | `0` |
| `tile_type` | `TileType` (module `world`) | `TileType::Basic` |

`TileType` is an **external enum** (module `world`, its own port spec). Only its `Basic` variant is referenced here (as the default). **C++:** `struct TileInfo { uint16_t fg_item_id=0, bg_item_id=0, flags=0; TileType tile_type=TileType::Basic; };`

### 1.5 `PlayerInfo` (struct) — `bot_state.rs`
Derives: `Clone, Serialize`.
| Field | Type |
|---|---|
| `net_id` | `u32` |
| `name` | `String` |
| `pos_x` | `f32` |
| `pos_y` | `f32` |
| `country` | `String` |

### 1.6 `InvSlot` (struct) — `bot_state.rs`
Derives: `Clone, Serialize`.
| Field | Type |
|---|---|
| `item_id` | `u16` |
| `amount` | `u8` |
| `is_active` | `bool` |
| `action_type` | `u8` |

### 1.7 `WorldObjectInfo` (struct) — `bot_state.rs`
Derives: `Clone, Serialize`. (Dropped items / world objects.)
| Field | Type |
|---|---|
| `uid` | `u32` |
| `item_id` | `u16` |
| `x` | `f32` |
| `y` | `f32` |
| `count` | `u8` |

### 1.8 `TrackInfo` (struct) — `bot_state.rs`
Derives: `Default, Clone, Serialize`. (Account "growtoken" / profile stats.)
| Field | Type | Default |
|---|---|---|
| `level` | `u32` | 0 |
| `grow_id` | `u64` | 0 |
| `install_date` | `u64` | 0 |
| `global_playtime` | `u64` | 0 |
| `awesomeness` | `u32` | 0 |

### 1.9 `BotDelays` (struct) — `bot_state.rs`  ⚠ the only (de)serialized-to-disk type
Derives: `Clone, Serialize, **Deserialize**`. Has explicit `Default`.
| Field (JSON key) | Type | **Default** |
|---|---|---|
| `place_ms` | `u64` | **500** |
| `walk_ms` | `u64` | **500** |
| `twofa_secs` | `u64` | **120** |
| `server_overload_secs` | `u64` | **30** |
| `too_many_logins_secs` | `u64` | **5** |
| `maintenance_secs` | `u64` | **600** |

These are the concrete retry/pacing constants referenced by the `BotStatus` doc-comments. **Preserve every default exactly.** Because this derives `Deserialize`, it is loaded from config JSON; in Nxrth use `nlohmann/json` with these exact keys so existing config files remain readable, or persist via ImGui settings — but keep the defaults byte-for-byte.

**C++:**
```cpp
struct BotDelays {
    uint64_t place_ms            = 500;
    uint64_t walk_ms             = 500;
    uint64_t twofa_secs          = 120;
    uint64_t server_overload_secs= 30;
    uint64_t too_many_logins_secs= 5;
    uint64_t maintenance_secs    = 600;
};
```

### 1.10 `BotState` (struct) — `bot_state.rs`  ★ the shared per-bot snapshot
Derives: `Clone, Serialize`. Has explicit `Default`. This is the object wrapped in `RwLock` and shared between the bot's own thread (writer) and the UI/manager (readers).

| Field | Type | Default | Notes |
|---|---|---|---|
| `status` | `BotStatus` | `Connecting` | |
| `username` | `String` | `""` | |
| `mac` | `String` | `""` | spoofed MAC |
| `world_name` | `String` | `""` | |
| `pos_x` | `f32` | `0.0` | **tile coords = pixels ÷ 32** |
| `pos_y` | `f32` | `0.0` | tile coords |
| `world_width` | `u32` | `0` | |
| `world_height` | `u32` | `0` | |
| `tiles` | `Vec<TileInfo>` | empty | row-major `width*height` when in-world |
| `objects` | `Vec<WorldObjectInfo>` | empty | dropped items |
| `players` | `Vec<PlayerInfo>` | empty | |
| `inventory` | `Vec<InvSlot>` | empty | |
| `inventory_size` | `u32` | `0` | max slots (from `SendInventoryState`) |
| `gems` | `i32` | `0` | signed |
| `console` | `Vec<String>` | empty | chat/console log lines |
| `ping_ms` | `u32` | `0` | ENet RTT, refreshed each run-loop tick |
| `delays` | `BotDelays` | `BotDelays::default()` | |
| `track_info` | `Option<TrackInfo>` | `None` | |
| `geiger_signal` | `Option<GeigerSignal>` | `None` | latest decoded geiger particle |
| `auto_collect` | `bool` | **`true`** | run loop auto-picks nearby drops |
| `collect_radius_tiles` | `u8` | **`3`** | 1–5; axis-aligned square: collect if `|Δx|,|Δy| ≤ tiles*32 px` |
| `collect_blacklist` | `Vec<u16>` | empty | item ids to skip; sorted+unique in API responses |
| `auto_reconnect` | `bool` | **`true`** | auto-reconnect after disconnect |

**C++:** mirror as a struct. `Option<T>` → `std::optional<T>`. `Vec<T>` → `std::vector<T>`. `f32`→`float`, `i32`→`int32_t`, `u32`→`uint32_t`, `u8`→`uint8_t`, `u16`→`uint16_t`. Keep the three non-trivial defaults (`auto_collect=true`, `collect_radius_tiles=3`, `auto_reconnect=true`) — a value-initialized (all-zero) struct would silently disable auto-collect/reconnect and set an out-of-range radius.

### 1.11 `BotCommand` (enum, tagged union) — `bot_state.rs`
The message type flowing UI → bot over the command channel. No serde derive (internal only). Variants and payloads (**exact field names/types**):

| Variant | Payload |
|---|---|
| `Move` | `{ x: i32, y: i32 }` |
| `WalkTo` | `{ x: u32, y: u32 }` |
| `RunScript` | `{ content: String }` |
| `StopScript` | *(unit)* |
| `Say` | `{ text: String }` |
| `Warp` | `{ name: String, id: String }` |
| `Disconnect` | *(unit)* |
| `Reconnect` | *(unit)* |
| `Place` | `{ x: i32, y: i32, item: u32 }` |
| `Hit` | `{ x: i32, y: i32 }` |
| `Wrench` | `{ x: i32, y: i32 }` |
| `Wear` | `{ item_id: u32 }` |
| `Unwear` | `{ item_id: u32 }` |
| `Drop` | `{ item_id: u32, count: u32 }` |
| `Trash` | `{ item_id: u32, count: u32 }` |
| `LeaveWorld` | *(unit)* |
| `Respawn` | *(unit)* |
| `FindPath` | `{ x: u32, y: u32 }` |
| `SetDelays` | `(BotDelays)` — **tuple variant**, single positional `BotDelays` |
| `SetAutoCollect` | `{ enabled: bool }` |
| `SetCollectConfig` | `{ radius_tiles: u8, blacklist: Vec<u16> }` |
| `SetAutoReconnect` | `{ enabled: bool }` |
| `AcceptAccess` | *(unit)* |

**C++:** use `std::variant` of small structs, or a `struct BotCommand { enum class Kind{...} kind; ... }` with a union/optional payload members. A `std::variant<Move, WalkTo, RunScript, …>` maps most cleanly. Note `SetDelays` carries a whole `BotDelays` by value; `RunScript` and `Say`/`Warp` own heap strings; `SetCollectConfig` owns a `std::vector<uint16_t>` — moves, not copies, when enqueuing.

### 1.12 Channel type aliases — `bot_state.rs`
```rust
pub type CmdSender   = mpsc::Sender<BotCommand>;
pub type CmdReceiver = mpsc::Receiver<BotCommand>;
```
Unbounded, **multi-producer / single-consumer**. Producers: UI/manager threads (the stored sender + clones handed out by `find_by_name`). Consumer: the bot's own thread (owns the receiver, moved into the thread closure). See §3.14 for the exact C++ queue contract.

---

### 1.13 `BotEntry` (struct) — `bot_manager.rs`
The manager's per-bot record (one per live bot).
| Field | Type | Meaning |
|---|---|---|
| `username` | `String` | display name; **`""` for ltoken bots**, `"HAR_TOKEN_BOT"` for HAR bots |
| `proxy_key` | `Option<String>` | `"ip:port"` of the SOCKS5 proxy, or `None` if no proxy — the capacity-counting key |
| `stop_flag` | `Arc<AtomicBool>` | set `true` to ask the bot to stop |
| `state` | `Arc<RwLock<BotState>>` | shared live snapshot |
| `cmd_tx` | `CmdSender` | push commands to this bot |
| `handle` | `Option<JoinHandle<()>>` | the OS thread handle; `Option` so it can be `take()`n out for joining |

**C++:**
```cpp
struct BotEntry {
    std::string                          username;
    std::optional<std::string>           proxy_key;
    std::shared_ptr<std::atomic<bool>>   stop_flag;
    std::shared_ptr<SharedBotState>      state;   // see §5: RwLock -> shared_mutex wrapper
    CmdSender                            cmd_tx;
    std::optional<std::thread>           handle;
};
```

### 1.14 `BotManager` (struct) — `bot_manager.rs`
| Field | Type | Meaning |
|---|---|---|
| `next_id` | `u32` (private) | next id to hand out; **monotonic, never reused** |
| `bots` | `HashMap<u32, BotEntry>` (pub) | live bots keyed by id |
| `retired_threads` | `Vec<JoinHandle<()>>` (private) | handles of stopped bots awaiting a non-blocking join |
| `items_dat` | `Arc<ItemsDat>` (pub) | shared read-only item metadata, loaded once |
| `ws_tx` | `WsTx` (pub) | event sink for BotAdded/BotRemoved (Mori: web broadcast) |

**C++:**
```cpp
class BotManager {
    uint32_t next_id_ = 0;
public:
    std::unordered_map<uint32_t, BotEntry> bots;
private:
    std::vector<std::thread> retired_threads_;
public:
    std::shared_ptr<ItemsDat> items_dat;   // ItemsDat::load() once
    EventSink                 ws_tx;        // see §4: not a web socket in Nxrth
};
```
> `BotManager` has **no internal lock**. In Mori it lives inside the axum app state behind a `Mutex`/`RwLock`. In Nxrth it must be owned by the UI thread or wrapped in a `std::mutex` — see §5. All public methods assume single-threaded access to the manager's own fields.

### 1.15 `BotInfo` (struct) — `bot_manager.rs`
Derives `serde::Serialize`. A flattened read-only view row for the UI table, produced by `list()`.
| Field | Type | Source (from `BotState`) |
|---|---|---|
| `id` | `u32` | the map key |
| `username` | `String` | `BotEntry.username` (**not** `BotState.username`) |
| `status` | `String` | `BotState.status.to_string()` (Display, e.g. `"in_game"`) |
| `world` | `String` | `BotState.world_name` |
| `pos_x` | `f32` | `BotState.pos_x` |
| `pos_y` | `f32` | `BotState.pos_y` |
| `gems` | `i32` | `BotState.gems` |
| `ping_ms` | `u32` | `BotState.ping_ms` |

**C++:** plain struct; the UI consumes it directly (no JSON needed).

---

### 1.16 External types used by this module (interface only — see their own specs)
| Type | Origin module | What this module uses |
|---|---|---|
| `Socks5Config` | `bot` | moved by value (`Option<Socks5Config>`) into the bot thread; field `proxy_addr` is a `SocketAddr` with `.ip()` and `.port()` (used to build `proxy_key`). |
| `RotatingLoginProxy` | `proxy_pool` | opaque; moved by value (`Option`) into the bot thread. |
| `WsTx` / `WsEvent` | `events` | `WsTx` is cheaply **clonable** and has `.send(WsEvent) -> Result<_,_>` (errors ignored). `WsEvent` variants used here: `BotAdded { bot_id: u32, username: String }`, `BotRemoved { bot_id: u32 }`. |
| `ItemsDat` | `items` | `ItemsDat::load() -> ItemsDat`; wrapped in `Arc`, cloned (ref-count) into every bot. Read-only shared reference data. |
| `TileType` | `world` | enum; only `TileType::Basic` referenced (TileInfo default). |
| `Bot` | `bot` | constructed inside each spawn; see §3.13 for the exact constructor signatures and the `run` entry point. |

---

## 2. FUNCTIONS — `BotManager` methods (step-by-step)

All method receivers are on `BotManager`. Unless noted, methods that mutate the map take `&mut self`.

### 3.1 `new(ws_tx: WsTx) -> BotManager`
1. `next_id = 0`.
2. `bots = empty`, `retired_threads = empty`.
3. `items_dat = Arc::new(ItemsDat::load())` — loads item metadata **once**, shared by all bots.
4. store `ws_tx`.

**C++:** constructor. Load `ItemsDat` once into a `shared_ptr`.

### 3.2 `reap_finished(&mut self)`  (private) — the garbage collector; called at the top of nearly every public method
Two phases:

**Phase A — natural deaths (bots still in the map whose thread ended on its own):**
1. Collect all ids where `entry.handle` is `Some` **and** `handle.is_finished()` is true (thread has exited).
2. For each such id: `remove` the `BotEntry` from `bots`; `take()` the handle and `join()` it (ignore the join result — a panic was already caught, see §3.13); then `ws_tx.send(WsEvent::BotRemoved { bot_id: id })` (ignore send error).

**Phase B — retired threads (bots stopped by the user via `stop()`):**
1. Walk `retired_threads` with an index `i`.
2. If `retired_threads[i].is_finished()`: `swap_remove(i)` it (O(1) removal that moves the last element into slot `i`) and `join()` it. **Do not advance `i`** (the swapped-in element must be checked).
3. Else `i += 1`.

Net effect: dead/stopped threads are joined without ever blocking the manager. `is_finished()` is a **non-blocking** check; `join()` is only ever called on a thread already known to have finished, so it returns immediately.

**C++ contract:**
- `std::thread` has no `is_finished()`. Add a per-bot `std::shared_ptr<std::atomic<bool>> done_flag` that the thread body sets to `true` as its very last action (after the try/catch), and treat `done_flag->load()` as `is_finished()`. Keep the `std::thread` in `handle`/`retired_threads_`; when `done` is observed, `handle->join()` (returns instantly) then destroy/erase.
- Phase B must use the same swap-erase-without-advance pattern (`std::swap(vec[i], vec.back()); vec.pop_back();` and re-check index `i`).
- Emit `BotRemoved` in Phase A only (Phase B bots already emitted `BotRemoved` in `stop()`).

### 3.3 `spawn(username, password, proxy: Option<Socks5Config>, login_proxy_url: Option<RotatingLoginProxy>) -> u32`
The standard login path.
1. `reap_finished()`.
2. **Dedup:** if `find_id_by_name(&username)` returns `Some(existing)`, print `"[Manager] Skipped '{username}' — already loaded as bot {existing}."` and **return `existing`** (no new thread).
3. `id = next_id; next_id += 1`.
4. `stop_flag = Arc(AtomicBool(false))`; clone it (`stop_clone`).
5. Clone `username`→`uname`, `password`→`pass` for the closure.
6. `state = Arc(RwLock(BotState{ status: Connecting, ..Default::default() }))`; clone it (`state_clone`).
7. Create the command channel `(cmd_tx, cmd_rx)`.
8. Clone `items_dat` (ref-count), clone `ws_tx` (`ws_tx_clone`), compute `assigned_proxy_key = proxy_key(&proxy)` **before** `proxy` is moved into the thread.
9. **Spawn thread**: `Builder::name("mori-bot-{id}").stack_size(1024*1024).spawn(closure).expect("failed to spawn bot thread")`.
   - Thread body wraps everything in `catch_unwind(AssertUnwindSafe(...))`:
     - `bot = if uname.contains('|') { Bot::new_ltoken(&uname, proxy, login_proxy_url, stop_clone.clone(), state_clone, cmd_rx, items_dat, id, Some(ws_tx_clone)) } else { Bot::new(&uname, &pass, proxy, login_proxy_url, stop_clone.clone(), state_clone, cmd_rx, items_dat, id, Some(ws_tx_clone)) }`
     - if `Some(bot)`: `bot.run(stop_clone)`.
   - On `Ok(_)` print `"[Bot:{id}] Stopped."`; on `Err(_)` (panic caught) print `"[Bot:{id}] Crashed."`.
10. `bots.insert(id, BotEntry{ username: username.clone(), proxy_key: assigned_proxy_key, stop_flag, state, cmd_tx, handle: Some(handle) })`.
11. `ws_tx.send(WsEvent::BotAdded { bot_id: id, username })`.
12. return `id`.

> **ltoken auto-detection:** a `username` containing `'|'` is treated as an **ltoken string** → `new_ltoken` (password ignored). This exact substring check must be preserved.

### 3.4 `spawn_requestly(username, password, proxy, login_proxy_url) -> u32`
**Identical to `spawn` in every step**, except in the non-ltoken branch it calls `Bot::new_requestly(&uname, &pass, …)` instead of `Bot::new(…)`. The ltoken branch is still `Bot::new_ltoken`. (Same `'|'` detection, same dedup, same thread name `mori-bot-{id}`, same insert/events.)

### 3.5 `spawn_newly(username, password, proxy, login_proxy_url) -> u32`
Same skeleton as `spawn`, **but there is NO ltoken branch** — it *always* calls `Bot::new_newly(&uname, &pass, proxy, login_proxy_url, stop_clone.clone(), state_clone, cmd_rx, items_dat, id, Some(ws_tx_clone))`, even if the username contains `'|'`. Dedup, thread name, insert, and `BotAdded` are identical.

### 3.6 `spawn_ltoken(ltoken_str: String, proxy, login_proxy_url) -> u32`
For adding a bot directly from a login token.
1. `reap_finished()`.
2. **No dedup check** (there is no username to match on).
3. `id = next_id; next_id += 1`.
4. Build `stop_flag`, `state` (status `Connecting`), channel, clone `items_dat`/`ws_tx`, `assigned_proxy_key = proxy_key(&proxy)`.
5. Spawn thread `"mori-bot-{id}"`, stack 1 MiB, `catch_unwind`:
   - `bot = Bot::new_ltoken(&ltoken_str, proxy, login_proxy_url, stop_clone.clone(), state_clone, cmd_rx, items_dat, id, Some(ws_tx_clone))`
   - if `Some`: `bot.run(stop_clone)`. Same `Ok`/`Err` prints.
6. `bots.insert(id, BotEntry{ username: **String::new()** (empty), proxy_key, stop_flag, state, cmd_tx, handle })`.
7. `ws_tx.send(WsEvent::BotAdded { bot_id: id, username: String::new() })`.
8. return `id`.

> Because `username` is empty, `find_id_by_name` will **never** match this bot (it returns `None` on empty input), so ltoken bots are never deduped and can be added multiple times. Preserve this.

### 3.7 `spawn_har_token(har_path: String, proxy, login_proxy_url) -> u32`
For adding a bot from a captured HAR file (extracts the token from an exported browser HAR).
- Same as `spawn_ltoken` but:
  - thread calls `Bot::new_har_token(&har_path, proxy, login_proxy_url, …)`.
  - `BotEntry.username = "HAR_TOKEN_BOT"` (literal), and `BotAdded.username = "HAR_TOKEN_BOT"`.
- No dedup check. Thread name `mori-bot-{id}`.

> **Caveat:** because all HAR bots share the literal username `"HAR_TOKEN_BOT"`, `find_id_by_name("HAR_TOKEN_BOT")` / `stop_by_name` would collide across multiple HAR bots (matches the first found). This is pre-existing Mori behavior; preserve it unless Nxrth deliberately fixes it.

### 3.8 `stop(&mut self, id: u32) -> bool`  — non-blocking stop
1. `reap_finished()`.
2. `remove` the `BotEntry` for `id`. If absent → return `false`.
3. Send `BotCommand::Disconnect` on its `cmd_tx` (ignore error).
4. `stop_flag.store(true, Relaxed)` — signals the bot's run loop to exit.
5. Push the (owned) `handle` into `retired_threads` — **do not join here** (join would block until the bot actually winds down its ENet/session). The next `reap_finished` joins it once finished.
6. `ws_tx.send(WsEvent::BotRemoved { bot_id: id })`.
7. return `true`.

> Ordering matters: send `Disconnect` **and** set the atomic — the bot may be blocked waiting on the command channel *or* spinning in its run loop, so both wake-paths are triggered. In C++, sending on the queue must also notify the condvar; setting the atomic must be observable by the run loop.

### 3.9 `list(&mut self) -> Vec<BotInfo>`
1. `reap_finished()`.
2. For each `(id, entry)` in `bots`: acquire a **read** lock on `entry.state` (poison-tolerant: `read().unwrap_or_else(PoisonError::into_inner)` — i.e. read even if a previous writer panicked), and build a `BotInfo` (see §1.15 mapping). `status = state.status.to_string()`.
3. Collect into a `Vec`. **Order is unspecified** (HashMap iteration order); the UI should sort by `id` if a stable order is wanted.

**C++:** take a shared (read) lock on each bot's `shared_mutex`. No poisoning exists in C++ `shared_mutex`, so just `lock_shared`. Return `std::vector<BotInfo>`; sort by `id` in the UI layer for a stable table.

### 3.10 `proxy_key_counts(&self) -> HashMap<String, usize>`  — proxy capacity counting
1. `counts = empty map`.
2. For each `entry` in `bots.values()`: if `entry.proxy_key` is `Some(key)`, `counts[key] += 1`.
3. return `counts`.

This is how the fleet knows **how many bots currently sit behind each `ip:port` proxy endpoint**. Bots with `proxy_key == None` (no proxy) are not counted. The *capacity limit* itself (max bots per proxy) is enforced by the **caller** (proxy pool / spawn orchestration), not here — this method only reports current occupancy. **C++:** `std::unordered_map<std::string,size_t>`. Note this is `&self` (const) — no `reap_finished` — so a just-stopped-but-not-yet-reaped bot is already gone from `bots` (removed in `stop`), so counts are current.

### 3.11 `get_state(&self, id: u32) -> Option<BotState>`
`bots.get(id)` → read-lock (poison-tolerant) → **clone** the whole `BotState` → `Some(clone)`; `None` if id absent. Returns a deep copy (snapshot), not a reference. **C++:** `std::optional<BotState>` by value; lock_shared, copy, unlock.

### 3.12 `send_cmd(&self, id: u32, cmd: BotCommand) -> bool`
`bots.get(id).map(|e| e.cmd_tx.send(cmd).is_ok()).unwrap_or(false)`.
- Returns `false` if the id is unknown **or** the send failed (receiver gone — bot thread already ended).
- **C++:** look up entry; `return entry.cmd_tx.try_send(std::move(cmd));` where the queue's `try_send` returns `false` if the consumer side is closed (see §3.14). No `reap_finished` here.

### 3.13 `run_script(&self, id: u32, content: String) -> bool`
Thin wrapper: `send_cmd(id, BotCommand::RunScript { content })`.

### 3.14 `find_id_by_name(&self, name: &str) -> Option<u32>`
1. If `name` is empty → `None`.
2. Find the first entry whose `username.eq_ignore_ascii_case(name)` (ASCII case-insensitive) → return its `id`, else `None`.

Used by the `spawn*` dedup: since stopped bots are removed from the map, a match here means the account is **genuinely still live**. **C++:** case-insensitive ASCII compare (fold `A–Z`↔`a–z` only; do **not** use locale-aware `tolower`). Empty name → `nullopt`.

### 3.15 `find_by_name(&self, name: &str) -> Option<(Arc<RwLock<BotState>>, CmdSender)>`
First entry with `username.eq_ignore_ascii_case(name)` → return **clones** of `(state handle, cmd_tx)`, so the caller can read state / send commands without holding the manager. `None` if no match. (Note: unlike `find_id_by_name`, this does **not** early-return on empty name — but an empty name won't match any non-empty username; it *would* match ltoken bots whose username is `""`. Preserve as-is.) **C++:** return `std::optional<std::pair<std::shared_ptr<SharedBotState>, CmdSender>>` (both are shared handles — copy the shared_ptr and the sender).

### 3.16 `stop_by_name(&mut self, name: &str) -> bool`
Find the id of the first entry with `username.eq_ignore_ascii_case(name)`; if found call `self.stop(id)`, else return `false`.

### 3.17 free function `proxy_key(proxy: &Option<Socks5Config>) -> Option<String>`
`proxy.as_ref().map(|p| format!("{}:{}", p.proxy_addr.ip(), p.proxy_addr.port()))`.
- `None` → `None`.
- `Some` → the string `"<ip>:<port>"` (e.g. `"203.0.113.7:1080"`). This is the capacity key used everywhere. **C++:** `std::optional<std::string> proxy_key(const std::optional<Socks5Config>&)` formatting `ip + ":" + port`. Match the IP rendering of the source `SocketAddr` (IPv4 dotted, IPv6 as the address without brackets since `format!("{}", ip)` prints the bare `IpAddr`).

---

### 3.13-ext External `Bot` constructor + run interface (needed to write the thread body)
All constructors return `Option<Bot>` (`None` = login/construction failed → the thread simply ends, its handle finishes, and `reap_finished` cleans it up and emits `BotRemoved`). Signatures as invoked here:

```
Bot::new(uname:&str, pass:&str, proxy:Option<Socks5Config>, login_proxy_url:Option<RotatingLoginProxy>,
         stop:Arc<AtomicBool>, state:Arc<RwLock<BotState>>, cmd_rx:CmdReceiver, items_dat:Arc<ItemsDat>,
         id:u32, ws_tx:Option<WsTx>) -> Option<Bot>
Bot::new_requestly(...same as new...) -> Option<Bot>
Bot::new_newly(...same as new...) -> Option<Bot>
Bot::new_ltoken(ltoken:&str, proxy, login_proxy_url, stop, state, cmd_rx, items_dat, id, ws_tx) -> Option<Bot>   // no password
Bot::new_har_token(har_path:&str, proxy, login_proxy_url, stop, state, cmd_rx, items_dat, id, ws_tx) -> Option<Bot>
bot.run(stop: Arc<AtomicBool>)   // blocking main loop; returns when stopped/disconnected
```
The thread **moves** `proxy`, `login_proxy_url`, `cmd_rx`, `items_dat` (Arc), `ws_tx` (clone), `state_clone`, `stop_clone` into itself. In C++ these become move-captured members of the bot object created on the new thread. See the `bot` module port spec for the constructor internals.

---

## 3. DEPENDENCY MAPPING (Rust crate/feature → Nxrth C++)

| Rust (in these files) | Nxrth C++ | Notes for this module |
|---|---|---|
| `std::thread::Builder` (named, `stack_size`) | `std::thread` | C++ can't set stack size or name portably via `std::thread`; use platform hooks: on Win32 name via `SetThreadDescription(L"nxrth-bot-<id>")`; set stack size via a `boost::thread`/`std::thread` with an attribute shim or accept the default (1 MiB is small — the OS default is usually larger, which is fine). Preserve the **name string** `nxrth-bot-<id>`. |
| `std::panic::catch_unwind` + `AssertUnwindSafe` | `try { … } catch (...) {}` around the whole thread body | One bot's exception must never call `std::terminate`. Print `"[Bot:<id>] Crashed."` in the catch, `"[Bot:<id>] Stopped."` on normal return. |
| `std::sync::mpsc::{Sender,Receiver}` (unbounded MPSC) | **`std::mutex` + `std::condition_variable` queue** (the "crossbeam-channel → mutex+condvar" rule) | See §3.14 contract. Unbounded FIFO, multi-producer, single-consumer, closable. |
| `Arc<T>` | `std::shared_ptr<T>` | ref-counted shared ownership (`items_dat`, `stop_flag`, `state`, `ws_tx` clones). |
| `RwLock<BotState>` | `std::shared_mutex` guarding a `BotState` (wrap as `SharedBotState`) | many readers (UI), one writer (bot thread). No poisoning in C++. |
| `AtomicBool` (`Ordering::Relaxed`) | `std::atomic<bool>` (use `memory_order_relaxed` to match, or `seq_cst` for safety) | stop flag. |
| `HashMap<u32,_>` / `HashMap<String,usize>` | `std::unordered_map` | bot registry / proxy counts. |
| `serde::Serialize` / `Deserialize` | `nlohmann/json` **only where JSON is actually needed** | Nxrth has no web server (tokio/axum/tower → `std::thread` + Dear ImGui). The only real (de)serialization is `BotDelays` config; everything else is read straight by ImGui. Keep the JSON key/value names from §1 if you persist `BotDelays`. |
| `ureq`/`reqwest`, `mlua`, `scraper`, `rusty_enet`, `md5`, `argon2` | libcurl(socks5h,cookies) / **native C++ (no Lua)** / regex+manual HTML / vendored patched ENet / bundled md5 / argon2 lib | **Not used in these two files** — they belong to the `bot`/login/proxy modules. `RunScript{content}` here is just an opaque string routed to the bot; in Nxrth the "script" engine is **native C++**, not Lua — the command payload stays a `std::string` but the executor is rewritten (see the bot/script port spec). |
| `WsTx` broadcast (tokio/axum) | in-process `EventSink` (see §4) | `BotAdded`/`BotRemoved` become UI notifications / fleet-registry mutations, **not** websocket frames. |

---

## 4. THREADING & SHARED STATE

### 5.1 Current Mori model (must be reproduced faithfully)
- **One OS thread per bot.** Spawned with name `mori-bot-<id>` (→ `nxrth-bot-<id>`) and a **1 MiB stack**. The thread owns the `Bot` object and runs `bot.run(stop_flag)` until stopped or disconnected.
- **Panic isolation:** each thread body is wrapped in `catch_unwind`; a panic prints `"[Bot:<id>] Crashed."` and the thread ends cleanly. The fleet keeps running.
- **Three shared handles per bot** bridge thread ↔ manager/UI:
  1. `stop_flag: Arc<AtomicBool>` — UI sets it (`stop`); bot polls it in `run`.
  2. `state: Arc<RwLock<BotState>>` — **bot is the sole writer**, UI/manager are readers (`list`, `get_state`). Reads are poison-tolerant.
  3. `cmd_tx/cmd_rx: mpsc` — UI/manager are producers, bot is the single consumer.
- **The `BotManager` itself is not internally synchronized.** In Mori it's held in axum shared state behind an outer lock. **Nxrth must own it on the UI thread or wrap it in a `std::mutex`.** Every `spawn*/stop/list/reap` call mutates `bots`/`retired_threads`/`next_id` and must be serialized.
- **Lifecycle / reaping is cooperative and non-blocking:**
  - A bot that ends on its own → thread finishes → next `reap_finished` (Phase A) joins it, removes it, emits `BotRemoved`.
  - A user `stop()` → removes it immediately, signals it (Disconnect + atomic), parks the handle in `retired_threads`, emits `BotRemoved` right away; the join happens later in `reap_finished` (Phase B). This keeps the UI responsive — `stop` never blocks on ENet teardown.
- **`items_dat: Arc<ItemsDat>`** is the one piece of genuinely shared, read-only fleet data today — loaded once, ref-counted into every bot.
- **`next_id`** guarantees globally unique, never-recycled ids (stable across the process lifetime).

### 5.2 C++ command-queue contract (replacing `mpsc`)
```cpp
template <class T>
class CommandQueue {                 // MPSC, unbounded, closable
    std::mutex m; std::condition_variable cv;
    std::deque<T> q; bool consumer_closed = false;
public:
    // producer side (CmdSender is a shared_ptr<CommandQueue>)
    bool try_send(T v){ std::lock_guard lk(m);
        if(consumer_closed) return false;      // == mpsc send Err (receiver gone)
        q.push_back(std::move(v)); cv.notify_one(); return true; }
    // consumer side (bot thread)
    std::optional<T> recv_timeout(std::chrono::milliseconds t){ std::unique_lock lk(m);
        cv.wait_for(lk,t,[&]{return !q.empty();});
        if(q.empty()) return std::nullopt;
        T v=std::move(q.front()); q.pop_front(); return v; }
    void close_consumer(){ std::lock_guard lk(m); consumer_closed=true; } // call when bot thread exits
};
using CmdSender = std::shared_ptr<CommandQueue<BotCommand>>;
```
- `send_cmd` → `try_send` returns `false` when the id is gone or the consumer closed (matches Mori's `is_ok()`).
- The bot thread must `close_consumer()` on exit so late `try_send`s fail rather than leak.
- `stop()` must both `try_send(Disconnect)` (wakes a queue-blocked bot) **and** set the atomic (wakes a run-loop-spinning bot).

### 5.3 Evolving into a **shared fleet** (Nxrth requirement: bots must see each other)
Today bots are isolated — no bot can observe another; only the manager has the global view. Nxrth needs a **shared fleet state** that every bot thread can read (and minimally write). Recommended design:

```cpp
// One instance, owned by BotManager, shared_ptr handed to every bot.
struct FleetMemberView {              // compact, copyable snapshot of one bot
    uint32_t    id;
    std::string username;
    BotStatus   status;
    std::string world_name;
    float       pos_x, pos_y;
    int32_t     gems;
    uint32_t    ping_ms;
    std::optional<std::string> proxy_key;   // for peer-aware proxy balancing
};

class FleetState {                    // the shared "bots see each other" registry
    mutable std::shared_mutex mtx;
    std::unordered_map<uint32_t, FleetMemberView> members;   // keyed by bot id
public:
    void upsert(const FleetMemberView& v);      // a bot publishes its own view each tick
    void erase(uint32_t id);                     // on stop/reap
    std::vector<FleetMemberView> snapshot() const;                // any bot: whole fleet
    std::vector<FleetMemberView> in_world(std::string_view w) const; // peers in same world
    size_t count_on_proxy(std::string_view key) const;              // live proxy occupancy
};
using FleetHandle = std::shared_ptr<FleetState>;
```

Wiring:
- `BotManager` constructs one `FleetState` and stores a `FleetHandle`; every `spawn*` passes the same `FleetHandle` into the bot (extra constructor arg), exactly as `items_dat`/`ws_tx` are passed today.
- Each bot, once per run-loop tick (right after it updates its own `BotState`), calls `fleet->upsert(myView)` with `id`, `username`, `status`, `world_name`, `pos`, `gems`, `ping_ms`, `proxy_key`. Cheap: one `unique_lock` on a small map.
- `stop()`/`reap_finished()` also call `fleet->erase(id)` so the registry stays in sync with `bots` (keep them consistent — erase in the same places you emit `BotRemoved`).
- **This subsumes `proxy_key_counts()`**: `FleetState::count_on_proxy` gives the same capacity number, now readable by *any* bot (not just the manager), enabling peer-aware proxy selection. Keep `BotManager::proxy_key_counts()` too (it's the authoritative, map-derived count) but derive UI/coordination from `FleetState` for bot-to-bot awareness.
- Concurrency: `FleetState` is the one **many-writer, many-reader** structure — use `std::shared_mutex` (writes = `upsert`/`erase`, reads = `snapshot`/`in_world`/`count_on_proxy`). Keep the critical sections tiny (copy in/out, no bot logic under the lock) to avoid contention as fleet size grows.
- **Scaling note (from prior Mori experience):** as bot count grows, per-tick global work must stay O(1)/bot. `upsert` is O(1); avoid any per-tick `snapshot()` of the whole fleet inside hot paths. Onboarding/geiger CPU load already stresses the fleet, so keep the shared-state writes lock-cheap and batched to one per tick.
- **The `EventSink` (former `WsTx`)** in Nxrth is just an in-process notifier the UI polls; `BotAdded{bot_id,username}`/`BotRemoved{bot_id}` become fleet-registry mutations + a UI dirty flag, **not** websocket frames. Keep the two event shapes so the ImGui layer can react.

### 5.4 Manager ownership in Nxrth
Wrap `BotManager` in `std::mutex` (or own it solely on the ImGui thread). All `spawn*`, `stop`, `stop_by_name`, `list`, `reap_finished`, `get_state`, `send_cmd`, `run_script`, `find_*`, `proxy_key_counts` mutate/iterate manager-owned fields and must run under that single lock. The per-bot `SharedBotState`/`CommandQueue`/`FleetState` have their **own** finer-grained locks and are safe to touch without the manager lock once you hold a `shared_ptr` to them (that's exactly what `find_by_name` hands out).

---

## 5. RENAME RULES — concrete occurrences in these two files

`Mori`/`mori` → `Nxrth`/`nxrth`; `Cloei`/`cloei` → `North`/`north`.

**`bot_manager.rs`:**
- Thread name literal `format!("mori-bot-{id}")` → `format!("nxrth-bot-{id}")`. **Occurs 5×** — once each in `spawn`, `spawn_requestly`, `spawn_newly`, `spawn_ltoken`, `spawn_har_token`. This is the **only** literal `mori` token in the file. Preserve the format (`nxrth-bot-<id>`) as the OS thread name (Win32 `SetThreadDescription`).
- Log tags `"[Manager] …"`, `"[Bot:{id}] Stopped."`, `"[Bot:{id}] Crashed."` contain **no** Mori/Cloei token — leave verbatim (or optionally namespace `[Manager]`→`[Nxrth/Manager]`, not required).

**`bot_state.rs`:**
- **No `Mori`/`mori`/`Cloei`/`cloei` tokens present.** Nothing to rename in this file.

**Not present in either file (so nothing to change here, but watch for them when this module is wired up elsewhere):** user-agent strings, config filenames, window titles, URL paths, `Cloei`/`cloei` author strings. If the config file that persists `BotDelays` is named after Mori elsewhere (e.g. `mori.json`/`mori_config.*`), rename to `nxrth.*` in that module.

---

## 6. Implementation checklist (Nxrth)

1. Port all §1 value types as plain C++ structs/enums with the **exact** field names, types, and defaults (esp. `BotDelays` numbers and `BotState`'s `auto_collect=true`, `collect_radius_tiles=3`, `auto_reconnect=true`).
2. Implement `BotStatus`/`GeigerArea` string mappings exactly (used by the UI status column).
3. Implement `CommandQueue<BotCommand>` (§3.14) — MPSC, unbounded, closable.
4. Implement `SharedBotState` = `shared_mutex`-guarded `BotState`.
5. Implement `BotManager` with a monotonic `next_id`, `unordered_map<uint32_t,BotEntry>`, `retired_threads_`, and the exact `reap_finished` two-phase logic (Phase A joins+erases+emits `BotRemoved`; Phase B swap-erases retired threads).
6. Implement all `spawn*` variants with: `reap_finished` first, dedup via `find_id_by_name` (only `spawn`/`spawn_requestly`/`spawn_newly`), the `'|'` ltoken auto-detect in `spawn`/`spawn_requestly` only, the correct `Bot::new_*` selection, thread name `nxrth-bot-<id>`, per-thread `try/catch`, and the `BotAdded` emit. Empty username for ltoken bots, `"HAR_TOKEN_BOT"` for HAR bots.
7. Implement `stop` (non-blocking: Disconnect + atomic + park handle + immediate `BotRemoved`), `list`, `get_state`, `send_cmd`, `run_script`, `find_id_by_name` (ASCII case-insensitive, empty→none), `find_by_name`, `stop_by_name`, `proxy_key_counts`, and the free `proxy_key` (`"ip:port"`).
8. Add the §5.3 `FleetState` shared registry and thread it into every bot so bots can see each other; keep it consistent with `bots` on spawn/stop/reap.
9. Own `BotManager` under a single `std::mutex` (or the ImGui thread); keep per-bot locks separate.
10. Apply the §7 renames (`nxrth-bot-<id>` ×5).
```
```

**Files:** the spec is written to `C:/Users/ebuxd/OneDrive/Masaüstü/Nxrth/docs/port-specs/09-fleet.md`.
