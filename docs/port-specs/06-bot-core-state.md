# Port Spec 06 — Bot Core & State (`bot/core.rs`, lines ~1–1760 + gate/const helpers)

Single source of truth for the C++ (Nxrth) port of the Mori `Bot` core: the `Bot` struct,
its constructors, the ENet host wrapper, the run/service loop, connect/reconnect/refresh flow,
and the fleet-wide "stagger gate" throttling system. An engineer must be able to reimplement
this module in C++ **without reading the Rust**.

> RENAME NOTE (applies to the whole spec): all identifiers/strings below are shown in their
> original Mori form. When porting, apply the rename rules in §5 (Mori→Nxrth, Cloei→North).
> This module contains exactly **one** literal `Mori` occurrence (line 167 doc comment); there
> are **no** `Cloei` occurrences here.

---

## 0. Source-of-truth constants (from `src/constants.rs`)

These are consumed all over this module (login packet, `default_klv`, `LoginInfo`). Copy them
verbatim into an Nxrth `constants.h`. GT is **little-endian**.

| Name | Type | Value |
|---|---|---|
| `PROTOCOL` | u32 | `226` |
| `GAME_VER` | &str | `"5.51"` |
| `FHASH` | i32 | `-716928004` |
| `DEFAULT_RID` | &str | `"025B42980AFB659E0394C846233653FF"` |
| `DEFAULT_MAC` | &str | `"74:d4:dd:6c:24:e1"` |
| `DEFAULT_WK` | &str | `"788E366E74D2D098398A35C3F6360DDA"` |
| `DEFAULT_HASH` | &str | `"381621508"` |
| `DEFAULT_HASH2` | &str | `"-332772458"` |
| `DEFAULT_FZ` | &str | `"18274296"` |
| `DEFAULT_PLATFORM_ID` | &str | `"15,1,0"` |
| `DEFAULT_ZF` | &str | `"1597752569"` |
| `DEFAULT_STEAM_TOKEN` | &str | long hex-blob string ending `...5d.240` (copy byte-for-byte from `constants.rs`) |

Version context (do not "fix"): protocol **226** is accepted at the ENet logon; `GAME_VER` must be
`"5.51"` because 5.50 gets `UPDATE REQUIRED` since the 2026-07-11 force-update.

---

## 1. TYPES

### 1.1 Module-level constants defined in `core.rs`

Stagger/backoff tuning. Reproduce exactly (all are `u64` milliseconds unless noted).

| Const | Type | Value | Meaning |
|---|---|---|---|
| `LOGIN_PACKET_STAGGER_MS` | u64 | `1000` | Min spacing between each bot sending its login packet (post-ServerHello). |
| `ENTER_GAME_STAGGER_MS` | u64 | `2000` | Min spacing between each bot's `action|enter_game`. |
| `HTTP_LOGIN_STAGGER_MS` | u64 | `2500` | Min spacing between each bot's HTTP login phase (server_data/dashboard/growid). Serializes HTTP login fleet-wide to dodge simultaneous-login throttle (403 / "Too many people logging in"). |
| `WARP_STAGGER_MS` | u64 | `1200` | Min spacing between each bot's world warp (subserver redirect burst control). |
| `GATEWAY_THROTTLE_COOLDOWN_MS` | u64 | `32_000` | Process-global cooldown latched on gateway logon throttle ("Fail to login. Please try again in 30 seconds"). |
| `GATEWAY_LOGON_SPACING_MS` | u64 | `1500` | Spacing between fleet gateway-logon retries after a throttle cooldown. |
| `DASHBOARD_STAGGER_MS` | u64 | `3500` | Fleet-wide min spacing between growid dashboard POSTs. |
| `REDIRECT_MAX_GAME_PROXY_TRIES` | u8 | `6` | Max game proxies tried for one redirect's subserver connect before abandoning to a full fresh gateway logon. |
| `GEIGER_COUNTER_ITEM_ID` | u16 | `2204` | (referenced elsewhere; carry the const) |
| `DEAD_GEIGER_COUNTER_ITEM_ID` | u16 | `2286` | (referenced elsewhere; carry the const) |
| `GATE_CONNECTED_MAX_AHEAD_MS` | u64 | `2500` | Backlog ceiling for connected-phase gates (LOGIN_PACKET/ENTER_GAME/WARP). Must stay under server's post-ServerHello login timeout — a bot holds a live ENet connection while it waits. |
| `GATE_HTTP_MAX_AHEAD_MS` | u64 | `300_000` | Backlog ceiling for the pre-connection HTTP-login stagger (dead sleep; no live connection). 5 min → preserves true 2.5 s spacing for large fleets. |

### 1.2 Global stagger GATES (process-static, shared across ALL bots)

Rust type is `OnceLock<Mutex<std::time::Instant>>` — a lazily-initialized, mutex-guarded
monotonic timestamp holding "next allowed slot".

| Static | Purpose |
|---|---|
| `LOGIN_PACKET_GATE` | Paces the login packet send after ServerHello. |
| `ENTER_GAME_GATE` | Paces `action|enter_game`. |
| `HTTP_LOGIN_GATE` | Paces the whole HTTP login phase (constructors + reconnect + refresh). |
| `WARP_GATE` | Paces world warps. |
| `GATEWAY_LOGON_GATE` | Fleet reconnect scheduler after a gateway throttle (cooldown + single-file retries). |
| `DASHBOARD_GATE` | Paces growid dashboard POSTs. |

**C++ mapping.** Each gate = one process-global `struct Gate { std::mutex m; std::chrono::steady_clock::time_point next_allowed; bool inited; };` (or a `std::atomic`-guarded lazy init). Use `std::chrono::steady_clock` (monotonic) — the Rust `Instant` is monotonic, **not** wall clock. These are the primary fleet-wide shared state of this module (see §4).

### 1.3 `enum BotHost`

ENet host wrapper that abstracts over a direct UDP socket vs a SOCKS5-UDP socket.

```
enum BotHost {
    Direct(enet::Host<UdpSocket>),      // plain UDP host
    Socks5(enet::Host<Socks5UdpSocket>) // UDP tunneled through SOCKS5 UDP-ASSOCIATE
}
```

**C++ mapping.** Vendored C ENet (patched for SOCKS5-UDP). Model as a small class holding an
`ENetHost*` plus a flag/strategy for whether its underlying socket is direct or SOCKS5-tunneled.
The Rust `enum` split exists only because the two socket types differ at compile time; in C++ the
socket abstraction (a patched ENet `socket` backend) makes one `BotHost` class sufficient. Methods
(all forward to the ENet peer/host): `next_event`, `connect`, `peer_rtt`, `peer_send`,
`peer_disconnect`, `peer_set_timeout`. See §2 for exact behavior.

### 1.4 `enum LoginMethod`

How the bot authenticates; also drives token-refresh fallback (see `refresh_token`, §2).

```
enum LoginMethod {
    Legacy   { password: String },   // GrowID login; on refresh, re-login with password (fetch_credentials)
    Newly    { password: String },   // Original Mori-style GrowID login, no HAR fallbacks (fetch_newly_credentials)
    Requestly{ password: String },   // HAR-backed dashboard login; on refresh replays requestly_logs.har (fetch_requestly_credentials)
    Ltoken,                          // token supplied directly; on refresh failure STOP the bot (no fallback)
    HarToken { har_path: String },   // token extracted from a local .har file; bypasses server_data.php; on refresh re-extracts from same HAR
}
```

**C++ mapping.** `std::variant` or a tagged struct `{ Method method; std::string password_or_harpath; }`.

### 1.5 `struct RedirectData` (`#[derive(Clone)]`)

Captured from `OnSendToServer`. Subserver tokens are consumed once.

```
struct RedirectData {
    server: String,
    port: u16,
    token: String,
    user: String,
    door_id: String,
    uuid: String,
    lmode: String,
    tank_id_name: String,
}
```

### 1.6 `struct GeigerGreenRepeat` (`#[derive(Clone, Copy)]`)

Tracks repeated green Geiger particles so rapid-green can be distinguished.

```
struct GeigerGreenRepeat { x: u32, y: u32, last_seen_ms: u64, count: u8 }
```

### 1.7 `struct Bot` — THE core struct (every field, in order)

Field visibility: `pub` fields are read by other modules (UI/script/manager); private fields are
internal. Port all fields; C++ types shown in the right column.

| # | Field | Rust type | pub? | C++ type | Purpose |
|---|---|---|---|---|---|
| 1 | `host` | `BotHost` | | `BotHost` | ENet host (direct or SOCKS5). |
| 2 | `proxy` | `Option<Socks5Config>` | pub | `std::optional<Socks5Config>` | Assigned **game** proxy (world/subserver UDP). |
| 3 | `login_proxy` | `Option<RotatingLoginProxy>` | | `std::optional<RotatingLoginProxy>` | Rotating HTTP login proxy (per-fetch fresh URL). |
| 4 | `bypass_enet` | `Option<Socks5Config>` | | `std::optional<Socks5Config>` | Pinned bypass exit IP for the ENet gateway logon (`protocol|225`), matching the IP that minted the current `ltoken`. `None` = logon uses the assigned game proxy. |
| 5 | `stop` | `Arc<AtomicBool>` | | `std::shared_ptr<std::atomic<bool>>` | Shared stop flag; login/relogin loops bail immediately when set. |
| 6 | `username` | `String` | pub | `std::string` | GrowID username (empty for ltoken/HAR until OnSpawn). |
| 7 | `login_method` | `LoginMethod` | | `LoginMethod` | See §1.4. |
| 8 | `ltoken` | `String` | | `std::string` | Legacy/login token, used in first ServerHello login packet. |
| 9 | `meta` | `String` | | `std::string` | `meta` from server_data.php, echoed in all login packets. |
| 10 | `mac` | `String` | pub | `std::string` | Per-session random identity value. |
| 11 | `hash` | `String` | | `std::string` | " |
| 12 | `hash2` | `String` | | `std::string` | " |
| 13 | `klv` | `String` | | `std::string` | Computed login-key value (see `default_klv`). |
| 14 | `fz` | `String` | | `std::string` | " |
| 15 | `game_version` | `String` | | `std::string` | " |
| 16 | `cbits` | `String` | | `std::string` | " |
| 17 | `player_age` | `String` | | `std::string` | " |
| 18 | `gdpr` | `String` | | `std::string` | " |
| 19 | `category` | `String` | | `std::string` | " |
| 20 | `total_playtime` | `String` | | `std::string` | " |
| 21 | `country` | `String` | | `std::string` | " |
| 22 | `zf` | `String` | | `std::string` | " |
| 23 | `platform_id` | `String` | | `std::string` | " |
| 24 | `steam_token` | `String` | | `std::string` | " |
| 25 | `wk` | `String` | | `std::string` | " |
| 26 | `rid` | `String` | | `std::string` | " |
| 27 | `redirect` | `Option<RedirectData>` | | `std::optional<RedirectData>` | Set by `OnSendToServer`; consumed by next redirected ServerHello. |
| 28 | `redirect_attempts` | `u8` | | `uint8_t` | Whether current redirect token already sent (count). |
| 29 | `last_redirect_token` | `Option<String>` | | `std::optional<std::string>` | Last concrete subserver token (some redirects send `-1` markers). |
| 30 | `last_redirect_uuid` | `Option<String>` | | `std::optional<std::string>` | Last concrete subserver UUIDToken. |
| 31 | `refresh_token_on_reconnect` | `bool` | | `bool` | Forces next non-redirect reconnect to obtain a fresh token first. |
| 32 | `server_addr` | `Option<SocketAddr>` | | `std::optional<sockaddr/endpoint>` | Gateway address to connect to. |
| 33 | `saw_server_hello` | `bool` | | `bool` | True once ServerHello seen this connection. |
| 34 | `connected_since` | `Option<Instant>` | | `std::optional<steady_clock::time_point>` | When current peer connected; drives the 30 s login watchdog. |
| 35 | `was_in_world` | `bool` | | `bool` | True once fully in-world (OnSpawn self). Distinguishes in-world drop from gateway rejection. |
| 36 | `redirect_connect_fails` | `u8` | | `uint8_t` | Consecutive failed subserver connects (rotate game proxy each; abandon after `REDIRECT_MAX_GAME_PROXY_TRIES`). |
| 37 | `pre_hello_disconnects` | `u8` | | `uint8_t` | Consecutive disconnects before ServerHello. |
| 38 | `login_reject_streak` | `u32` | | `uint32_t` | Consecutive ServerHello+`225` logons dropped with no redirect (exit IP rate-limited). |
| 39 | `login_throttle_streak` | `u32` | | `uint32_t` | Consecutive `logon_fail` "login throttled" responses. |
| 40 | `in_gate_wait` | `bool` | | `bool` | Re-entrancy guard so a `service_once` during a gate wait can't recurse into another gate wait. |
| 41 | `start_time` | `Instant` | | `steady_clock::time_point` | For network-time in ping replies. |
| 42 | `pos_x` | `f32` | pub | `float` | World position (pixels). |
| 43 | `pos_y` | `f32` | pub | `float` | " |
| 44 | `local` | `LocalPlayer` | pub | `LocalPlayer` | Bot's own identity in the current world. |
| 45 | `players` | `HashMap<u32, Player>` | pub | `std::unordered_map<uint32_t, Player>` | Other players, keyed by net_id. |
| 46 | `inventory` | `Inventory` | pub | `Inventory` | Updated on SendInventoryState. |
| 47 | `equipped_items` | `HashSet<u16>` | | `std::unordered_set<uint16_t>` | Cached active/equipped items. |
| 48 | `world` | `Option<World>` | pub | `std::optional<World>` | Current world; updated on SendMapData. |
| 49 | `peer_id` | `Option<enet::PeerID>` | | `std::optional<PeerId>` | Active peer; set on Connect, cleared on Disconnect. |
| 50 | `state` | `Arc<RwLock<BotState>>` | pub | `std::shared_ptr<RwLock<BotState>>` | Shared state written by bot, read by UI layer. |
| 51 | `cmd_rx` | `CmdReceiver` | | queue receiver | Commands from UI layer, executed each tick. |
| 52 | `temporary_data` | `TemporaryData` | pub | `TemporaryData` | One-shot dialog callback holder. |
| 53 | `auto_collect` | `bool` | pub | `bool` | Auto-collect nearby dropped items. |
| 54 | `auto_reconnect` | `bool` | pub | `bool` | Auto-reconnect after disconnect. |
| 55 | `collect_radius_tiles` | `u8` | | `uint8_t` | 1–5; pixel radius = tiles×32. Default `3`. |
| 56 | `collect_blacklist` | `HashSet<u16>` | | `std::unordered_set<uint16_t>` | Item IDs excluded from auto-collect. |
| 57 | `collect_timer` | `Instant` | | `steady_clock::time_point` | Last collect() run. |
| 58 | `astar` | `AStar` | | `AStar` | Pathfinder, reused across calls. |
| 59 | `pathfind_target` | `Option<(u32,u32)>` | | `std::optional<std::pair<uint32_t,uint32_t>>` | Current routing target tile. |
| 60 | `pathfind_recalc` | `bool` | | `bool` | Set on OnSetPos to force replan. |
| 61 | `delays` | `BotDelays` | pub | `BotDelays` | Configurable action/backoff delays. |
| 62 | `items_dat` | `Arc<ItemsDat>` | pub | `std::shared_ptr<ItemsDat>` | Item DB for collision lookups. |
| 63 | `event_tx` | `Option<crossbeam_channel::Sender<BotEventRaw>>` | | queue sender | Forwards events to script thread (None = no script). |
| 64 | `script_req_rx` | `Option<crossbeam_channel::Receiver<ScriptRequest>>` | | queue receiver | Requests from script thread. |
| 65 | `script_reply_tx` | `Option<crossbeam_channel::Sender<ScriptReply>>` | | queue sender | Replies to script thread. |
| 66 | `script_stop` | `Arc<AtomicBool>` | pub | `shared_ptr<atomic<bool>>` | Interrupts a running script. **In Nxrth there is NO Lua** — see §3. |
| 67 | `reconnect_after` | `Option<Instant>` | | `std::optional<steady_clock::time_point>` | Delay reconnect until this instant (cooldowns). |
| 68 | `pending_2fa` | `bool` | | `bool` | "Advanced Account Protection" seen → next logon_fail applies 2FA cooldown. |
| 69 | `pending_relogon` | `bool` | | `bool` | "Server requesting that you re-logon" seen. |
| 70 | `pending_server_overload` | `bool` | | `bool` | "SERVER OVERLOADED" seen. |
| 71 | `pending_too_many_logins` | `bool` | | `bool` | "Too many people logging in" seen. |
| 72 | `pending_login_throttle` | `bool` | | `bool` | "Please try again in N seconds" / "Fail to login" seen. |
| 73 | `pending_place_prepare` | `bool` | | `bool` | "Server couldn't prepare a place" seen. |
| 74 | `pending_update_required` | `bool` | | `bool` | "UPDATE REQUIRED" seen → stop bot on logon_fail. |
| 75 | `pending_maintenance` | `bool` | | `bool` | "undergoing maintenance" seen. |
| 76 | `stop_requested` | `bool` | | `bool` | Makes run loop exit next iteration. |
| 77 | `bot_id` | `u32` | pub | `uint32_t` | ID in BotManager; tags WS/UI events & log lines. |
| 78 | `ws_tx` | `Option<WsTx>` | | event sink | Broadcast sender for real-time UI events (None standalone). **Nxrth: Dear ImGui state sink, NO web/WebSocket.** |
| 79 | `last_ping` | `u32` | | `uint32_t` | Last broadcast ping; suppresses redundant ping events. |
| 80 | `geiger_green_repeat` | `Option<GeigerGreenRepeat>` | | `std::optional<GeigerGreenRepeat>` | See §1.6. |

### 1.8 Referenced external types (defined in other modules; needed to compile this one)

- `Socks5Config` (`bot/shared.rs`): `{ proxy_addr: SocketAddr, username: Option<String>, password: Option<String> }`. Method `to_url() -> String`: `socks5://user:pass@addr` if both creds present, else `socks5://addr`.
- `Credentials` (`bot/auth.rs`): `{ ltoken: String, meta: String, addr: SocketAddr, identity: Option<LoginIdentity>, bypass_enet: Option<Socks5Config> }`.
- `LoginIdentity` (`bot/auth.rs`): 17 `String` fields, in order: `game_version, cbits, player_age, gdpr, category, total_playtime, country, zf, rid, mac, wk, hash, hash2, fz, platform_id, steam_token, klv`.
- `TemporaryData` (`bot/shared.rs`): `{ dialog_callback: Mutex<Option<DialogCallback>> }`; `Default` = `None`.
- `LoginInfo` (`server_data.rs`): `{ protocol: u32, game_version: String }`.
- `RotatingLoginProxy` (`proxy_pool.rs`): supplies `fresh_url() -> String` (rotates an HTTP(S)/socks proxy URL per call).
- `BotState`/`BotStatus`/`BotDelays`/`CmdReceiver`/etc. — see the bot_state spec.

---

## 2. FUNCTIONS

### 2.1 Free helper functions

**`default_klv(rid: &str, hash: &str) -> String`**
1. Parse `hash` as `i32`; on parse failure fall back to `DEFAULT_HASH.parse::<i32>()`.
2. Return `compute_klv(GAME_VER, PROTOCOL.to_string(), rid, hash_val)`. (`compute_klv` is the crypto module: an MD5-based key derivation — see the crypto spec; use the bundled md5.)

**`value_or_default(value: String, default: &str) -> String`** — returns `default` if `value.trim()` is empty, else `value`. (Trim-empty check, not just empty.)

**`resolve_login_identity(identity: Option<LoginIdentity>) -> LoginIdentity`**
Fills every identity field with a default if blank. Defaults used:
- `game_version` → `GAME_VER`; `cbits` → `"1536"`; `player_age` → `"23"`; `gdpr` → `"2"`; `category` → `"_16"`; `total_playtime` → `"0"`; `country` → `"us"`; `zf` → `DEFAULT_ZF`; `rid` → `DEFAULT_RID`; `mac` → `DEFAULT_MAC`; `wk` → `DEFAULT_WK`; `hash` → `DEFAULT_HASH`; `hash2` → `DEFAULT_HASH2`; `fz` → `DEFAULT_FZ`; `platform_id` → `DEFAULT_PLATFORM_ID`; `steam_token` → `DEFAULT_STEAM_TOKEN`; `klv` → `default_klv(rid, hash)` (computed from the *resolved* rid/hash).
- Note: `rid` and `hash` are resolved first, then the `fallback_klv` is computed from them, then `klv` uses that fallback if the provided klv is blank.
- `None` branch: same defaults with a fresh `default_klv(DEFAULT_RID, DEFAULT_HASH)`.

**`sorted_blacklist_vec(set: &HashSet<u16>) -> Vec<u16>`** — copy to vec, `sort_unstable`, return (ascending). Used to publish blacklist to shared state deterministically.

**`now_millis() -> u64`** — wall-clock UNIX epoch millis, clamped to `u64::MAX`, `0` on error. (This is the ONE wall-clock use; all other timing is monotonic `Instant`.) C++: `std::chrono::system_clock` epoch ms.

**`reserve_gate_slot(gate, spacing_ms, max_ahead_ms) -> Instant`** — the heart of the stagger system. Atomically reserves this bot's slot and returns the instant it may proceed. The mutex is held ONLY for the reservation, never during the wait.
```
lock gate  (lazy-init to Instant::now() on first use; recover from poison by taking inner)
now      = Instant::now()
horizon  = now + max_ahead_ms
slot     = clamp(*next_allowed, now, horizon)          // never in the past, never past horizon
*next_allowed = min(slot + spacing_ms, horizon)         // advance the cursor, capped at horizon
unlock
return slot
```
CRITICAL invariant (bounded backlog): `next_allowed` is monotonic; `max_ahead_ms` caps how far ahead a slot may be handed out AND pins `next_allowed` at that horizon so the backlog can never exceed `max_ahead_ms`. Under light load, full `spacing_ms` is preserved; under saturation it degrades to "everyone within `max_ahead_ms`, slightly bursty" rather than pushing waits into minutes. For connected-phase gates this prevents holding a live ENet connection open past the server login timeout.

**`wait_global_gate(gate, spacing_ms) -> u64`** — DEAD-SLEEP variant for the pre-connection HTTP-login stagger (no live ENet host yet). Reserves with `GATE_HTTP_MAX_AHEAD_MS`, then if `slot > now` sleeps `slot - now` and returns the waited ms; else returns `0`. Does NOT hold the lock during sleep.

**`pace_dashboard()`** (`pub(crate)`) — `wait_global_gate(&DASHBOARD_GATE, DASHBOARD_STAGGER_MS)`, discard result. Call right before any `/player/login/dashboard` POST, from any module.

**`pace_http_login()`** (`pub(crate)`) — `wait_global_gate(&HTTP_LOGIN_GATE, HTTP_LOGIN_STAGGER_MS)`. Call before a full HTTP re-login on reconnect/refresh paths so bursts don't bypass the stagger.

**`reserve_throttle_slot(gate, cooldown_ms, spacing_ms) -> Instant`** — reserve a fleet reconnect slot after a gateway throttle:
```
lock gate
now     = Instant::now()
floor   = now + cooldown_ms          // shared bypass IP must cool off
horizon = floor + 60s                // cap the single-file backlog
base    = clamp(*next_allowed, floor, horizon)
*next_allowed = min(base + spacing_ms, horizon)
unlock
return base
```

**`is_http_403_text(message: &str) -> bool`** — lowercase, true if contains `"403"` or `"forbidden"`.

**Logging/formatting helpers** (port as utility functions; behavior lightly load-bearing):
- `is_high_frequency_noise(msg)` — true if the `[Bot...]`-stripped body starts with `"GameUpdatePacket"`, `"CallFunction: OnClearItemTransforms"`, or `"PingReply sent"`. These are file-log-only (kept out of the in-memory console + event feed to avoid RAM blowup).
- `bot_msg_body(msg)` — strips a leading `[Bot` … `] ` tag; returns the body (or whole msg if untagged).
- `truncate_text(value, max_chars)` — if char count ≤ max, return as-is; else take `max_chars` chars + `"\n...<truncated>"`.
- `summarize_detail(detail)` — first non-blank line, trimmed, truncated to 140 chars.
- `redact_packet_text(text)` — per line, if `key|value` and the lowercased trimmed key ∈ {`token`, `ltoken`, `ubiticket`, `tankidpass`, `password`, `steamtok`, `steamtoken`, `fcmtoken`}, replace with `key|<redacted>`. Apply to ALL logged/emitted packet text.
- `hex_preview(bytes, max_len)`, `format_variant_value`, `format_variant_list`, `format_game_packet_fields`, `format_game_packet_detail` — diagnostic formatting for the traffic feed; port only if you keep the traffic inspector UI.

### 2.2 `BotHost` methods

- **`next_event() -> Option<EventNoRef>`**: call `host.service()`. On `Ok(Some(e))` return the event (`e.no_ref()`); on `Ok(None)` OR **`Err(_)` return `None`** (skip the tick). CRITICAL: a socket-level error from `enet_host_service` (e.g. transient Windows `WSAECONNRESET` after a SOCKS5 relay ICMP "port unreachable") must NOT crash the bot thread — swallow it. A truly dead relay is still caught by ENet's own timeout → clean Disconnect event.
- **`connect(addr, channels, data)`**: `host.connect(addr, channels, data)`; the Rust `.expect("connect failed")` panics on failure — in Nxrth, treat a connect error as a failed attempt (log + let the run loop retry), do **not** abort the process.
- **`peer_rtt(id) -> Duration`**: `host.peer_mut(id).round_trip_time()`.
- **`peer_send(id, channel, packet)`**: `host.peer_mut(id).send(channel, packet)`, ignore the Result (`.ok()`).
- **`peer_disconnect(id, data)`**: `host.peer_mut(id).disconnect(data)`.
- **`peer_set_timeout(id, limit, minimum, maximum)`**: `host.peer_mut(id).set_timeout(limit, minimum, maximum)`. Used to make the peer tolerant of flaky game-proxy UDP relays (ENet default minimum is 5 s, too aggressive).

### 2.3 `Bot::create_host(proxy: Option<&Socks5Config>) -> BotHost`  ← SOCKS5 UDP-bind-fail SAFE FALLBACK (critical)

ENet `HostSettings` used for every host: `peer_limit = 1`, `channel_limit = 2`, `compressor = RangeCoder`, `checksum = crc32`, `using_new_packet = true`, rest default.

1. **If `proxy` is `Some(p)`:**
   - Log `"[Bot] Connecting via proxy {p.to_url()}"`.
   - `local_addr = "0.0.0.0:0"`.
   - Retry loop **`for attempt in 0..4`** (i.e. up to 4 attempts): call `Socks5UdpSocket::bind_through_proxy(local_addr, p.proxy_addr, username, password)`. On success, build an ENet host on that socket → return `BotHost::Socks5(host)`. On failure, if `attempt < 3` sleep **300 ms** and retry. (A SOCKS5 UDP-ASSOCIATE can transiently fail: proxy busy / port renegotiating.)
   - **If all 4 attempts fail:** do **NOT** connect direct (that leaks the operator's REAL IP to Growtopia and gets it flagged). Instead log `"[Bot] Proxy UDP bind failed after retries — NOT falling back to direct (would leak real IP); will retry with a fresh proxy"` and bind a **dead loopback socket** `127.0.0.1:0` as a `BotHost::Direct`. The subsequent connect simply fails and the run loop retries with a fresh proxy port next cycle. **This is the SOCKS5 UDP-bind-fail safe fallback — replicate it exactly. Never fall through to a real direct connection when a proxy was configured.**
2. **If `proxy` is `None`:** bind `0.0.0.0:0` UDP directly → `BotHost::Direct(host)`.

C++: build the ENet host with the vendored/patched ENet (SOCKS5-UDP support). The 4-try/300 ms retry and the "dead loopback on give-up" behavior are load-bearing anti-IP-leak safeguards.

### 2.4 Constructors

All five public constructors share the same tail: build a `log_fn` closure (writes to `logger::log`, pushes to `state.console` capped at **100** lines dropping the oldest, and emits a `WsEvent::Console{bot_id, message}`), stagger the HTTP login via `wait_global_gate(&HTTP_LOGIN_GATE, HTTP_LOGIN_STAGGER_MS)` (logging `"[Bot] staggering HTTP login by {ms} ms (avoids simultaneous-login throttle)"` if >0), fetch credentials, then build the `Bot` and call `bot.host.connect(addr, 2, 0)`. All return `Option<Bot>` (Nxrth: `std::optional<Bot>` or nullable) — a failed spawn returns `None`/null and must NOT panic the worker thread.

- **`new(username, password, proxy, login_proxy, stop, state, cmd_rx, items_dat, bot_id, ws_tx) -> Option<Bot>`**: HTTP-stagger → `fetch_credentials(...)` (returns `None` → propagate `None`) → `new_with_credentials(username, LoginMethod::Legacy{password}, creds, ...)`.
- **`new_requestly(...)`**: identical but `fetch_requestly_credentials` + `LoginMethod::Requestly{password}`.
- **`new_newly(...)`**: identical but `fetch_newly_credentials` + `LoginMethod::Newly{password}`.
- **`new_with_credentials(username, login_method, creds, proxy, login_proxy, stop, state, cmd_rx, items_dat, bot_id, ws_tx) -> Bot`** (private):
  - Destructure `creds` → `ltoken, meta, addr, identity, bypass_enet`.
  - `resolve_login_identity(identity)` → all 17 identity strings.
  - **`host = create_host(bypass_enet.or(proxy))`** — the initial ENet logon must leave from the SAME IP that minted the ltoken (pinned bypass IP); world/subserver traffic later switches to the game proxy.
  - Construct `Bot` with all fields (defaults listed in §2.5).
  - Write to shared state: `s.username`, `s.mac`, `s.collect_radius_tiles`, `s.collect_blacklist = sorted_blacklist_vec(...)`.
  - `bot.host.connect(addr, 2, 0)`; return bot.
- **`new_ltoken(ltoken_str, proxy, login_proxy, stop, state, cmd_rx, items_dat, bot_id, ws_tx) -> Option<Bot>`**:
  - `parse_ltoken_string(ltoken_str)` (see below); invalid positional or keyed input is rejected before the bot starts.
  - `rid/mac/wk` = parsed value or default (`value_or_default`). All other identity fields set to hardcoded defaults (`hash=DEFAULT_HASH`, `hash2=DEFAULT_HASH2`, `fz=DEFAULT_FZ`, `game_version=GAME_VER`, `cbits="1536"`, `player_age="23"`, `gdpr="2"`, `category="_16"`, `total_playtime="0"`, `country="us"`, `zf=DEFAULT_ZF`, `platform_id=DEFAULT_PLATFORM_ID`, `steam_token=DEFAULT_STEAM_TOKEN`, `klv=default_klv(rid,hash)`).
  - HTTP-stagger, then **server_data fetch loop** (see §2.6). `meta` from the response.
  - Parse `"{server}:{port}"` → `SocketAddr`; on error log `"[Bot] invalid server address ... — not spawning bot"` and return `None`.
  - `host = create_host(proxy)` (no bypass pinning for ltoken), `login_method = Ltoken`, `username = ""`, `bypass_enet = None`. Write `s.mac`/`s.collect_radius_tiles`/`s.collect_blacklist`, `connect(addr,2,0)`, return.
- **`new_har_token(har_path, proxy, login_proxy, stop, state, cmd_rx, items_dat, bot_id, ws_tx) -> Option<Bot>`**:
  - `har_parser::extract_auth_data(har_path)` → on `Ok` log `"[Bot] HAR auth data extracted successfully"`; on `Err(e)` log `"[Bot] HAR payload extraction failed: {e} — not starting bot"` and return `None`.
  - Fields from the HAR data with `value_or_default` fallbacks: `ltoken=data.token`, `rid/mac/wk/hash/hash2/fz/platform_id/steam_token` from data (defaulted if blank), `klv = data.klv if non-blank else default_klv(rid,hash)`. Remaining identity fields hardcoded as in `new_ltoken`.
  - HTTP-stagger, server_data fetch loop, `meta` from response, parse addr (same error handling → `None`).
  - `host = create_host(proxy)`, `login_method = HarToken{har_path}`, `bypass_enet=None`, `username=""`. `connect(addr,2,0)`, return. (Note: `new_har_token` does NOT write mac/collect state to shared state before connecting — a minor divergence; keep or normalize, but match if bit-exact behavior matters.)

**`parse_ltoken_string(s) -> Option<LtokenRecord>`**: accepts strict positional
`refreshToken|rid|mac|wk` records and provider-keyed `key:value|...` records.
Positional and keyed `refreshToken:` inputs use `checktoken`; keyed provider
`token:` inputs are gateway tokens and skip that exchange.
Keyed fields are order-independent, unknown fields are tolerated, and required
fields are `token`, 32-character `rid`, non-empty `mac`, and either a
32-character `wk` or `NONE0`. Optional `platform`, `name`, `cbits`,
`playerAge`, and `vid` metadata is parsed when valid.

**`login_token_field(token: &str) -> &'static str`**: if the token contains ≥ 3 dot-separated segments (JWT-shaped) return `"UbiTicket"`, else `"token"`. Selects which field the login packet uses (see §2.9).

### 2.5 Default field values at construction (all constructors)

`redirect=None`, `redirect_attempts=0`, `last_redirect_token=None`, `last_redirect_uuid=None`, `refresh_token_on_reconnect=false`, `server_addr=Some(addr)`, `saw_server_hello=false`, `connected_since=None`, `was_in_world=false`, `redirect_connect_fails=0`, `pre_hello_disconnects=0`, `login_reject_streak=0`, `login_throttle_streak=0`, `in_gate_wait=false`, `peer_id=None`, `pos_x=0.0`, `pos_y=0.0`, `start_time=now`, `local=LocalPlayer::default()`, `players={}`, `inventory=Inventory::default()`, `equipped_items={}`, `world=None`, `temporary_data=default`, `auto_collect=true`, `auto_reconnect=true`, `collect_radius_tiles=3`, `collect_blacklist={}`, `collect_timer=now`, `astar=AStar::new()`, `pathfind_target=None`, `pathfind_recalc=false`, `delays=BotDelays::default()`, `event_tx=None`, `script_req_rx=None`, `script_reply_tx=None`, `script_stop=Arc::new(false)`, `reconnect_after=None`, all `pending_*=false`, `stop_requested=false`, `last_ping=0`, `geiger_green_repeat=None`.

### 2.6 server_data fetch loop (used by `new_ltoken`, `new_har_token`, `reconnect_main`)

`login_info = { protocol: PROTOCOL, game_version: GAME_VER }`. Then loop:
1. If `stop` is set → log `"[Bot] login aborted — bot was stopped"` and return `None`.
2. `rotating = login_proxy.map(fresh_url())`; `proxy_url = login_http_proxy_url(proxy, rotating)`.
3. Log `"[Bot] fetching server_data (alternate={alternate})..."`.
4. `get_server_data_proxied(alternate, &login_info, proxy_url)`: `Ok(s)` → break with `s`; `Err(e)` → flip `alternate`, log `"[Bot] fetch: server_data failed: {e} — retrying in 5s"`, sleep **5 s**, retry.

`alternate` toggles the growtopia vs growtopia2 host. `get_server_data_proxied` is the server_data module (libcurl in Nxrth).

### 2.7 `run(&mut self, stop_flag: Arc<AtomicBool>)` — the main loop

One iteration:
1. If `stop_flag` set → log `"[Bot] Stop flag set, exiting."` break.
2. If `stop_requested` → log `"[Bot] Stop requested internally, exiting."` break.
3. If `reconnect_after` is `Some(at)` and `now >= at`: clear `reconnect_after`; if `auto_reconnect`, take `refresh = refresh_token_on_reconnect` (clear it), log `"[Bot] Reconnect cooldown elapsed — reconnecting with current session"`, call `reconnect_main(refresh)`.
4. Drain all pending commands: `while cmd_rx.try_recv() -> handle_command(cmd)`.
5. If `peer_id` present: `rtt = peer_rtt(id).as_millis() as u32`; write `state.ping_ms = rtt`; if `rtt != last_ping` update `last_ping` and emit `WsEvent::BotPing{bot_id, ping_ms}`.
6. `service_once()` (§2.8).
7. **Login watchdog**: if `connected_since = Some(since)` and `world.is_none()` and `since.elapsed() >= 30s`:
   - Log `"[Bot] login stalled — 30s connected with no world (flaky game proxy?); dropping to retry via gateway"`.
   - Rotate game proxy: `proxy_pool::next_game_proxy(self.proxy)` → if `Some(fresh)` log `"[Bot] rotating game proxy after stall → {fresh.proxy_addr}"`, `self.proxy = Some(fresh)`.
   - `connected_since=None`, `redirect=None`, `redirect_attempts=0`, `redirect_connect_fails=0`, `saw_server_hello=false` (so the resulting Disconnect routes to a clean gateway reconnect), and if `peer_id` present, `peer_disconnect(id, 0)`.
8. `drain_script_requests()` — **Nxrth: replace with native automation dispatch, NO Lua** (§3).
9. If `auto_collect` and `collect_timer.elapsed() >= 500ms`: reset `collect_timer`, call `collect()`.
10. Sleep **10 ms**.

After the loop exits: `shutdown()`.

**`shutdown(&mut self)`**: set `script_stop=true`; drop `script_req_rx/script_reply_tx/event_tx`; if `peer_id` present, `peer_disconnect(id,0)` then service the host 5 times with 10 ms sleeps (to flush the disconnect).

**`sleep_ms(&mut self, ms)`**: busy-until `now+ms`, calling `service_once()` + sleeping 10 ms each iteration (keeps ENet alive during a sleep).

### 2.8 `service_once(&mut self)` — process all pending ENet events

`while let Some(event) = host.next_event()`:

**Connect { peer: id }:**
- `peer_id=Some(id)`, `saw_server_hello=false`, `connected_since=Some(now)`.
- `peer_set_timeout(id, 0, 12_000, 30_000)` — limit=0 keeps ENet default (32); minimum raised from 5 s → **12 s** so a short UDP relay gap doesn't drop an in-world bot; maximum **30 s** so a sustained outage still times out cleanly.
- Log `"[Bot] Connected: peer {id.0}"`.

**Disconnect { peer: id, data }:**
- `peer_id=None`, `connected_since=None`, `pathfind_target=None`, `pathfind_recalc=false`.
- `data` = ENet disconnect reason code: 0 = local/transport timeout (flaky proxy / peer vanished); non-zero = server-supplied code. Log `"[Bot] Disconnected: peer {id.0} (reason code {data})"`.
- `disconnected_before_server_hello = redirect.is_none() && !saw_server_hello`.
- Write shared state: `status=Connecting`, `world_name=""`, `players=[]`, `ping_ms=0`. Emit `BotStatus{"connecting"}` and `BotWorld{world_name=""}`.
- **Branching (in order):**
  - **(A) `redirect = Some(r)`:**
    - If `redirect_attempts > 0`: log `"[Bot] Redirect token was already used {n} time(s) — waiting for a fresh redirect"`, clear `redirect`+`redirect_attempts`, and if `auto_reconnect` call `reconnect_main(false)`.
    - Else (attempts==0 → subserver ServerHello not yet reached): parse `"{r.server}:{r.port}"`:
      - `Ok(addr)`: `redirect_connect_fails += 1`. If `>= REDIRECT_MAX_GAME_PROXY_TRIES (6)`: log `"[Bot] redirect: {n} game proxies failed to reach subserver {server}:{port} — abandoning redirect, full re-login"`, clear redirect/attempts/fails, if `auto_reconnect` → `reconnect_main(true)`. Else: if `redirect_connect_fails >= 2`, rotate game proxy via `next_game_proxy` (log `"[Bot] redirect: subserver unreachable via current game proxy — rotating to {addr}"`, set `self.proxy`); then log `"[Bot] Redirecting to {server}:{port}"`, `host = create_host(self.proxy)`, `saw_server_hello=false`, `host.connect(addr,2,0)`.
      - `Err(e)`: log `"[Bot] Invalid redirect address '{server}:{port}' ({e}) — dropping redirect, reconnecting to gateway"`, clear redirect/attempts/fails, if `auto_reconnect` → `reconnect_main(false)`.
  - **(B) else if `reconnect_after.is_some()`:** do nothing (a delayed reconnect is already scheduled, e.g. 2FA cooldown).
  - **(C) else if `auto_reconnect`:** `refresh_token = refresh_token_on_reconnect` (clear it), then:
    - **(C1) `disconnected_before_server_hello`:** `pre_hello_disconnects += 1`; log `"[Bot] disconnected before ServerHello ({n}/3)"`. If `>= 3`: log `"[Bot] no ServerHello after retries - forcing fresh login"`, set `refresh_token=true`, reset `pre_hello_disconnects=0`. Log `"[Bot] Server disconnected — reconnecting with current session"`, then `schedule_reconnect("Server disconnected before ServerHello", refresh_token, 1_500)`.
    - **(C2) else if `was_in_world`:** an authenticated in-world session dropped — almost always a flaky game/world proxy, NOT a logon rejection. Recover like a manual re-add: set `was_in_world=false`, `pre_hello_disconnects=0`, log `"[Bot] in-world session dropped (game proxy?) — restarting login from scratch on a fresh exit IP + token"`, then `schedule_reconnect("In-world session dropped", true, 1_500)` (force refresh). Do NOT touch `login_reject_streak` — this is not a rate-limit event.
    - **(C3) else (reached ServerHello + sent `225`, gateway dropped with no redirect = silent rejection, exit IP rate-limited/flagged):** `pre_hello_disconnects=0`; `login_reject_streak += 1`. If `refresh_token || login_reject_streak == 1`: log `"[Bot] Server disconnected — reconnecting with current session"`, `schedule_reconnect("Server disconnected after ServerHello", refresh_token, 1_500)` (prompt retry once). Else: `secs = clamp(15 * min(login_reject_streak, 8), 15, 120)`; `reconnect_after = now + secs`; if `login_reject_streak % 3 == 0` set `refresh_token_on_reconnect=true`; log `"[Bot] logon rejected by gateway ({n}x, ServerHello but no redirect) — exit IP likely rate-limited; backing off {secs}s before retry"`.
  - **(D) else:** log `"[Bot] Server disconnected — auto-reconnect is disabled"`.

**Receive { peer: id, channel_id, packet }:** `packet_size = packet.data().len()`. `IncomingPacket::parse(packet.data())`:
- `ServerHello` → emit traffic `("in","server_hello",size,"ServerHello")`, call `on_server_hello()` (§2.9).
- `Text(s)` → emit traffic `("in","text",size,redact(s))`, log `"[Bot] Text: {s}"`.
- `GameMessage(s)` → emit traffic, log `"[Bot] GameMessage: {s}"`, forward to script channel if present, then substring-scan `s` (see §2.10). Sets the `pending_*` flags and handles `action|request_token`, `action|logon_fail`, etc.
- `GameUpdate(pkt)` → emit traffic, forward to script channel, `update_geiger_signal(&pkt)`, then `match pkt.packet_type` (SetCharacterState, CallFunction → `on_call_function`, PingRequest → `on_ping_request`, SendInventoryState → `Inventory::parse`, SendMapData → `World::parse`, …). These are handled in later specs; carry the geiger + call-function dispatch as-is.

### 2.9 `on_server_hello(&mut self)` — login-packet gate

1. `saw_server_hello=true`, `pre_hello_disconnects=0`, `redirect_connect_fails=0` (reaching a server proves this leg's proxy works).
2. `data = match redirect.take()`:
   - `Some(r)`: `redirect_attempts += 1`, log `"[Bot] ServerHello (redirect → {r.door_id})"`, `data = build_redirect_packet(&r)`.
   - `None`: log `"[Bot] ServerHello"`, then **`wait_for_global_gate(&LOGIN_PACKET_GATE, LOGIN_PACKET_STAGGER_MS, "login packet")`** (fleet stagger while keeping ENet serviced), `data = build_login_packet()`.
3. `println!("\n=== RAW LOGIN PACKET ===\n{}\n========================", redact(data))` (stdout diagnostic).
4. `send_text(&data)`.

**`wait_for_global_gate(gate, spacing_ms, label)`** — connected-phase gate wait that KEEPS ENET SERVICED:
1. `slot = reserve_gate_slot(gate, spacing_ms, GATE_CONNECTED_MAX_AHEAD_MS)`. If `slot <= now` return immediately.
2. `waited_ms = slot - now`; if `>= 50` log `"[Bot] login pacing: waiting ~{waited_ms}ms before {label} (keeping ENet serviced)"`.
3. If already `in_gate_wait` (re-entrant) → plain `sleep(waited_ms)` and return (no nested servicing).
4. Else set `in_gate_wait=true`; loop while `now < slot`: if stop → break; else `service_once()`; sleep 10 ms. Then `in_gate_wait=false`.
   - CRITICAL: this gate is entered while CONNECTED (before sending login/enter_game/warp). A dead sleep would let the server time the ENet connection out ("disconnected before ServerHello") under fleet load — hence servicing the host during the wait. This is the single most important scaling fix; replicate exactly.

Enter-game and warp use the same helper (see §2.11, §2.12).

### 2.10 GameMessage substring handlers + `logon_fail` dispatch (throttle logic)

When a `GameMessage(s)` arrives, do these substring checks (order as written):
- `"action|request_token"` → log `"[Bot] Server requested a fresh login token - fetching now."`, clear `redirect`/`redirect_attempts`/`refresh_token_on_reconnect`/`reconnect_after`, call `reconnect_main(true)`, and `continue` (skip rest of this event).
- `"Advanced Account Protection"` → `pending_2fa=true`.
- `"action|log"` && `"SERVER OVERLOADED"` → `pending_server_overload=true`.
- `"action|log"` && `"Too many people logging in"` → `pending_too_many_logins=true`.
- `"action|log"` && (`"Please try again in"` || `"Fail to login"`) → **`pending_login_throttle=true`**.
- `"Server couldn't prepare a place"` → `pending_place_prepare=true`.
- `"action|log"` && `"Server requesting that you re-logon"` → log, clear `redirect`, `pending_relogon=true`.
- `"action|log"` && `"UPDATE REQUIRED"` → `pending_update_required=true`.
- `"action|log"` && `"undergoing maintenance"` → `pending_maintenance=true`.
- `"action|logon_fail"` → dispatch on the FIRST matching pending flag (each clears its own flag). After dispatch, always `peer_disconnect(id, 0)`:
  1. `pending_2fa` → `secs=delays.twofa_secs`; log; `state.status=TwoFactorAuth`; `reconnect_after=now+secs`; emit `BotStatus{"two_factor_auth"}`.
  2. `pending_server_overload` → `secs=delays.server_overload_secs + (bot_id % 7)`; log; `status=ServerOverloaded`; `reconnect_after`; emit.
  3. `pending_too_many_logins` → `secs=delays.too_many_logins_secs + (bot_id % 5)`; log; `status=TooManyLogins`; `reconnect_after`; emit.
  4. **`pending_login_throttle` (THE "Fail to login 30s" ROTATE LOGIC):**
     - Clear flag; `login_throttle_streak += 1`.
     - `slot = reserve_throttle_slot(&GATEWAY_LOGON_GATE, GATEWAY_THROTTLE_COOLDOWN_MS (32_000), GATEWAY_LOGON_SPACING_MS (1500))`; `secs = slot - now` (as secs).
     - `streak = login_throttle_streak`; `rotate = login_proxy.is_some()`.
     - If `rotate`: `refresh_token_on_reconnect=true`, `login_throttle_streak=0`, log `"[Bot] Logon failed — 'Fail to login, try again in 30s' ({streak}x): restarting login from scratch on a FRESH exit IP + token, fleet retry in ~{secs} s."` — i.e. on ANY "Fail to login 30s" with a rotating login proxy configured, restart the WHOLE login from scratch (server_data → dashboard → fresh token on a fresh exit IP → gateway logon), exactly like a manual first-time login. Safe because the fetch is fleet-paced and the round-robin pool hands out a DIFFERENT exit port each attempt. (The ltoken is IP-bound; reusing the same throttled exit IP loops forever — this rotates out of it.)
     - Else (no rotating login proxy): log `"[Bot] Logon failed — 'Fail to login, try again in 30s' ({streak}x); reusing token (no rotating login proxy configured), fleet retry in ~{secs} s."` (nothing to rotate to; reuse token and wait out the ~30 s IP cooldown).
     - `state.status=TooManyLogins`; `reconnect_after=Some(slot)`; emit `BotStatus{"too_many_logins"}`.
  5. `pending_place_prepare` → `secs=delays.server_overload_secs + (bot_id % 9)`; log; `status=ServerOverloaded`; `reconnect_after`; emit.
  6. `pending_relogon` → clear; log `"[Bot] Logon failed — server requested re-logon. Reconnecting."` (no cooldown set — reconnect happens via the normal Disconnect path after `peer_disconnect`).
  7. `pending_update_required` → clear; log `"[Bot] Logon failed — client update required. Stopping bot."`; `status=UpdateRequired`; emit `BotStatus{"update_required"}`; `stop_requested=true`.
  8. `pending_maintenance` → clear; `secs=delays.maintenance_secs`; log; `status=Maintenance`; `reconnect_after`; emit `BotStatus{"maintenance"}`.
  9. **else (no pending flag):** log `"[Bot] Logon failed — clearing redirect and reconnecting"`; clear `redirect`/`redirect_attempts`; `refresh_token_on_reconnect=true`.

### 2.11 `on_call_function` → `OnSuperMainStartAcceptLogon…` (enter-game gate) and `OnSendToServer` / `OnSpawn`

Handled inside `on_call_function` (dispatch on VariantList[0] name string):
- **`"OnSuperMainStartAcceptLogonHrdxs47254722215a"`** (exact magic string): `state.status=Connected`; **`wait_for_global_gate(&ENTER_GAME_GATE, ENTER_GAME_STAGGER_MS, "enter_game")`**; `send_text("action|enter_game\n")`; emit `BotStatus{"connected"}`.
- **`"OnSendToServer"`** (subserver redirect): parse VariantList slots →
  - `[1]` = port (u16, default 0), `[2]` = raw_token (string, default `"0"`), `[3]` = user_id (string, default `"0"`), `[4]` = server_str, `[5]` = lmode (i32, default 0), `[6]` = tank_id_name.
  - Split `server_str` on `|` into ≤3 parts: `server = parts[0].trim_end()`, `door_id = parts[1].trim_end()` (if empty → `"0"`), `raw_uuid = parts[2].trim_end()`.
  - **Token marker handling:** parse `raw_token` as i64; if `< 0` (a `-1`-style marker): use `last_redirect_token` if cached (log "using cached redirect token for marker…"), else keep `raw_token` (log "no cached redirect token…"). Otherwise: if non-empty, cache into `last_redirect_token`; use `raw_token`.
  - **UUID marker handling:** if `raw_uuid` empty or `== "-1"`: use `last_redirect_uuid` if cached, else keep raw. Otherwise cache into `last_redirect_uuid`, use raw.
  - Log `"[Bot] OnSendToServer → {server}:{port} door={door_id}"`.
  - `redirect = Some(RedirectData{server, port, token, user:user_id, door_id, uuid, lmode:lmode.to_string(), tank_id_name})`; `redirect_attempts=0`; `login_reject_streak=0`; `login_throttle_streak=0` (gateway accepted → exit IP is fine); `peer_disconnect(id,0)` (drop to reconnect to the subserver).
- **`"OnSpawn"`** with a `"type"` key (local player) → sets `local.net_id`/`local.user_id`, clears `redirect`/`redirect_attempts`/`refresh_token_on_reconnect`, calls `clear_login_state_flags()`, `connected_since=None`, **`was_in_world=true`** (a later drop = in-world session loss), logs OnSpawn + the ltoken string, `state.status=InGame`, emit `BotStatus{"in_game"}`. Without a `"type"` key → other player spawn (adds to `players`, emits `PlayerSpawn`).

**`clear_login_state_flags(&mut self)`**: clears all 8 `pending_*` flags and resets `login_reject_streak=0`, `login_throttle_streak=0`, `pre_hello_disconnects=0`. Called once fully in-world so substring-set flags don't leak into a future session.

### 2.12 `warp(&mut self, name: &str, id: &str)` — warp gate

1. **`wait_for_global_gate(&WARP_GATE, WARP_STAGGER_MS, "warp")`** — serialize warps fleet-wide (a bulk warp otherwise fires many simultaneous subserver redirect handshakes; the burst gets partially dropped).
2. `redirect=None`, `redirect_attempts=0`.
3. `send_game_message("action|join_request\nname|{name}|{id}\ninvitedWorld|0\n")`.

(Related nearby helpers: `say(text)` → `send_text("action|input\n|text|{text}\n")`; `leave_world()` → `send_game_message("action|quit_to_exit\n")`; `respawn()` → `send_text("action|respawn\n")`.)

### 2.13 `reconnect_main(&mut self, refresh_token: bool)`

Always reconnects to the gateway (`server_addr`) and re-sends the login packet, so it must leave from the pinned bypass IP.
1. `host = create_host(bypass_enet.or(proxy))`; `peer_id=None`; `saw_server_hello=false`.
2. **If `refresh_token`:** `server_addr=None`; call `refresh_token()` (§2.14). If it repopulated `server_addr`: `host = create_host(bypass_enet.or(proxy))` again (refresh may have re-pinned `bypass_enet`), `saw_server_hello=false`, log `"[Bot] reconnect: connecting to refreshed {addr}"`, `host.connect(addr,2,0)`, return.
3. **Else:** log `"[Bot] reconnect: reusing current login token"`; if `server_addr` present: `saw_server_hello=false`, log `"[Bot] reconnect: connecting to current {addr}"`, `host.connect(addr,2,0)`, return.
4. **Terminal-stop guard:** if `stop_requested || stop` set → log `"[Bot] reconnect aborted — bot is stopping (no server address)"` and return. (`refresh_token()` sets `stop_requested` for terminal cases; without this guard the bot would spin in the fetch loop forever.)
5. Otherwise (no server_addr yet), do a full server_data re-fetch:
   - Build `proxy_candidates: Vec<(label, Option<url>)>`:
     - If `login_proxy` present: `url = login_proxy.fresh_url()`. If `url` starts with `http://`/`https://`, also compute `alternate_scheme_url(url)`. Push `("rotating login proxy", Some(url))` and, if an alt exists, `("rotating login proxy (alt scheme)", Some(alt))`. Log `"[Bot] reconnect: rotating login proxy enabled; assigned game proxy fallback disabled"`. (A `socks5://` login proxy has no HTTP-scheme fallback.)
     - Else if `proxy` present: push `("assigned game proxy", Some(proxy.to_url()))`.
     - If still empty: push `("direct", None)`.
   - `pace_http_login()` (fleet-space this fetch).
   - Loop: honor stop (log `"[Bot] reconnect aborted — bot stopped"`, return). For each `(label, url)` candidate, for `alternate in [false, true]`: log `"[Bot] reconnect: fetching server_data (alternate={alternate}, http_proxy={label})..."`; `get_server_data_proxied(...)`: `Ok(s)` → break out with `s`; `Err(e)` → log `"[Bot] reconnect: server_data failed via {label}: {e}"`; if `is_http_403_text(e)` → log `"[Bot] reconnect: proxy returned 403; skipping growtopia2 alternate for {label}"` and `break` (skip the alternate for this candidate). After all candidates fail: log `"[Bot] reconnect: all server_data proxy candidates failed - retrying in 5s"`, sleep 5 s, repeat.
   - On success: `meta = server_data.meta`. Parse `"{server}:{port}"`: `Ok(addr)` → `server_addr=Some(addr)`, `saw_server_hello=false`, log `"[Bot] reconnect: connecting to fetched {addr}"`, `host.connect(addr,2,0)`. `Err(e)` → log `"[Bot] reconnect: invalid server address '{server}:{port}' ({e}) — retrying in 10s"`, `reconnect_after=now+10s` (retry later, do not crash).

### 2.14 `refresh_token(&mut self)`

Refreshes `self.ltoken`. NOTE: the `check_token` fast-path is fully commented out ("disabled entirely per user request") — do NOT reintroduce it; go straight to the login-method fallback:
- **`Ltoken`** → log `"[Bot] ltoken login — no fallback credentials, stopping bot"`; `stop_requested=true`.
- **`HarToken { har_path }`** → log `"[Bot] HAR token refresh failed — re-extracting from HAR file"`; `har_parser::extract_auth_data(har_path)`: `Ok(data)` → `ltoken=data.token`; `Err(e)` → log `"[Bot] HAR payload extraction failed: {e}"` and return (leave token as-is).
- **`Legacy { password }`** → log `"[Bot] falling back to full re-login"`; build a `log_fn` (same shape as constructors); `pace_http_login()`; `fetch_credentials(username, password, proxy, login_proxy, stop, log_fn)`: `Some(creds)` → `apply_credentials(creds)`; `None` → `stop_requested=true`.
- **`Newly { password }`** → log `"[Bot] falling back to newly re-login"`; same, using `fetch_newly_credentials`.
- **`Requestly { password }`** → log `"[Bot] falling back to Requestly HAR re-login"`; same, using `fetch_requestly_credentials`.

**`apply_credentials(&mut self, creds)`**: `ltoken=creds.ltoken`, `meta=creds.meta`, **`bypass_enet=creds.bypass_enet`** (re-pin logon IP so the re-login's ENet logon leaves from the new token's IP), `server_addr=Some(creds.addr)`, clear `redirect`/`redirect_attempts`/`last_redirect_token`/`last_redirect_uuid`; if `creds.identity` present → `apply_login_identity(identity)`.

**`apply_login_identity(&mut self, identity)`**: run `resolve_login_identity(Some(identity))`, then assign all 17 identity strings onto `self`; also write `state.mac = self.mac`.

### 2.15 `schedule_reconnect(&mut self, reason, refresh_token: bool, base_ms: u64)`

Escalating backoff with jitter:
1. `streak = min(max(login_reject_streak, pre_hello_disconnects as u32), 8)`.
2. `backoff = base_ms * max(streak, 1)`.
3. `jitter = ((bot_id * 137) + (streak * 251)) % 1_000`.
4. `delay_ms = min(backoff + jitter, 30_000)`.
5. `refresh_token_on_reconnect |= refresh_token`.
6. `reconnect_after = now + delay_ms`.
7. Log `"[Bot] {reason} - reconnecting in {delay_ms}ms"`.

### 2.16 Packet send helpers (wire framing)

All three prepend a **4-byte little-endian message-type u32** then the payload, and send reliably on channel 0 (game packet honors a `reliable` flag). Message types (from `protocol/packet.rs`): `MSG_SERVER_HELLO=1`, `MSG_TEXT=2`, `MSG_GAME_MESSAGE=3`, `MSG_GAME_PACKET=4`. `GAME_PACKET_SIZE=56` bytes.
- **`send_text(text)`**: if `peer_id` → `raw = [2u32 LE][text bytes][0x00]` (`make_text_packet`), emit traffic `("out","text",…,redact(text))`, `peer_send(id, 0, Packet::reliable(raw))`.
- **`send_game_message(text)`**: same with `[3u32 LE]` (`make_game_message_packet`).
- **`send_game_packet(pkt, reliable)`**: `raw = [4u32 LE][pkt.to_bytes() = 56 bytes (+extra_data)]` (`make_game_packet`), emit traffic, send `Packet::reliable(raw)` if `reliable` else `Packet::unreliable(raw)`.

`IncomingPacket::parse(data)`: `len < 4` → `None`; read `msg_type` (first 4 bytes LE); `1`→ServerHello, `2|3`→NUL/high-byte-terminated UTF-8 string (Text/GameMessage), `4`→GameUpdate (56-byte struct + optional extra_data). Full struct layout is in the packet spec.

### 2.17 Login packet wire formats (exact — `build_login_packet`, `build_redirect_packet`, `build_login_data`)

All are newline-terminated `key|value\n` strings sent via `send_text` (message type 2). `{PROTOCOL}`=226, `{FHASH}`=-716928004.

**`build_login_packet()`** — three variants:
1. If `login_method == Newly`: exactly
   `protocol|{PROTOCOL}\nltoken|{ltoken}\nplatformID|{platform_id}\n`
2. Else if `login_token_field(ltoken) == "UbiTicket"` (token has ≥3 dot-segments): fields in order —
   `UbiTicket|{ltoken}` , `requestedName|` , `f|1` , `protocol|{PROTOCOL}` , `game_version|{game_version}` , `fz|{fz}` , `cbits|{cbits}` , `player_age|{player_age}` , `GDPR|{gdpr}` , `FCMToken|` , `category|{category}` , `totalPlaytime|{total_playtime}` , `klv|{klv}` , `steamToken|{steam_token}` , `hash2|{hash2}` , `meta|{meta}` , `fhash|{FHASH}` , `rid|{rid}` , `platformID|{platform_id}` , `deviceVersion|0` , `country|{country}` , `hash|{hash}` , `mac|{mac}` , `wk|{wk}` , `zf|{zf}` (each `\n`-terminated).
3. Else (plain `token`): same field set but header order is `protocol|…` , `ltoken|{ltoken}` , `platformID|{platform_id}` , `requestedName|` , `f|1` , `game_version|…` , … (note: no `UbiTicket`; `platformID` appears near the top, not before `deviceVersion`). Fields after that: `fz, cbits, player_age, GDPR, FCMToken|, category, totalPlaytime, klv, steamToken, hash2, meta, fhash, rid, deviceVersion|0, country, hash, mac, wk, zf`.

**`build_redirect_packet(r)`** — built line-by-line (order matters):
`tankIDName|{r.tank_id_name}` , `tankIDPass|` , `requestedName|` , `f|1` , **`protocol|211`** (hardcoded 211, NOT PROTOCOL) , `game_version|{game_version}` , `fz|{fz}` , `cbits|{cbits}` , `player_age|{player_age}` , `GDPR|{gdpr}` , `FCMToken|` , `category|{category}` , `totalPlaytime|{total_playtime}` , `klv|{klv}` , `hash2|{hash2}` , `meta|{meta}` , `fhash|{FHASH}` , `rid|{rid}` , `platformID|{platform_id}` , `deviceVersion|0` , `country|{country}` , `hash|{hash}` , `mac|{mac}` , `wk|{wk}` , `zf|{zf}` , `lmode|{r.lmode}` , `user|{r.user}` , `token|{r.token}` , `UUIDToken|{r.uuid}` , then **only if `r.door_id` non-empty**: `doorID|{r.door_id}` , finally `aat|2`.

**`build_login_data()`** — the `clientData` string for the (currently disabled) check-token endpoint; identical field set to variant 3 of the login packet minus `protocol|`/`ltoken|`/`platformID` header (starts `requestedName|\nf|1\nprotocol|{PROTOCOL}\n…`). Port for completeness; not on the hot path since check_token is disabled.

---

## 3. DEPENDENCY MAPPING (Rust crate → Nxrth C++)

| Rust dependency (this module) | Used for | Nxrth C++ replacement |
|---|---|---|
| `rusty_enet` (`enet::Host`, `Packet`, `PeerID`, `RangeCoder`, `crc32`, `HostSettings`) | ENet game protocol over UDP | **Vendored C ENet, patched for SOCKS5-UDP.** Keep `peer_limit=1`, `channel_limit=2`, range-coder compressor, crc32 checksum, "new packet" mode. |
| `crate::socks5::Socks5UdpSocket` | SOCKS5 UDP-ASSOCIATE tunnel for the ENet socket | The patched ENet socket backend + a SOCKS5 UDP client (see socks5 spec). `bind_through_proxy(local, proxy_addr, user, pass)`. |
| `ureq`/`reqwest` (via `get_server_data_proxied`, `fetch_*credentials`) | HTTP login (server_data.php, dashboard, growid) | **libcurl** with `socks5h://` proxy support + cookie jar. |
| `crate::proxy_pool::RotatingLoginProxy` / `next_game_proxy` | Rotating login/game proxy pool | Native C++ round-robin proxy pool (see proxy_pool spec). `fresh_url()`, `next_game_proxy(current)`. |
| `serde_json` (in auth/server_data) | JSON parse of login responses | **nlohmann/json**. |
| `md5` (via `compute_klv`) | KLV key derivation | **bundled md5**. |
| `argon2` (elsewhere in auth) | password hashing on some login paths | **argon2 lib** (not directly in this file; carry for auth). |
| `crossbeam_channel` (`event_tx`, `script_req_rx`, `script_reply_tx`) | script thread comms | **`std::mutex` + `std::condition_variable` bounded queue** (`try_recv`/`try_send` semantics: non-blocking). In Nxrth these feed the **native automation subsystem**, not Lua. |
| `mlua` (script_stop, drain_script_requests) | Lua scripting VM | **NONE — native C++ automation.** `script_stop` becomes an automation-interrupt flag; `drain_script_requests()` becomes native command dispatch. Drop Lua entirely. |
| `tokio`/`axum`/`tower` (`ws_tx: WsTx`, `WsEvent`) | async web server + WebSocket UI feed | **`std::thread` + Dear ImGui native UI. NO web server, NO WebSocket.** Replace `emit(WsEvent::…)` with pushing into an ImGui-observed state/event buffer. Keep the console-ring-buffer (cap 100) and per-bot event semantics; render them in ImGui. |
| `std::sync::{Arc, Mutex, RwLock, OnceLock, AtomicBool}` | shared ownership + gates | `std::shared_ptr`, `std::mutex`, a read/write lock (`std::shared_mutex`), lazy-init statics (function-local static or `std::once_flag`), `std::atomic<bool>`. |
| `std::time::Instant` / `SystemTime` | monotonic timing / wall clock | `std::chrono::steady_clock` for all `Instant`; `std::chrono::system_clock` ONLY for `now_millis()`. |

libcurl specifics for the login flow: use `socks5h://` (remote DNS) so the proxy resolves the GT host; enable a cookie jar; the `alternate` flag selects the growtopia vs growtopia2 server_data host; on HTTP 403 skip the alternate for that proxy candidate (`is_http_403_text`).

---

## 4. THREADING & SHARED STATE

**Per-bot threading.** Each `Bot` runs on its own OS thread; `run()` is the thread body — a 10 ms tick loop that services ENet, drains commands, runs the login watchdog, dispatches automation, and auto-collects. There is **no async**; port to one `std::thread` per bot. The ENet host, peer, and all non-`pub` fields are single-thread-owned by that loop.

**Bot ↔ UI shared state.** `state: Arc<RwLock<BotState>>` is the read-mostly bridge to the UI. The bot writes status/world/players/ping/console; the UI reads. Console is a ring buffer capped at **100** entries (drop oldest). In Nxrth: `std::shared_ptr` + `std::shared_mutex`; ImGui reads it. Rust recovers from a poisoned lock via `PoisonError::into_inner` — in C++ there is no poisoning; just lock normally, but never hold the state lock across an ENet/network call.

**Bot ↔ script/automation.** `event_tx`/`script_req_rx`/`script_reply_tx` are crossbeam channels to a script worker. In Nxrth these become mutex+condvar queues feeding the **native** automation subsystem (no Lua). `script_stop` is a shared `atomic<bool>` interrupt.

**FLEET-WIDE shared state — the stagger gates (the important cross-bot coupling).** The six process-static gates (§1.2) plus `reserve_gate_slot`/`reserve_throttle_slot`/`wait_global_gate` are the ONLY state shared across *all* bots, and they are how bots become "aware of each other" for login pacing. Every bot reserves slots from the same monotonic cursor so the fleet's HTTP logins, login packets, enter-game, warps, and post-throttle reconnects are serialized/spaced rather than bursting. In Nxrth these MUST remain a single set of process-global objects shared by every bot thread (a `std::mutex`-guarded `steady_clock::time_point` each), lazily initialized once. Getting this wrong = the "toplu sokunca giremiyor" (can't mass-login) failure the comments describe. Design notes to preserve:
- Connected-phase gates (LOGIN_PACKET/ENTER_GAME/WARP) service ENet during the wait (`wait_for_global_gate`) and use the small `GATE_CONNECTED_MAX_AHEAD_MS=2500` ceiling; the HTTP gate dead-sleeps with the large `GATE_HTTP_MAX_AHEAD_MS=300000` ceiling.
- `in_gate_wait` prevents recursion when the serviced wait's `service_once` re-enters another gate.
- The gateway-throttle gate (`GATEWAY_LOGON_GATE`) applies a `32 s` cooldown floor + `1.5 s` single-file spacing, capped at cooldown+60 s.
- `next_game_proxy` mutates only the calling bot's `self.proxy`; the pool itself (round-robin state) is shared and rotates a different exit each call — this is what lets a throttled bot escape an IP-bound `ltoken` throttle (see the memory note on GT ltoken IP-binding).

---

## 5. RENAME RULES (Mori→Nxrth, Cloei→North) for the C++ port

General rules to apply mechanically across this module:
- Every `Mori`/`mori` identifier, file/module path, log line, window title, user-agent, config filename → `Nxrth`/`nxrth`.
- Every `Cloei`/`cloei` (upstream author/repo) → `North`/`north`.

**Concrete occurrences found in `bot/core.rs`:**
- **Line 167**, doc comment on `LoginMethod::Newly`: `"Original Mori-style GrowID login without HAR fallbacks."` → `"Original Nxrth-style GrowID login without HAR fallbacks."` (the ONLY literal `Mori` in this file).
- **No `Cloei`/`cloei` occurrences** appear in this file.

**Cross-cutting rename targets in this module (not literal "Mori" here, but part of the framework identity — normalize when porting):**
- The `crate::` module paths (`crate::logger`, `crate::proxy_pool`, `crate::socks5`, `crate::har_parser`, `crate::bot_state`, …) become Nxrth namespaces/headers (e.g. `nxrth::logger`). The crate itself is the Mori binary (`Mori.exe`) → Nxrth (`Nxrth.exe`); see the build/copy convention memory note (build → copy to the Nxrth test folder).
- Log-line tags `"[Bot]"` / `"[Bot#{id}]"` are NOT Mori-branded — keep as-is (or rebrand only if you rebrand the whole log format).
- `requestly_logs.har` (referenced by `LoginMethod::Requestly`) is a fixed external filename, not a Mori identifier — leave unless the operator's config renames it.
- Any operator-facing strings you add (window title, user-agent, config filenames) must use `Nxrth`/`nxrth`. This module itself sets no window title or user-agent (those live in the web/UI and HTTP layers — apply the rename there).
- The disabled `check_token` block and `println!("=== RAW LOGIN PACKET ===")` carry no brand strings; port or drop the stdout dump as you prefer.

---

## 6. Porting checklist / gotchas (do-not-break list)

1. **create_host anti-IP-leak**: 4 SOCKS5-bind tries, 300 ms apart; on total failure bind a DEAD loopback and let connect fail — NEVER fall back to a real direct socket when a proxy was configured.
2. **next_event swallows socket errors** — a service error skips the tick, it does not crash the thread.
3. **Bypass-IP pinning**: gateway logon (`build_login_packet` path) uses `bypass_enet.or(proxy)`; redirect/subserver + world traffic uses `proxy`. `apply_credentials`/`refresh_token` re-pin `bypass_enet`.
4. **Connected-phase gates keep ENet serviced during the wait** (`wait_for_global_gate`), guarded by `in_gate_wait`. Dead-sleeping here causes fleet login collapse.
5. **Monotonic gate cursor with a horizon cap** (`reserve_gate_slot`): bounded backlog; never schedule past `now + max_ahead_ms`.
6. **"Fail to login 30s" rotate**: with a rotating login proxy, restart the whole login on a fresh exit IP + token every time (`refresh_token_on_reconnect=true`, streak reset), waiting the fleet throttle slot; without one, reuse the token and wait.
7. **Disconnect branch priority**: redirect → scheduled-reconnect → auto_reconnect{before-hello / was_in_world / post-hello-reject} → disabled. Exact streak counters and backoff formulas (§2.8, §2.15) are load-bearing.
8. **30 s login watchdog** rotates the game proxy and forces a clean gateway reconnect.
9. **protocol|211 is hardcoded** in the redirect packet (not PROTOCOL=226).
10. **Little-endian everywhere**; 4-byte message-type prefix (1/2/3/4); `GAME_PACKET_SIZE=56`.
