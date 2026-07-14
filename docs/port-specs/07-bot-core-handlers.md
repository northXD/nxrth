# Port Spec 07 — Bot Core Event Loop & Packet Handlers

**Source:** `Mori-2.0.0/src/bot/core.rs` (approx. lines 1760–3520, plus supporting
types pulled in from `protocol/packet.rs`, `protocol/variant.rs`, `protocol/crypto.rs`,
`player.rs`, `constants.rs`).
**Target module:** Adonai `bot/core` — the per-bot ENet service loop, disconnect/redirect
state machine, incoming-packet dispatch, login/redirect packet builders, and the
`CallFunction` handler dispatch.

This document is the single source of truth. An engineer must be able to reimplement
this module in C++ **without** reading the Rust. Every constant, offset, magic string,
and control-flow branch below is load-bearing — preserve them exactly. GT wire data is
**little-endian**.

> **Rename rules are mandatory** — see §5. In the port, everything is Adonai/North, not
> Mori/Cloei. This spec quotes the original identifiers so you can find them; do not copy
> the old names into Adonai.

---

## 0. Constants (module-level)

All of these live at the top of the module and must be reproduced with identical values.

### 0.1 Stagger / pacing constants (`u64` milliseconds unless noted)

| Name | Value | Meaning |
|---|---|---|
| `LOGIN_PACKET_STAGGER_MS` | `1000` | Min spacing between bots sending their login packet after a (non-redirect) ServerHello. |
| `ENTER_GAME_STAGGER_MS` | `2000` | Min spacing between bots sending `action|enter_game`. |
| `HTTP_LOGIN_STAGGER_MS` | `2500` | Min spacing between each bot's HTTP login phase (server_data/dashboard/growid). |
| `WARP_STAGGER_MS` | `1200` | Min spacing between world warps (used elsewhere; gate declared here). |
| `GATEWAY_THROTTLE_COOLDOWN_MS` | `32_000` | Process-global cooldown after a gateway "Fail to login… 30 seconds" throttle. |
| `GATEWAY_LOGON_SPACING_MS` | `1500` | Spacing between fleet gateway-logon retries after a throttle cooldown. |
| `DASHBOARD_STAGGER_MS` | `3500` | Fleet-wide min spacing between growid dashboard POSTs. |
| `REDIRECT_MAX_GAME_PROXY_TRIES` | `6` (`u8`) | Game proxies to try for one redirect's subserver connect before abandoning the redirect for a full fresh logon. |
| `GEIGER_COUNTER_ITEM_ID` | `2204` (`u16`) | Geiger counter item id (used by geiger logic, out of this module's focus). |
| `DEAD_GEIGER_COUNTER_ITEM_ID` | `2286` (`u16`) | Dead geiger counter item id. |
| `GATE_CONNECTED_MAX_AHEAD_MS` | `2500` | Backlog ceiling for connected-phase gates (LOGIN_PACKET / ENTER_GAME / WARP). Must stay well under the server's post-ServerHello login timeout since a live ENet peer is held open while waiting. |
| `GATE_HTTP_MAX_AHEAD_MS` | `300_000` | Backlog ceiling for the pre-connection HTTP login gate (dead sleep, no live peer). |

### 0.2 Protocol/version constants (from `constants.rs`)

| Name | Value | Notes |
|---|---|---|
| `PROTOCOL` | `226` (`u32`) | Protocol number = 175 + minor. Sent as `protocol|226` in login packets. |
| `GAME_VER` | `"5.51"` | `game_version` field. Force-enforced by the live game as of 2026-07-11 (5.50 → UPDATE REQUIRED). |
| `FHASH` | `-716928004` (`i32`) | Emitted as `fhash|-716928004`. |
| `DEFAULT_RID` | `"025B42980AFB659E0394C846233653FF"` | |
| `DEFAULT_MAC` | `"74:d4:dd:6c:24:e1"` | |
| `DEFAULT_WK` | `"788E366E74D2D098398A35C3F6360DDA"` | |
| `DEFAULT_HASH` | `"381621508"` | |
| `DEFAULT_HASH2` | `"-332772458"` | |
| `DEFAULT_FZ` | `"18274296"` | |
| `DEFAULT_PLATFORM_ID` | `"15,1,0"` | |
| `DEFAULT_ZF` | `"1597752569"` | |
| `DEFAULT_STEAM_TOKEN` | long hex-string blob ending `…5d.240` (see constants.rs verbatim) | Copy byte-for-byte. |

**Note on redirect packet:** `build_redirect_packet` hardcodes `protocol|211` (NOT
`PROTOCOL`). This is intentional — the subserver redirect logon uses protocol 211. Keep it.

### 0.3 Fleet-wide static gates (process-global, lazily initialized)

Each is a `OnceLock<Mutex<Instant>>` in Rust — i.e. a lazily-initialized, mutex-guarded
`Instant` (monotonic timestamp) shared across **all** bot threads in the process.

- `LOGIN_PACKET_GATE`
- `ENTER_GAME_GATE`
- `HTTP_LOGIN_GATE`
- `WARP_GATE`
- `GATEWAY_LOGON_GATE` — reconnect scheduler after a gateway throttle.
- `DASHBOARD_GATE` — growid dashboard POST pacing.

**C++ mapping:** one `struct Gate { std::mutex m; std::chrono::steady_clock::time_point next_allowed; bool init=false; };`
per gate as a function-local `static` (thread-safe init under C++11) or a namespace-scope
`static` with a `std::once_flag`. These are **fleet-wide shared state** (§4).

---

## 1. TYPES

### 1.1 `LoginMethod` (enum, private)

Controls initial login and token-refresh fallback. Variants:

```
Legacy   { password: String }   // Standard GrowID: if check_token fails, re-login w/ password.
Newly    { password: String }   // Original "Newly"-style GrowID login without HAR fallbacks.
Requestly{ password: String }   // HAR-backed dashboard login; replays requestly_logs.har on refresh failure.
Ltoken                          // Token provided directly; refresh failure → stop the bot (no fallback).
HarToken { har_path: String }   // Token from a local .har file; bypasses server_data.php. Refresh → re-extract from same HAR.
```

C++: `enum class LoginMethodKind { Legacy, Newly, Requestly, Ltoken, HarToken };` plus a
`std::string password_or_har_path` payload. `build_login_packet` only branches on `Newly`
(§2.6), so at minimum the kind must be queryable.

### 1.2 `RedirectData` (struct, `#[derive(Clone)]`)

Captured from `OnSendToServer`; the subserver token/uuid are consumed once.

| Field | Type | Notes |
|---|---|---|
| `server` | `String` | Subserver host/IP. |
| `port` | `u16` | Subserver port. |
| `token` | `String` | Subserver login token (may be resolved from cache — see §2.9). |
| `user` | `String` | User id string. |
| `door_id` | `String` | Door id; `"0"` if the redirect gave none. |
| `uuid` | `String` | UUIDToken (may be resolved from cache). |
| `lmode` | `String` | Login mode (stringified `i32`). |
| `tank_id_name` | `String` | Growtopia account name for the redirect logon. |

### 1.3 `GeigerGreenRepeat` (struct, `#[derive(Clone, Copy)]`)

Peripheral to this module (geiger tracking) but a field of `Bot`.

| Field | Type |
|---|---|
| `x` | `u32` |
| `y` | `u32` |
| `last_seen_ms` | `u64` |
| `count` | `u8` |

### 1.4 `BotHost` (enum, private) — ENet host wrapper

Wraps the ENet host so the socket type (direct UDP vs SOCKS5-tunneled UDP) is erased.

```
Direct(enet::Host<UdpSocket>)
Socks5(enet::Host<Socks5UdpSocket>)
```

Methods (all just dispatch to the inner host):

- `next_event() -> Option<EventNoRef>` — calls `host.service()`. **Critical:** on
  `Err(_)` it returns `None` (skip the tick) — it must **NOT** panic/throw. A transient
  socket error (e.g. Windows `WSAECONNRESET` after a SOCKS5 relay ICMP port-unreachable)
  must not kill the bot thread; a truly dead relay is still caught by ENet's own timeout →
  clean `Disconnect`. On `Ok(Some(e))` returns `e.no_ref()`; on `Ok(None)` returns `None`.
- `connect(addr: SocketAddr, channels: usize, data: u32)` — `host.connect(...)`; the Rust
  `.expect("connect failed")` panics on failure (acceptable — connect config error).
- `peer_rtt(id) -> Duration`
- `peer_send(id, channel: u8, packet: &enet::Packet)` — errors ignored (`.ok()`).
- `peer_disconnect(id, data: u32)`
- `peer_set_timeout(id, limit: u32, minimum: u32, maximum: u32)` — used with
  `(0, 12_000, 30_000)` on connect (see §2.2). limit=0 keeps ENet's default (32).

**C++ mapping:** a single `EnetHost` wrapper over the vendored C ENet host; the socket
kind (direct vs SOCKS5-UDP) is a construction parameter, not a type split. `next_event()`
must swallow socket errors and return "no event".

### 1.5 `Bot` (struct, `pub`) — the per-bot actor

Every field (exact names + types). Fields are grouped by concern; the group headers are
commentary, not part of the layout.

**Networking / identity**
| Field | Type | Notes |
|---|---|---|
| `host` | `BotHost` | Current ENet host. Rebuilt on redirect and reconnect. |
| `proxy` | `Option<Socks5Config>` (pub) | Assigned **game** proxy. World/subserver traffic uses this. |
| `login_proxy` | `Option<RotatingLoginProxy>` | Rotating HTTP login proxy (per-attempt fresh exit IP). |
| `bypass_enet` | `Option<Socks5Config>` | Pinned bypass exit IP for the ENet **logon** (`protocol|225` to gateway), matching the IP that minted the current `ltoken`. `None` → logon uses the game proxy. |
| `stop` | `Arc<AtomicBool>` | Shared stop flag (same `Arc` passed to `run`). |
| `username` | `String` (pub) | GrowID / display name. |
| `login_method` | `LoginMethod` | |
| `ltoken` | `String` | Legacy token from HTTP login; used in first (non-redirect) ServerHello. |
| `meta` | `String` | `meta` from server_data.php; echoed in every login packet. |

**Per-session login identity (all `String`)**
`mac` (pub), `hash`, `hash2`, `klv`, `fz`, `game_version`, `cbits`, `player_age`, `gdpr`,
`category`, `total_playtime`, `country`, `zf`, `platform_id`, `steam_token`, `wk`, `rid`.
These populate the login/redirect packets. Defaults resolved via `resolve_login_identity`
(§2.5).

**Redirect / reconnect state machine**
| Field | Type | Notes |
|---|---|---|
| `redirect` | `Option<RedirectData>` | Set by `OnSendToServer`; consumed by next redirected ServerHello. |
| `redirect_attempts` | `u8` | Whether the current redirect token was already sent (incremented on consume). |
| `last_redirect_token` | `Option<String>` | Last concrete subserver token this session (redirect `-N` markers reuse it). |
| `last_redirect_uuid` | `Option<String>` | Last concrete subserver UUIDToken this session. |
| `refresh_token_on_reconnect` | `bool` | Forces next non-redirect reconnect to fetch a fresh token first. |
| `server_addr` | `Option<SocketAddr>` | Gateway address (from server_data). |
| `saw_server_hello` | `bool` | True once a ServerHello was seen on the current connection. |
| `connected_since` | `Option<Instant>` | Set on Connect; login watchdog uses it (30 s → drop & retry). Cleared in-world. |
| `was_in_world` | `bool` | True once fully in-world (`OnSpawn` self). Distinguishes in-world drop from logon reject. |
| `redirect_connect_fails` | `u8` | Consecutive failures reaching the redirect **subserver**. Rotates game proxy; after `REDIRECT_MAX_GAME_PROXY_TRIES` abandons redirect. Reset on any ServerHello. |
| `pre_hello_disconnects` | `u8` | Consecutive disconnects before any ServerHello. `>=3` forces a fresh login. |
| `login_reject_streak` | `u32` | Consecutive logons that reached ServerHello + sent `protocol|225` but were dropped with no redirect (silent reject = exit IP rate-limited/flagged). Drives escalating backoff. Reset on redirect. |
| `login_throttle_streak` | `u32` | Consecutive `logon_fail` carrying the "login throttled" flag. Reset on redirect. |
| `in_gate_wait` | `bool` | Re-entrancy guard: prevents a `service_once` made while waiting in a stagger gate from recursing into another gate wait. |
| `reconnect_after` | `Option<Instant>` | Delayed-reconnect deadline (2FA/overload/throttle cooldowns). |

**World / player state**
| Field | Type | Notes |
|---|---|---|
| `start_time` | `Instant` | Network time base for ping replies. |
| `pos_x`, `pos_y` | `f32` (pub) | Bot position in **pixels**. |
| `local` | `LocalPlayer` (pub) | Bot's identity/physics in current world. |
| `players` | `HashMap<u32, Player>` (pub) | Other players, keyed by net_id. |
| `inventory` | `Inventory` (pub) | Updated on SendInventoryState. |
| `equipped_items` | `HashSet<u16>` | Locally-cached active/equipped item ids. |
| `world` | `Option<World>` (pub) | Current world; `Some` = in a world. Updated on SendMapData. |
| `peer_id` | `Option<enet::PeerID>` | Active peer; set on Connect, cleared on Disconnect. |

**Shared state & IO**
| Field | Type | Notes |
|---|---|---|
| `state` | `Arc<RwLock<BotState>>` (pub) | Web/UI-visible mirror. §4. |
| `cmd_rx` | `CmdReceiver` | Commands from the UI layer, drained each tick. |
| `temporary_data` | `TemporaryData` (pub) | Holds a one-shot `dialog_callback` fired on next `OnDialogRequest`. |
| `auto_collect` | `bool` (pub) | Run loop auto-collects nearby dropped items. |
| `auto_reconnect` | `bool` (pub) | Auto-reconnect after disconnect. |
| `collect_radius_tiles` | `u8` | 1–5; pixel radius = tiles × 32. |
| `collect_blacklist` | `HashSet<u16>` | Item ids excluded from auto-collect. |
| `collect_timer` | `Instant` | Last collect() time. |
| `astar` | `AStar` | Reused pathfinder. |
| `pathfind_target` | `Option<(u32,u32)>` | Current routing target tile. |
| `pathfind_recalc` | `bool` | Set on `OnSetPos` during pathfinding to replan. |
| `delays` | `BotDelays` (pub) | Configurable action/cooldown delays. |
| `items_dat` | `Arc<ItemsDat>` (pub) | Item DB for collision lookups. |
| `event_tx` | `Option<crossbeam_channel::Sender<BotEventRaw>>` | Forwards raw events to a running script thread. |
| `script_req_rx` | `Option<Receiver<ScriptRequest>>` | Requests from script thread. |
| `script_reply_tx` | `Option<Sender<ScriptReply>>` | Replies to script thread. |
| `script_stop` | `Arc<AtomicBool>` (pub) | Interrupt a running script. |

> **Adonai note:** `event_tx`/`script_req_rx`/`script_reply_tx`/`script_stop` exist to
> drive the **Lua** script engine. Adonai has **no Lua**. Drop the Lua VM entirely; if
> Adonai keeps a native scripting/automation hook, replace these channels with the
> std::mutex+condvar queue described in §3. The `try_send(BotEventRaw::…)` calls in
> `service_once` become "push to the native automation queue if one is attached".

**Pending login-failure flags (all `bool`)** — set from substring matches on incoming
`GameMessage`/`Track`, consumed by the next `action|logon_fail`:
`pending_2fa`, `pending_relogon`, `pending_server_overload`, `pending_too_many_logins`,
`pending_login_throttle`, `pending_place_prepare`, `pending_update_required`,
`pending_maintenance`.

**Misc**
| Field | Type | Notes |
|---|---|---|
| `stop_requested` | `bool` | Makes `run` exit next iteration. |
| `bot_id` | `u32` (pub) | Fleet id; tags WS events and log lines. |
| `ws_tx` | `Option<WsTx>` | Broadcast sender for real-time UI events (None standalone). **Adonai: replace with the ImGui/native event sink.** |
| `last_ping` | `u32` | Last broadcast ping (suppresses redundant BotPing). |
| `geiger_green_repeat` | `Option<GeigerGreenRepeat>` | Geiger green-particle repeat tracker. |

### 1.6 `LocalPlayer` (from `player.rs`)

| Field | Type | Source |
|---|---|---|
| `net_id` | `u32` | From `OnSpawn` self (`netID`). |
| `user_id` | `u32` | From `OnSpawn` self (`userID`). |
| `hack_type` | `u32` | From `SetCharacterState`: `pkt.value`. Echoed as `net_id` in ping reply when in-world. |
| `build_length` | `u8` | From `SetCharacterState`: `jump_count - 126` (saturating). |
| `punch_length` | `u8` | From `SetCharacterState`: `animation_type - 126` (saturating). |
| `gravity` | `f32` | From `SetCharacterState`: `vector_x2`. |
| `velocity` | `f32` | From `SetCharacterState`: `vector_y2`. |

(Names `build_length`/`punch_length` are legacy misnomers — they carry tile X/Y used to
scale the ping reply vectors, see §2.8.)

### 1.7 `Player` (from `player.rs`)

`net_id: u32`, `user_id: u32`, `name: String`, `country: String`, `position: (f32,f32)`
(pixels), `avatar: String`, `online_id: String`, `e_id: String`, `ip: String`,
`col_rect: String`, `title_icon: String`, `m_state: u32`, `invisible: bool`.

### 1.8 Wire packet types (from `protocol/packet.rs`)

#### Outer message-type tags (`u32`, first 4 bytes of every ENet payload, little-endian)

| Const | Value |
|---|---|
| `MSG_SERVER_HELLO` | `1` |
| `MSG_TEXT` | `2` |
| `MSG_GAME_MESSAGE` | `3` |
| `MSG_GAME_PACKET` | `4` |
| `MSG_TRACK` | `6` |
| `MSG_CLIENT_LOG_REQUEST` | `7` |

(There is no type 5 defined here.)

#### `IncomingPacket` (parse result)

```
ServerHello
Text(&str)
GameMessage(&str)
GameUpdate(GameUpdatePacket)
Track(&str)
ClientLogRequest
Unknown { msg_type: u32, data: &[u8] }
```

**`IncomingPacket::parse(data)` — deserialization steps:**
1. If `data.len() < 4` → return `None` (no packet).
2. `msg_type = u32 LE` from `data[0..4]`. `payload = data[4..]`.
3. Switch on `msg_type`:
   - `1` → `ServerHello`.
   - `2` or `3` → decode a string: take `payload` up to the first byte that is `0x00`
     **or** `>= 0x80` (NUL- or high-byte-terminated), UTF-8 decode; if not valid UTF-8 →
     `None`. Wrap as `Text` (2) or `GameMessage` (3).
   - `4` → `GameUpdatePacket::from_bytes(payload)` → `GameUpdate`, or `None` if too short.
   - `6` → same string decode as (2/3) → `Track`; invalid UTF-8 → `None`.
   - `7` → `ClientLogRequest`.
   - anything else → `Unknown { msg_type, data: payload }`.

> C++: the string terminator scan is "stop at first byte where `b == 0 || b >= 0x80`".
> Do not assume a single NUL.

#### `GamePacketType` (`repr(u8)`) — NET_GAME_PACKET subtypes

Byte value 0 in the game packet header. Full table (value → name):

`0x00 State, 0x01 CallFunction, 0x02 UpdateStatus, 0x03 TileChangeRequest, 0x04 SendMapData,
0x05 SendTileUpdateData, 0x06 SendTileUpdateDataMultiple, 0x07 TileActivateRequest,
0x08 TileApplyDamage, 0x09 SendInventoryState, 0x0A ItemActivateRequest,
0x0B ItemActivateObjectRequest, 0x0C SendTileTreeState, 0x0D ModifyItemInventory,
0x0E ItemChangeObject, 0x0F SendLock, 0x10 SendItemDatabaseData, 0x11 SendParticleEffect,
0x12 SetIconState, 0x13 ItemEffect, 0x14 SetCharacterState, 0x15 PingReply, 0x16 PingRequest,
0x17 GotPunched, 0x18 AppCheckResponse, 0x19 AppIntegrityFail, 0x1A Disconnect, 0x1B BattleJoin,
0x1C BattleEvent, 0x1D UseDoor, 0x1E SendParental, 0x1F GoneFishin, 0x20 Steam, 0x21 PetBattle,
0x22 Npc, 0x23 Special, 0x24 SendParticleEffectV2, 0x25 ActiveArrowToItem, 0x26 SelectTileIndex,
0x27 SendPlayerTributeData, 0x28 FtueSetItemToQuickInventory, 0x29 PveNpc, 0x2A PvpCardBattle,
0x2B PveApplyPlayerDamage, 0x2C PveNpcPositionUpdate, 0x2D SetExtraMods, 0x2E OnStepOnTileMod`,
and `Unknown(u8)` for any other value (round-trips the raw byte via `as_u8`).

C++: `enum class GamePacketType : uint8_t { … }` with a `from_u8`/`to_u8` that maps unknown
bytes to a sentinel while preserving the raw byte for re-emit.

#### `PacketFlags` (`u32` bitflags)

The only flag this module's dispatch depends on is `EXTENDED = 0x0000_0008` (presence of
`extra_data`). Full set for fidelity:

`WALK 0x1, UNK_2 0x2, SPAWN_RELATED 0x4, EXTENDED 0x8, FACING_LEFT 0x10, STANDING 0x20,
FIRE_DAMAGE 0x40, JUMP 0x80, GOT_KILLED 0x100, PUNCH 0x200, PLACE 0x400, TILE_CHANGE 0x800,
GOT_PUNCHED 0x1000, RESPAWN 0x2000, OBJECT_COLLECT 0x4000, TRAMPOLINE 0x8000, DAMAGE 0x10000,
SLIDE 0x20000, PARASOL 0x40000, UNK_GRAVITY_RELATED 0x80000, SWIM 0x100000, WALL_HANG 0x200000,
POWER_UP_PUNCH_START 0x400000, POWER_UP_PUNCH_END 0x800000, UNK_TILE_CHANGE 0x1000000,
HAY_CART_RELATED 0x2000000, ACID_RELATED_DAMAGE 0x4000000, UNK_3 0x8000000,
ACID_DAMAGE 0x10000000`.

Use `from_bits_retain` semantics — i.e. keep **all** bits including unknown ones (do not
mask). C++: store as raw `uint32_t`; test `EXTENDED` with `(flags & 0x8) != 0`.

#### `GameUpdatePacket` — **56-byte wire layout** (`GAME_PACKET_SIZE = 56`)

Struct fields and their byte offsets within the 56-byte header (everything after the outer
4-byte `msg_type`). All multi-byte values are **little-endian**.

| Offset | Size | Field | Type |
|---|---|---|---|
| 0 | 1 | `packet_type` | `u8` (GamePacketType) |
| 1 | 1 | `object_type` | `u8` |
| 2 | 1 | `jump_count` | `u8` |
| 3 | 1 | `animation_type` | `u8` |
| 4 | 4 | `net_id` | `u32` |
| 8 | 4 | `target_net_id` | `i32` |
| 12 | 4 | `flags` | `u32` (PacketFlags) |
| 16 | 4 | `float_variable` | `f32` |
| 20 | 4 | `value` | `u32` |
| 24 | 4 | `vector_x` | `f32` |
| 28 | 4 | `vector_y` | `f32` |
| 32 | 4 | `vector_x2` | `f32` |
| 36 | 4 | `vector_y2` | `f32` |
| 40 | 4 | `particle_rotation` | `f32` |
| 44 | 4 | `int_x` | `i32` |
| 48 | 4 | `int_y` | `i32` |
| 52 | 4 | `extra_data_size` | `u32` (byte length of `extra_data`) |
| 56 | var | `extra_data` | `Vec<u8>` (only present when `flags & EXTENDED`) |

**`from_bytes(data)`:**
1. If `data.len() < 56` → `None`.
2. Read all header fields at the offsets above (LE).
3. If `flags` contains `EXTENDED`: `start=56`, `end=56+extra_data_size`; if
   `data.len() < end` → `None`; else `extra_data = data[56..end]`. Otherwise
   `extra_data = empty`.

**`to_bytes()`:**
1. `extra_data_size = flags.EXTENDED ? extra_data.len() : 0`.
2. Allocate `56 + extra_data_size` zeroed bytes.
3. Write header fields at offsets (LE). `buf[52..56] = extra_data_size as u32 LE`.
4. If `extra_data_size > 0`, append `extra_data`.

> **Edge case:** if `EXTENDED` is set but `extra_data` is empty, `extra_data_size = 0` and
> no bytes are appended — round-trips cleanly. If `EXTENDED` is clear, `extra_data` is
> ignored entirely on serialize.

#### Packet builders

- `make_text_packet(text) -> Vec<u8>`: `[MSG_TEXT (u32 LE = 2)] + text bytes + [0x00]`.
- `make_game_message_packet(text) -> Vec<u8>`: `[MSG_GAME_MESSAGE (u32 LE = 3)] + text bytes + [0x00]`.
- `make_game_packet(pkt) -> Vec<u8>`: `[MSG_GAME_PACKET (u32 LE = 4)] + pkt.to_bytes()`.

Both text builders append a single trailing NUL. C++: build a `std::vector<uint8_t>`.

### 1.9 `Variant` / `VariantList` (from `protocol/variant.rs`)

Used to decode `CallFunction` args. Full variant deserialization is a separate module (06);
here is what this module relies on.

`Variant` cases: `Float(f32)`, `String(String)`, `Vec2(f32,f32)`, `Vec3(f32,f32,f32)`,
`Unsigned(u32)`, `Signed(i32)`, `Unknown`.

Accessors used here:
- `as_string() -> String`: Float→`v.to_string()`, String→clone, Vec2→`"{x}, {y}"`,
  Vec3→`"{x}, {y}, {z}"`, Unsigned→`v.to_string()`, Signed→`v.to_string()`, Unknown→`""`.
- `as_int32() -> i32`: `Signed(v)→v`, else `0`.
- `as_vec2() -> (f32,f32)`: `Vec2(x,y)→(x,y)`, else `(0.0,0.0)`.

`VariantList::deserialize(data) -> Result`:
1. `count = data[0]` (u8). Cursor advances byte-by-byte, LE for multi-byte.
2. For each of `count` entries: read `index: u8` (ignored), `var_type: u8`, then per type:
   Float→`f32`; String→`len: u32` then `len` bytes UTF-8-lossy; Vec2→2×`f32`; Vec3→3×`f32`;
   Unsigned→`u32`; Signed→`i32`; Unknown→no payload.
3. `get(index) -> Option<&Variant>` is positional (`variants[index]`).

**Adonai:** reimplement natively (nlohmann/json is NOT the wire format — this is a custom
binary TLV). See spec 06 for the full variant module.

### 1.10 `hash_string` (from `protocol/crypto.rs`)

Growtopia's rotate-left-5 hash with a trailing NUL. Used for the ping-reply
`target_net_id`.

```
fn hash_string(s: &str) -> i32:
    h: u32 = 0x55555555
    for each byte b in (s.bytes() then one 0u8):
        h = rotate_left(h, 5) wrapping_add (b as u32)
    return h as i32   // reinterpret the u32 bit pattern as i32
```

C++: `uint32_t h = 0x55555555; for (byte in s) { h = (h<<5)|(h>>27); h += byte; }` then one
more iteration with byte `0`, then `return (int32_t)h;`. Rotate is 32-bit `rotl`.

### 1.11 `parse_pipe_map` (from `player.rs`)

Parse `key|value\nkey|value\n…` into a map. For each line: split on the **first** `|`
(`splitn(2)`); `key = left.trim()`, `val = right` (raw, or `""` if no `|`). Skip lines whose
trimmed key is empty. Returns `HashMap<String,String>`. Used by `OnSpawn`/`OnRemove`/etc.

---

## 2. FUNCTIONS (behavioral spec)

### 2.1 `run(&mut self, stop_flag: Arc<AtomicBool>)` — the driver loop (context)

Top-level loop (one iteration ≈ 10 ms). Not the primary focus but frames the handlers:

1. If `stop_flag` (relaxed load) set → log `"[Bot] Stop flag set, exiting."`, break.
2. If `self.stop_requested` → log `"[Bot] Stop requested internally, exiting."`, break.
3. If `reconnect_after` set and now ≥ it: clear it; if `auto_reconnect`, capture
   `refresh = refresh_token_on_reconnect`, clear `refresh_token_on_reconnect`, log
   `"[Bot] Reconnect cooldown elapsed — reconnecting with current session"`, call
   `reconnect_main(refresh)`.
4. Drain `cmd_rx` (`try_recv` loop) → `handle_command`.
5. If `peer_id` set: read RTT (`peer_rtt` → ms as u32), write `state.ping_ms`; if changed
   vs `last_ping`, update and emit `BotPing`.
6. `service_once()` (§2.2).
7. **Login watchdog:** if `connected_since` set, `world.is_none()`, and elapsed ≥ **30 s**:
   log stall; rotate the game proxy via `next_game_proxy(self.proxy)` if it returns one
   (`self.proxy = Some(fresh)`, log `"rotating game proxy after stall → {addr}"`); clear
   `connected_since`, `redirect`, `redirect_attempts=0`, `redirect_connect_fails=0`,
   `saw_server_hello=false`; if `peer_id`, `host.peer_disconnect(id, 0)`. (Routes the
   resulting Disconnect down the clean-gateway-reconnect path.)
8. `drain_script_requests()` (Adonai: native automation queue; §3).
9. If `auto_collect` and `collect_timer` elapsed ≥ **500 ms**: reset timer, `collect()`.
10. Sleep 10 ms.
On loop exit: `shutdown()` (disconnect peer, service 5× with 10 ms sleeps, tear down
script channels).

### 2.2 `service_once(&mut self)` — the ENet event pump (**core**)

Drains **all** currently-available ENet events in a `while let Some(event) = host.next_event()`
loop, matching on event kind:

#### Connect `{ peer: id }`
- `peer_id = Some(id)`
- `saw_server_hello = false`
- `connected_since = Some(now)`
- `host.peer_set_timeout(id, 0, 12_000, 30_000)` — raise the timeout minimum from ENet's
  default 5 s to **12 s** (tolerate brief game-proxy relay gaps), max **30 s**; limit=0
  keeps default 32.
- log `"[Bot] Connected: peer {id.0}"`

#### Disconnect `{ peer: id, data }` — **the reconnect/redirect state machine**

`data` is the ENet disconnect reason code (0 = local/transport timeout; non-zero = a code
the server supplied on an explicit disconnect). Steps:

1. `peer_id = None`; `connected_since = None`; `pathfind_target = None`;
   `pathfind_recalc = false`.
2. log `"[Bot] Disconnected: peer {id.0} (reason code {data})"`.
3. **Capture** `disconnected_before_server_hello = redirect.is_none() && !saw_server_hello`
   (computed **now**, before any mutation).
4. Under `state` write lock: `status = Connecting`; `world_name = ""`; `players = []`;
   `ping_ms = 0`.
5. Emit `BotStatus{"connecting"}` and `BotWorld{""}`.
6. **Branch A — a pending redirect exists** (`let Some(r) = redirect.clone()`):
   - **A1. `redirect_attempts > 0`** (token already used): log
     `"[Bot] Redirect token was already used {n} time(s) — waiting for a fresh redirect"`;
     `redirect=None`; `redirect_attempts=0`; if `auto_reconnect` → `reconnect_main(false)`.
     *(Defensive path: in normal flow `on_server_hello` takes the redirect, so it is `None`
     by the time attempts>0; preserve the guard anyway.)*
   - **A2. `redirect_attempts == 0`** (still trying to reach the subserver): parse
     `"{r.server}:{r.port}"` as a socket address:
     - **Ok(addr):**
       - `redirect_connect_fails += 1` (saturating).
       - If `redirect_connect_fails >= REDIRECT_MAX_GAME_PROXY_TRIES` (6): log
         `"[Bot] redirect: {n} game proxies failed to reach subserver {server}:{port} — abandoning redirect, full re-login"`;
         `redirect=None`; `redirect_attempts=0`; `redirect_connect_fails=0`; if
         `auto_reconnect` → `reconnect_main(true)` (**true** = force fresh token).
       - Else:
         - If `redirect_connect_fails >= 2`: rotate the game proxy —
           `next_game_proxy(self.proxy)`; if `Some(fresh)`, log
           `"[Bot] redirect: subserver unreachable via current game proxy — rotating to {fresh.proxy_addr}"`,
           set `self.proxy = Some(fresh)`.
         - log `"[Bot] Redirecting to {server}:{port}"`.
         - `self.host = create_host(self.proxy.as_ref())` (rebuild host on the (possibly
           rotated) game proxy).
         - `saw_server_hello = false`.
         - `host.connect(addr, 2, 0)` (2 channels, connect data 0).
         - **Note:** `redirect` is NOT cleared here — it stays `Some` so the subserver's
           ServerHello can consume it (§2.7).
     - **Err(e):** log
       `"[Bot] Invalid redirect address '{server}:{port}' ({e}) — dropping redirect, reconnecting to gateway"`;
       `redirect=None`; `redirect_attempts=0`; `redirect_connect_fails=0`; if
       `auto_reconnect` → `reconnect_main(false)`.
7. **Branch B — no redirect, but `reconnect_after.is_some()`**: do nothing (a delayed
   reconnect, e.g. 2FA cooldown, is already scheduled).
8. **Branch C — no redirect, no scheduled reconnect, `auto_reconnect` true:**
   - `let mut refresh_token = refresh_token_on_reconnect; refresh_token_on_reconnect = false;`
   - **C1. `disconnected_before_server_hello`** (never reached a ServerHello):
     - `pre_hello_disconnects += 1` (saturating); log
       `"[Bot] disconnected before ServerHello ({n}/3)"`.
     - If `pre_hello_disconnects >= 3`: log
       `"[Bot] no ServerHello after retries - forcing fresh login"`; `refresh_token = true`;
       `pre_hello_disconnects = 0`.
     - log `"[Bot] Server disconnected — reconnecting with current session"`.
     - `schedule_reconnect("Server disconnected before ServerHello", refresh_token, 1_500)`.
   - **C2. else if `was_in_world`** (authenticated + OnSpawn'd, then dropped — almost always
     a flaky game/world proxy, NOT a logon reject):
     - `was_in_world = false`; `pre_hello_disconnects = 0`.
     - log `"[Bot] in-world session dropped (game proxy?) — restarting login from scratch on a fresh exit IP + token"`.
     - `schedule_reconnect("In-world session dropped", true, 1_500)` (**true** = fresh token).
     - Does **not** touch `login_reject_streak` (this is not a rate-limit event).
   - **C3. else** (reached ServerHello, sent `protocol|225`, dropped with no redirect & no
     auth = silent rejection → exit IP rate-limited/flagged):
     - `pre_hello_disconnects = 0`; `login_reject_streak += 1` (saturating).
     - If `refresh_token || login_reject_streak == 1` (first reject or a pending refresh —
       retry once promptly): log `"[Bot] Server disconnected — reconnecting with current session"`;
       `schedule_reconnect("Server disconnected after ServerHello", refresh_token, 1_500)`.
     - Else (escalating backoff): `secs = (15 * min(login_reject_streak, 8)).clamp(15,120)`;
       `reconnect_after = now + secs`; if `login_reject_streak % 3 == 0` →
       `refresh_token_on_reconnect = true`; log
       `"[Bot] logon rejected by gateway ({n}x, ServerHello but no redirect) — exit IP likely rate-limited; backing off {secs}s before retry"`.
9. **Branch D — auto-reconnect disabled:** log
   `"[Bot] Server disconnected — auto-reconnect is disabled"`.

> **Redirect-vs-reject discrimination is the crux.** The three post-ServerHello outcomes
> (in-world drop / gateway silent reject / redirect-in-progress) require different recovery.
> Preserve `was_in_world`, `saw_server_hello`, and the `redirect`/`redirect_attempts`
> semantics exactly, or the fleet will either hammer a throttled IP or fail to recover
> in-world drops.

#### Receive `{ peer: id, channel_id, packet }` — **incoming packet dispatch**

`packet_size = packet.data().len()`. `match IncomingPacket::parse(packet.data())`:

- **`Some(ServerHello)`**: `emit_traffic("in","server_hello",size,"ServerHello")`;
  `on_server_hello()` (§2.7).
- **`Some(Text(s))`**: `emit_traffic("in","text",size,redact_packet_text(s))`; log
  `"[Bot] Text: {s}"`.
- **`Some(GameMessage(s))`**: `emit_traffic("in","game_message",size,redact_packet_text(s))`;
  log `"[Bot] GameMessage: {s}"`; if `event_tx` present, `try_send(BotEventRaw::GameMessage{text})`
  (ignore failure). Then **substring scans** to set pending flags / take actions (order as
  written):
  - contains `"action|request_token"` → log
    `"[Bot] Server requested a fresh login token - fetching now."`; `redirect=None`;
    `redirect_attempts=0`; `refresh_token_on_reconnect=false`; `reconnect_after=None`;
    `reconnect_main(true)`; **`continue`** (restart the event-drain loop; skip remaining
    scans for this packet).
  - contains `"Advanced Account Protection"` → `pending_2fa = true`.
  - contains `"action|log"` && `"SERVER OVERLOADED"` → `pending_server_overload = true`.
  - contains `"action|log"` && `"Too many people logging in"` → `pending_too_many_logins = true`.
  - contains `"action|log"` && (`"Please try again in"` || `"Fail to login"`) →
    `pending_login_throttle = true`.
  - contains `"Server couldn't prepare a place"` → `pending_place_prepare = true`.
  - contains `"action|log"` && `"Server requesting that you re-logon"` → log
    `"[Bot] Server requested re-logon — clearing redirect data."`; `redirect=None`;
    `pending_relogon = true`.
  - contains `"action|log"` && `"UPDATE REQUIRED"` → `pending_update_required = true`.
  - contains `"action|log"` && `"undergoing maintenance"` → `pending_maintenance = true`.
  - contains `"action|logon_fail"` → **consume** the pending flags (first match wins, in
    this priority order), then unconditionally `host.peer_disconnect(id, 0)`:
    1. `pending_2fa`: clear it; `secs = delays.twofa_secs`; log
       `"[Bot] Logon failed — 2FA (Advanced Account Protection). Retrying in {secs} s."`;
       `state.status = TwoFactorAuth`; `reconnect_after = now + secs`; emit
       `BotStatus{"two_factor_auth"}`.
    2. `pending_server_overload`: clear; `secs = delays.server_overload_secs + (bot_id % 7)`;
       log `"… server overloaded. Retrying in {secs} s."`; `status = ServerOverloaded`;
       `reconnect_after = now + secs`; emit `BotStatus{"server_overloaded"}`.
    3. `pending_too_many_logins`: clear; `secs = delays.too_many_logins_secs + (bot_id % 5)`;
       log `"… too many logins at once. Retrying in {secs} s."`; `status = TooManyLogins`;
       `reconnect_after = now + secs`; emit `BotStatus{"too_many_logins"}`.
    4. `pending_login_throttle`: clear; `login_throttle_streak += 1` (saturating). Reserve a
       fleet-wide slot: `slot = reserve_throttle_slot(&GATEWAY_LOGON_GATE, GATEWAY_THROTTLE_COOLDOWN_MS, GATEWAY_LOGON_SPACING_MS)`;
       `secs = slot.saturating_duration_since(now).as_secs()`. `rotate = login_proxy.is_some()`.
       - If `rotate`: `refresh_token_on_reconnect = true`; `login_throttle_streak = 0`; log
         `"[Bot] Logon failed — 'Fail to login, try again in 30s' ({streak}x): restarting login from scratch on a FRESH exit IP + token, fleet retry in ~{secs} s."`.
       - Else: log
         `"[Bot] Logon failed — 'Fail to login, try again in 30s' ({streak}x); reusing token (no rotating login proxy configured), fleet retry in ~{secs} s."`.
       - `status = TooManyLogins`; `reconnect_after = Some(slot)`; emit `BotStatus{"too_many_logins"}`.
         (`streak` logged is the value captured **before** any reset above.)
    5. `pending_place_prepare`: clear; `secs = delays.server_overload_secs + (bot_id % 9)`;
       log `"… server could not prepare a place. Retrying in {secs} s."`; `status = ServerOverloaded`;
       `reconnect_after = now + secs`; emit `BotStatus{"server_overloaded"}`.
    6. `pending_relogon`: clear; log
       `"[Bot] Logon failed — server requested re-logon. Reconnecting."` (no cooldown set;
       the peer_disconnect below triggers an immediate-ish reconnect via the normal path).
    7. `pending_update_required`: clear; log
       `"[Bot] Logon failed — client update required. Stopping bot."`; `status = UpdateRequired`;
       emit `BotStatus{"update_required"}`; `stop_requested = true`.
    8. `pending_maintenance`: clear; `secs = delays.maintenance_secs`; log
       `"… server maintenance. Retrying in {secs} s."`; `status = Maintenance`;
       `reconnect_after = now + secs`; emit `BotStatus{"maintenance"}`.
    9. **else** (no pending flag): log
       `"[Bot] Logon failed — clearing redirect and reconnecting"`; `redirect=None`;
       `redirect_attempts=0`; `refresh_token_on_reconnect=true`.
    Then `host.peer_disconnect(id, 0)`.
- **`Some(GameUpdate(pkt))`**:
  `emit_traffic("in", "game_update:{pkt.packet_type:?}", size, format_game_packet_detail(&pkt))`;
  if `event_tx`, `try_send(BotEventRaw::GameUpdate{pkt.clone()})`; `update_geiger_signal(&pkt)`
  (geiger module, out of focus). Then `match pkt.packet_type`:
  - `SetCharacterState`: `local.hack_type = pkt.value`; `local.build_length =
    pkt.jump_count.saturating_sub(126)`; `local.punch_length = pkt.animation_type.saturating_sub(126)`;
    `local.gravity = pkt.vector_x2`; `local.velocity = pkt.vector_y2`.
  - `CallFunction`: `extra = pkt.extra_data.clone()`; `net_id = id.0 as u32`; if
    `VariantList::deserialize(&extra)` Ok → if `event_tx`, `try_send(BotEventRaw::VariantList{vl, net_id})`;
    then `on_call_function(id, &extra)` (§2.9 — note it re-deserializes the same bytes).
  - `PingRequest`: `on_ping_request(pkt.value)` (§2.8).
  - `SendInventoryState`: `Inventory::parse(&pkt.extra_data)` → on Ok log
    `"[Bot] Inventory: {count} items"`, set `equipped_items` = ids of items whose
    `flag & 1 != 0`, `inventory = inv.clone()`, `emit_inventory_update()`; on Err log
    `"[Bot] Inventory parse error: {e}"`.
  - `SendMapData`: `players.clear()`; `local = LocalPlayer::default()`;
    `geiger_green_repeat = None`; `World::parse(&pkt.extra_data)` → on Ok: log world dims;
    `world = Some(world.clone())`; build `TileInfo`/`WorldObjectInfo` vectors; under `state`
    lock set `world_name`, `world_width`, `world_height`, `tiles`, `objects`, `players=[]`,
    `status=InGame`, `geiger_signal=None`; drop lock; emit `BotStatus{"in_game"}`,
    `BotWorld{name}`, `WorldLoaded{name,width,height,tiles}`, `ObjectsUpdate{objects}`. On
    Err log `"[Bot] World parse error: {e}"`.
  - `State` → `on_state(&pkt)`; `TileChangeRequest` → `on_tile_change(&pkt)`;
    `SendTileUpdateData` → `on_send_tile_update_data`; `SendTileUpdateDataMultiple` →
    `on_send_tile_update_data_multiple`; `SendTileTreeState` → `on_send_tile_tree_state`;
    `ModifyItemInventory` → `on_modify_item_inventory`; `ItemChangeObject` →
    `on_item_change_object`; `SendLock` → `on_send_lock`. (These handlers are specced in
    the world/tile module; they consume `pkt` fields per §1.8.)
  - `_` (any other subtype) → log `"[Bot] {pkt}"` (the Display form from §1.8).
- **`Some(Track(s))`**: `emit_traffic("in","track",size,redact_packet_text(s))`; log
  `"[Bot] Track: {s}"`; if contains `"Authentication_error|23"` → `pending_place_prepare=true`.
  Parse `s` into a `HashMap<&str,&str>` by `split_once('|')` per line; read `Level`(u32),
  `GrowId`(u64), `installDate`(u64), `Global_Playtime`(u64), `Awesomeness`(u32) (default 0
  on missing/parse-fail); write `state.track_info = Some(TrackInfo{…})`; emit
  `BotTrackInfo{…}`.
- **`Some(ClientLogRequest)`**: `emit_traffic("in","client_log_request",size,"ClientLogRequest")`;
  log `"[Bot] ClientLogRequest"`.
- **`Some(Unknown{msg_type,data})`**: `emit_traffic("in","unknown:{msg_type}",size,"unknown msg_type={msg_type} payload_len={len}")`;
  log `"[Bot] Unknown msg_type={msg_type} len={len}"`.
- **`None`** (parse failure): hex-dump the raw bytes (`"%02x "`-joined);
  `emit_traffic("in","parse_error",size,"channel={channel_id}\n{hex}")`; log
  `"[Bot] Failed to parse packet ({n} bytes on ch {channel_id}): {hex}"`.

### 2.3 `send_text(&mut self, text: &str)`
If `peer_id` set: `raw = make_text_packet(text)`;
`emit_traffic("out","text",raw.len(),redact_packet_text(text))`;
`host.peer_send(id, 0, &Packet::reliable(raw))`. (Channel 0, **reliable**.) No-op if not
connected.

### 2.4 `send_game_message(&mut self, text: &str)`
Same as `send_text` but `make_game_message_packet` and kind `"game_message"`. Channel 0,
reliable.

### 2.5 `send_game_packet(&mut self, pkt: &GameUpdatePacket, reliable: bool)`
If `peer_id`: `raw = make_game_packet(pkt)`;
`emit_traffic("out","game_update:{pkt.packet_type:?}",raw.len(),format_game_packet_detail(pkt))`;
`enet_pkt = reliable ? Packet::reliable(raw) : Packet::unreliable(raw)`; `host.peer_send(id, 0, &enet_pkt)`.

### 2.6 `build_login_packet(&self) -> String` — first (gateway) logon body

Sent via `send_text` after a **non-redirect** ServerHello. Three exclusive forms:

**Form 1 — `login_method` is `Newly`** (minimal):
```
protocol|{PROTOCOL}\nltoken|{ltoken}\nplatformID|{platform_id}\n
```

**Form 2 — else if `login_token_field(&ltoken) == "UbiTicket"`** (JWT-style token, i.e.
`ltoken.split('.').count() >= 3`): the token is placed in the `UbiTicket|` field and the
field order differs slightly (note `requestedName|` and `f|1` come right after UbiTicket,
and there is no leading `protocol|` line before it):
```
UbiTicket|{ltoken}\nrequestedName|\nf|1\nprotocol|{PROTOCOL}\n
game_version|{game_version}\nfz|{fz}\ncbits|{cbits}\nplayer_age|{player_age}\nGDPR|{gdpr}\nFCMToken|\n
category|{category}\ntotalPlaytime|{total_playtime}\nklv|{klv}\nsteamToken|{steam_token}\nhash2|{hash2}\nmeta|{meta}\nfhash|{FHASH}\n
rid|{rid}\nplatformID|{platform_id}\ndeviceVersion|0\ncountry|{country}\nhash|{hash}\nmac|{mac}\nwk|{wk}\nzf|{zf}\n
```

**Form 3 — else (default `token`/`ltoken` login):**
```
protocol|{PROTOCOL}\nltoken|{ltoken}\nplatformID|{platform_id}\nrequestedName|\nf|1\n
game_version|{game_version}\nfz|{fz}\ncbits|{cbits}\nplayer_age|{player_age}\nGDPR|{gdpr}\nFCMToken|\n
category|{category}\ntotalPlaytime|{total_playtime}\nklv|{klv}\nsteamToken|{steam_token}\nhash2|{hash2}\nmeta|{meta}\nfhash|{FHASH}\n
rid|{rid}\ndeviceVersion|0\ncountry|{country}\nhash|{hash}\nmac|{mac}\nwk|{wk}\nzf|{zf}\n
```

`login_token_field(token)`: returns `"UbiTicket"` if `token.split('.').count() >= 3`
(i.e. ≥ 2 dots / 3 segments), else `"token"`. C++: count `.` occurrences; ≥2 → UbiTicket.

> Every `\n` is a literal newline. Field order and presence are load-bearing — the gateway
> validates the field set. `FCMToken|` and (Form 3) the missing `platformID` after `rid`
> are intentional. `klv` is not strictly validated by the current server (see memory
> gt-version-check) but must still be present and well-formed.

### 2.7 `on_server_hello(&mut self)` — ServerHello handler

1. `saw_server_hello = true`; `pre_hello_disconnects = 0`; `redirect_connect_fails = 0`
   (this leg's game proxy works).
2. `data = match redirect.take()`:
   - `Some(r)`: `redirect_attempts += 1` (saturating); log
     `"[Bot] ServerHello (redirect → {r.door_id})"`; `data = build_redirect_packet(&r)`.
   - `None`: log `"[Bot] ServerHello"`; `wait_for_global_gate(&LOGIN_PACKET_GATE,
     LOGIN_PACKET_STAGGER_MS=1000, "login packet")` (§2.12); `data = build_login_packet()`.
3. `println!` a `=== RAW LOGIN PACKET ===` block with `redact_packet_text(&data)` to stdout
   (developer console). **Adonai:** route through the native logger, redacted; do not print
   raw tokens.
4. `send_text(&data)`.

> `redirect.take()` **consumes** the redirect (sets it to `None`) and bumps
> `redirect_attempts` to 1. This is why the Disconnect A1 path (attempts>0 with redirect
> Some) is effectively unreachable in the happy path.

### 2.8 `on_ping_request(&mut self, challenge: u32)` — PingReply

`challenge` is `pkt.value` from a `PingRequest` game packet.

1. `time_val = start_time.elapsed().as_millis() as u32` (network time).
2. `bx = (local.build_length == 0) ? 2.0 : local.build_length as f32`.
3. `by = (local.punch_length == 0) ? 2.0 : local.punch_length as f32`.
4. `in_world = world.is_some()`.
5. Build `reply = GameUpdatePacket { packet_type: PingReply (0x15),
   target_net_id: hash_string(challenge.to_string()), value: time_val,
   vector_x: bx * 32.0, vector_y: by * 32.0, ..Default::default() }`.
6. If `in_world`: `reply.net_id = local.hack_type`; `reply.vector_x2 = local.velocity`;
   `reply.vector_y2 = local.gravity`.
7. `send_game_packet(&reply, true)` (**reliable**).

> `target_net_id` = `hash_string` of the **decimal string** of `challenge`, not the raw
> integer. All other fields default (0 / 0.0 / flags empty → no extra_data).

### 2.9 `on_call_function(&mut self, id: PeerID, extra_data: &[u8])` — CallFunction dispatch

1. `VariantList::deserialize(extra_data)`; on Err log `"[Bot] VariantList parse error: {e}"`
   and return.
2. `fn_name = vl.get(0).map(as_string).unwrap_or_default()`. log
   `"[Bot] CallFunction: {fn_name}"`.
3. `match fn_name.as_str()`:

**`"OnSendToServer"` — redirect capture (subserver handoff):**
- `port = vl.get(1).as_string().parse::<u16>().unwrap_or(0)`.
- `raw_token = vl.get(2).map(as_string).unwrap_or("0")`.
- `user_id = vl.get(3).map(as_string).unwrap_or("0")`.
- `server_str = vl.get(4).map(as_string).unwrap_or_default()`.
- `lmode = vl.get(5).as_string().parse::<i32>().unwrap_or(0)`.
- `tank_id_name = vl.get(6).map(as_string).unwrap_or_default()`.
- Split `server_str` on `|` into at most 3 parts (`splitn(3, '|')`):
  - `server = parts[0].trim_end()`.
  - `door_id = parts[1].trim_end()` if non-empty else `"0"`.
  - `raw_uuid = parts[2].trim_end()` or `""`.
- **Token resolution** (`match raw_token.trim().parse::<i64>()`):
  - `Ok(value)` with `value < 0` (a marker like `-1`): if `last_redirect_token` is `Some`,
    use the cached value and log
    `"[Bot] OnSendToServer -> using cached redirect token for marker {raw_token} lmode={lmode}"`;
    else log `"[Bot] OnSendToServer -> no cached redirect token for marker {raw_token} lmode={lmode}"`
    and use `raw_token` as-is.
  - Otherwise (`value >= 0`, or not an integer): if `raw_token.trim()` non-empty →
    `last_redirect_token = Some(raw_token.clone())`; use `raw_token`.
- **UUID resolution**: `uuid_marker = raw_uuid.trim()`; if `uuid_marker` is empty or `"-1"`:
  if `last_redirect_uuid` is `Some`, use it and log
  `"[Bot] OnSendToServer -> using cached redirect UUIDToken for marker {uuid_marker:?} lmode={lmode}"`;
  else log `"… no cached redirect UUIDToken …"` and use `raw_uuid`. Otherwise
  `last_redirect_uuid = Some(raw_uuid.clone())`; use `raw_uuid`.
- log `"[Bot] OnSendToServer → {server}:{port} door={door_id}"`.
- `redirect = Some(RedirectData { server, port, token, user: user_id, door_id, uuid,
  lmode: lmode.to_string(), tank_id_name })`.
- `redirect_attempts = 0`; `login_reject_streak = 0`; `login_throttle_streak = 0` (gateway
  accepted the logon → exit IP is fine).
- `host.peer_disconnect(id, 0)` — the Disconnect handler (§2.2 Branch A2) then connects to
  the subserver.

**`"OnSpawn"`:** `message = vl.get(1).as_string()`; `data = parse_pipe_map(&message)`.
- **If `data` contains key `"type"` → this is the local player (self):**
  - `local.net_id = data["netID"].parse().unwrap_or(0)`;
    `local.user_id = data["userID"].parse().unwrap_or(0)`.
  - `redirect = None`; `redirect_attempts = 0`; `refresh_token_on_reconnect = false`.
  - `clear_login_state_flags()` (§2.11) — login succeeded, wipe stale pending_* flags and
    reject/throttle streaks.
  - `connected_since = None` (login watchdog satisfied); `was_in_world = true` (a later drop
    = in-world session loss → §2.2 C2).
  - log `"[Bot] OnSpawn (self) net_id={net_id} user_id={user_id}"`.
  - log `"[Bot] ltoken string: {ltoken}|{rid}|{mac}|{wk}"`. **Adonai: redact — never log the
    raw ltoken to the shared console.**
  - `state.status = InGame`; emit `BotStatus{"in_game"}`.
- **Else (another player):** parse from `data`:
  - `posXY` → split on `|`, parse each as `f32`; `position = (parts[0]||0.0, parts[1]||0.0)`
    (**pixels**).
  - `net_id = data["netID"]||0`, `user_id = data["userID"]||0`,
    `m_state = data["mstate"]||0u32`, `invisible = (data["invis"] as u32 || 0) != 0`,
    `name = data["name"]||""`, `country = data["country"]||""`,
    `avatar/onlineID(eid: "eid")/ip/colrect/titleIcon` from their keys (`avatar`, `onlineID`,
    `eid`, `ip`, `colrect`, `titleIcon`), all default `""`.
  - log `"[Bot] OnSpawn player={name} net_id={net_id} pos=({x:.0},{y:.0})"`.
  - Insert `Player` into `players[net_id]`; rebuild `state.players` (map each to `PlayerInfo{
    net_id, name, pos_x = position.0/32.0, pos_y = position.1/32.0, country}`); emit
    `PlayerSpawn{ bot_id, net_id, name, country, x = pos.0/32.0, y = pos.1/32.0 }`.

**`"OnSetPos"`:** `(x, y) = vl.get(1).as_vec2()` (pixels). `pos_x = x`; `pos_y = y`; if
`pathfind_target.is_some()` → `pathfind_recalc = true`. Under lock: `state.pos_x = x/32.0`,
`state.pos_y = y/32.0`. log `"[Bot] OnSetPos → ({x}, {y})"`; emit `BotMove{ x/32.0, y/32.0 }`.

**`"OnSuperMainStartAcceptLogonHrdxs47254722215a"`** (subserver accepted logon — **exact
magic string**): `state.status = Connected`;
`wait_for_global_gate(&ENTER_GAME_GATE, ENTER_GAME_STAGGER_MS=2000, "enter_game")`;
`send_text("action|enter_game\n")`; emit `BotStatus{"connected"}`.

**`"OnRemove"`:** `data = parse_pipe_map(vl.get(1).as_string())`;
`net_id = data["netID"]||0`; `players.remove(&net_id)`; rebuild `state.players`; log
`"[Bot] OnRemove net_id={net_id}"`; emit `PlayerLeave{ net_id }`.

**`"OnSetBux"`:** `gems = vl.get(1).as_int32()`; `inventory.add_gems(gems)`;
`state.gems = gems`; emit `BotGems{ gems }`.

**`"OnConsoleMessage"`:** `message = vl.get(1).as_string()`;
`sync_geiger_state_from_console(&message)` (geiger module); `log_console(message)` (logs the
raw server console line).

**`"OnDialogRequest"`:** `message = vl.get(1).as_string()`; log `"[Bot] Dialog: {message}"`;
take the one-shot `temporary_data.dialog_callback` (lock, `.take()`); if `Some(cb)` →
`cb(self)`. (Adonai: a `std::function<void(Bot&)>` guarded by a mutex; fire-once.)

**`"SetHasGrowID"`:** if `vl.get(2)` present → `growid = it.as_string()`;
`username = growid`; `state.username = growid`; emit `BotUsername{ username: growid }`.
(Note: uses arg **index 2**, not 1.)

**`"OnRequestWorldSelectMenu"`:** `world = None`; `pathfind_target = None`;
`pathfind_recalc = false`; under lock: `state.world_name = "EXIT"`, `state.status = InGame`,
and `removed = inventory.remove_temp_items()`; if `removed` → `emit_inventory_update()`; emit
`BotStatus{"in_game"}`, `BotWorld{"EXIT"}`, `WorldLoaded{ name:"EXIT", width:0, height:0,
tiles:[] }`; log `"[Bot] OnRequestWorldSelectMenu → cleared world"`.

**`_` (any other function name):** no-op.

### 2.10 `build_redirect_packet(&self, r: &RedirectData) -> String` — subserver logon body

Sent after a **redirect** ServerHello. Built by string concatenation in this exact order
(every line ends `\n`):
```
tankIDName|{r.tank_id_name}
tankIDPass|
requestedName|
f|1
protocol|211                 ← literal 211, NOT PROTOCOL
game_version|{game_version}
fz|{fz}
cbits|{cbits}
player_age|{player_age}
GDPR|{gdpr}
FCMToken|
category|{category}
totalPlaytime|{total_playtime}
klv|{klv}
hash2|{hash2}
meta|{meta}
fhash|{FHASH}
rid|{rid}
platformID|{platform_id}
deviceVersion|0
country|{country}
hash|{hash}
mac|{mac}
wk|{wk}
zf|{zf}
lmode|{r.lmode}
user|{r.user}
token|{r.token}
UUIDToken|{r.uuid}
doorID|{r.door_id}          ← ONLY if r.door_id is non-empty
aat|2
```
`doorID|…` is appended **only when `r.door_id` is not empty**. `aat|2` is always last.
Note this packet has no `steamToken`, no `platformID`-after-rid quirk differences vs the
gateway packet — reproduce line-for-line.

### 2.11 `clear_login_state_flags(&mut self)`
Sets all eight `pending_*` flags to `false` and zeroes `login_reject_streak`,
`login_throttle_streak`, `pre_hello_disconnects`. Called on successful in-world spawn.

### 2.12 Gate helpers (fleet pacing) — see also §4

**`reserve_gate_slot(gate, spacing_ms, max_ahead_ms) -> Instant`** (pure reservation):
Lock `gate`; `now = Instant::now()`; `horizon = now + max_ahead_ms`;
`slot = (*next_allowed).clamp(now, horizon)`;
`*next_allowed = min(slot + spacing_ms, horizon)`; unlock; return `slot`. The lock is held
**only** for the reservation, never during the wait. `max_ahead_ms` bounds the backlog:
under saturation the k-th caller is never told to wait past `horizon`, and `next_allowed` is
pinned there.

**`wait_for_global_gate(&mut self, gate, spacing_ms, label)`** (connected-phase, keeps ENet
alive): `slot = reserve_gate_slot(gate, spacing_ms, GATE_CONNECTED_MAX_AHEAD_MS=2500)`; if
`slot <= now` return; compute `waited_ms`; if `waited_ms >= 50` log
`"[Bot] login pacing: waiting ~{waited_ms}ms before {label} (keeping ENet serviced)"`.
**Critical:** the wait is spent holding a live ENet connection, so it must keep servicing:
- If `in_gate_wait` (re-entrant) → plain `sleep(waited_ms)` and return.
- Else set `in_gate_wait = true`; loop while `now < slot`: if `stop_requested` or
  `stop.load()` break; `service_once()`; `sleep(10ms)`. Then `in_gate_wait = false`.

**`wait_global_gate(gate, spacing_ms) -> u64`** (dead-sleep, pre-connection): reserve with
`GATE_HTTP_MAX_AHEAD_MS=300_000`; if `slot > now`, sleep the delta and return it, else 0.
Wrapped by:
- `pace_http_login()` → `wait_global_gate(&HTTP_LOGIN_GATE, HTTP_LOGIN_STAGGER_MS)`.
- `pace_dashboard()` → `wait_global_gate(&DASHBOARD_GATE, DASHBOARD_STAGGER_MS)`.

**`reserve_throttle_slot(gate, cooldown_ms, spacing_ms) -> Instant`** (post-throttle
scheduler): lock; `now`; `floor = now + cooldown_ms`; `horizon = floor + 60s`;
`base = (*next_allowed).clamp(floor, horizon)`;
`*next_allowed = min(base + spacing_ms, horizon)`; return `base`.

### 2.13 `schedule_reconnect(&mut self, reason, refresh_token, base_ms)`
- `streak = max(login_reject_streak, pre_hello_disconnects as u32).min(8)`.
- `backoff = base_ms * max(streak, 1)` (saturating).
- `jitter = ((bot_id * 137) + (streak * 251)) % 1000`.
- `delay_ms = min(backoff + jitter, 30_000)`.
- `refresh_token_on_reconnect |= refresh_token`.
- `reconnect_after = now + delay_ms`.
- log `"[Bot] {reason} - reconnecting in {delay_ms}ms"`.

### 2.14 `reconnect_main(&mut self, refresh_token)` — gateway reconnect (context)

Always reconnects to the **gateway** (`server_addr`) and re-sends `protocol|225` with the
`ltoken`, so it leaves from the **pinned bypass IP** (the token's minting IP). Only redirects
use the game proxy. Steps:
1. `host = create_host(bypass_enet.or(proxy))`; `peer_id = None`; `saw_server_hello = false`.
2. If `refresh_token`: `server_addr = None`; `refresh_token()` (may re-pin `bypass_enet` and
   set `server_addr`); if `server_addr` now `Some(addr)`: rebuild host on
   `bypass_enet.or(proxy)`; `saw_server_hello=false`; log; `host.connect(addr,2,0)`; return.
3. Else: log `"[Bot] reconnect: reusing current login token"`; if `server_addr` set → connect
   and return.
4. If `stop_requested || stop.load()` → log abort and return (avoids an undead thread when
   `refresh_token()` set `stop_requested` for terminal cases).
5. Otherwise fetch fresh `server_data`: build `LoginInfo{ protocol: PROTOCOL, game_version:
   GAME_VER }`; assemble proxy candidates (rotating login proxy + alt-scheme fallback for
   http(s) only; else assigned game proxy; else `direct`); `pace_http_login()`; loop trying
   `get_server_data_proxied(alternate ∈ {false,true}, &login_info, proxy_url)` — on a 403
   (`is_http_403_text`) skip the growtopia2 alternate for that candidate; on total failure
   sleep 5 s and retry (honoring stop). On success set `self.meta`, parse
   `"{server}:{port}"` → `server_addr`, connect; on bad address set `reconnect_after = now +
   10s`.

`refresh_token()` (context): tries the login-method-specific fallback (`Legacy`/`Newly`/
`Requestly` → `pace_http_login()` then `fetch_*_credentials(...)` → `apply_credentials`;
`HarToken` → re-extract from the `.har`; `Ltoken` → no fallback, `stop_requested = true`).
The `check_token` loop is **disabled** (commented out) — do not re-enable it in Adonai unless
asked.

### 2.15 `create_host(proxy: Option<&Socks5Config>) -> BotHost` (context)

ENet `HostSettings`: `peer_limit=1`, `channel_limit=2`, `compressor=RangeCoder`,
`checksum=crc32`, `using_new_packet=true`.
- With a proxy: log `"[Bot] Connecting via proxy {url}"`; up to **4 attempts** to
  `Socks5UdpSocket::bind_through_proxy(0.0.0.0:0, proxy_addr, user, pass)` then
  `enet::Host::new(socket, settings)` → `BotHost::Socks5`; sleep 300 ms between attempts.
  After 4 failures do **NOT** fall back to direct (would leak the real IP); instead bind a
  dead loopback socket (`127.0.0.1:0`) as `BotHost::Direct` so the connect simply fails and
  the run loop retries next cycle. log the "NOT falling back to direct" warning.
- Without a proxy: bind `0.0.0.0:0` → `BotHost::Direct`.

> **Adonai:** the vendored C ENet must be patched for SOCKS5-UDP (the `Socks5UdpSocket`
> equivalent). Preserve the 4-retry / 300 ms / no-direct-fallback policy — it is a
> real-IP-leak safeguard.

### 2.16 Logging / traffic / redaction helpers

- `log_console(&self, msg)`: if `msg` starts with `"[Bot] "`, rewrite to `"[Bot#{bot_id}] "`.
  Always `logger::log(&msg)` (file). If `is_high_frequency_noise(&msg)` → return (file-only).
  Else push to `state.console` (cap 100, drop oldest) and emit `Console{ bot_id, message }`.
- `is_high_frequency_noise(msg)`: strip the `[Bot…] ` tag (`bot_msg_body`), true if the body
  starts with `"GameUpdatePacket"`, `"CallFunction: OnClearItemTransforms"`, or
  `"PingReply sent"`.
- `emit_traffic(direction, kind, size, detail)`: `detail = truncate_text(&detail, 6000)`;
  `summary = summarize_detail(&detail)` (first non-empty line, trimmed, truncated to 140);
  emit `Traffic{ bot_id, direction, kind, size, summary, detail, timestamp_ms: now_millis() }`.
- `truncate_text(v, max_chars)`: if char-count ≤ max, return as-is; else first `max_chars`
  chars + `"\n...<truncated>"`.
- `redact_packet_text(text)`: per line, if it has a `|`, lowercase the trimmed key; if the
  key ∈ {`token`, `ltoken`, `ubiticket`, `tankidpass`, `password`, `steamtok`, `steamtoken`,
  `fcmtoken`} → replace the value with `"{key}|<redacted>"`; else keep the line. Join with
  `\n`. **Adonai must apply this to every logged/emitted packet body.**
- `emit(event)`: `if let Some(tx) = ws_tx { tx.send(event) }`. **Adonai:** push to the ImGui
  event sink / fleet event bus instead of a WebSocket broadcaster.
- `now_millis()`: `SystemTime::now()` since UNIX_EPOCH as `u64` ms (saturating), 0 on error.

---

## 3. THREADING & SHARED STATE

- **One OS thread per bot.** `run()` owns the `Bot` and runs the ~10 ms loop until stop.
  `service_once()` is called only from that thread (and re-entrantly, but guarded, from
  `wait_for_global_gate`/`sleep_ms` which still run on the same thread). C++: `std::thread`
  per bot, owned by a fleet manager.
- **`state: Arc<RwLock<BotState>>`** is the only cross-thread mirror of a bot: the UI/fleet
  layer reads it; the bot thread writes it under short-lived write locks (always via
  `unwrap_or_else(PoisonError::into_inner)` — i.e. **ignore lock poisoning and proceed**).
  C++: `std::shared_ptr<BotState>` guarded by a `std::shared_mutex`; keep critical sections
  tiny (the Rust code drops the guard before emitting events — replicate that to avoid
  holding the lock across the event sink).
- **Fleet-wide static gates** (`LOGIN_PACKET_GATE`, `ENTER_GAME_GATE`, `HTTP_LOGIN_GATE`,
  `WARP_GATE`, `GATEWAY_LOGON_GATE`, `DASHBOARD_GATE`) are **process-global** and shared by
  every bot thread. They are the mechanism by which bots are "aware of each other": they
  serialize logins/warps/dashboard POSTs so a bulk spawn doesn't stampede the shared login
  IP. This is the fleet coordination surface — in Adonai these become
  `std::mutex`-guarded `steady_clock::time_point` singletons. Do **not** make them per-bot;
  correctness of the whole fleet's login pacing depends on them being shared.
- **`stop: Arc<AtomicBool>`** and **`script_stop: Arc<AtomicBool>`** — shared atomics; C++
  `std::atomic<bool>` (relaxed loads match the Rust `Ordering::Relaxed`).
- **Channels:** `cmd_rx` (UI→bot commands), `event_tx`/`script_req_rx`/`script_reply_tx`
  (bot↔script), `ws_tx` (bot→UI). The Rust ones are `crossbeam_channel`. Adonai maps every
  channel to the **std::mutex + condvar bounded queue** described in the porting brief:
  - `cmd_rx` → command queue drained non-blocking each tick (`try_recv` loop).
  - `event_tx` → native automation event queue (only if Adonai keeps a scripting hook;
    otherwise delete the `try_send(BotEventRaw::…)` calls). **No Lua.**
  - `ws_tx` → in-process event bus feeding the Dear ImGui UI; `emit`/`log_console`/
    `emit_traffic` become pushes onto that bus. **No web server / no Axum.**
- **Re-entrancy guard `in_gate_wait`** prevents `service_once` (invoked during a gate wait)
  from recursing into another gate wait. Preserve it (a plain `bool` on the bot; the bot is
  single-threaded so no atomic needed).

---

## 4. DEPENDENCY MAPPING (Rust crate → Adonai C++)

| Rust (this module) | Purpose here | Adonai C++ |
|---|---|---|
| `rusty_enet` (`enet::Host`, `Packet`, `PeerID`, `EventNoRef`, `RangeCoder`, `crc32`) | ENet transport; range coder + crc32 checksum; `using_new_packet` | **Vendored C ENet**, patched for SOCKS5-UDP. Keep `peer_limit=1`, `channel_limit=2`, range coder compressor, crc32 checksum, new-packet mode. |
| `crate::socks5::Socks5UdpSocket` | SOCKS5 UDP-ASSOCIATE socket under ENet | libcurl is HTTP-only — this is a **custom SOCKS5-UDP** socket layer feeding ENet. Port the socks5 module natively (see its own spec); libcurl (`socks5h`) is only for the HTTP login side. |
| `crossbeam_channel` | cmd/event/script/ws channels | `std::mutex + std::condition_variable` bounded queue (per brief). |
| `serde_json` (elsewhere in login) | JSON for HTTP login responses | `nlohmann/json`. **Not** used for the ENet wire format. |
| `md5` (via `compute_klv`) | klv/hash computation | bundled md5. |
| `argon2` (login module) | password hashing on some login paths | argon2 lib (login module, not this one). |
| `ureq`/`reqwest` (login/server_data) | HTTP login, server_data.php, dashboard | libcurl with `socks5h://` + cookie jar (login module). |
| `mlua` (`event_tx`/script channels) | Lua script engine | **None — no Lua.** Delete the script VM; native automation hooks if any. |
| `tokio`/`axum`/`tower` (web layer) | async web dashboard | **None — no web server.** Dear ImGui native UI + `std::thread`. |
| `scraper` (login HTML) | parse dashboard HTML | regex / manual HTML scan (login module). |
| `bitflags` (`PacketFlags`) | packet flag bitset | plain `uint32_t` with named constants; test `EXTENDED` = `&0x8`. |
| `rand` (crypto rid/mac) | random ids | `<random>` (identity module). |
| `std::sync::{RwLock,Mutex,OnceLock,Arc,AtomicBool}` | shared state / gates | `std::shared_mutex`, `std::mutex`, function-local `static` + `std::once_flag`, `std::shared_ptr`, `std::atomic<bool>`. |

---

## 5. RENAME RULES (Mori→Adonai, Cloei→North)

Apply globally in the port. Every identifier / path / log line / window title / user-agent /
config filename bearing **Mori/mori** → **Adonai/adonai**; every **Cloei/cloei** → **North/north**.

Concrete occurrences spotted in and around this module:

- **Source path / crate name:** `Mori-2.0.0/src/bot/core.rs` → `adonai/.../bot/core.*`; the
  crate/namespace `mori` → `adonai`.
- **Doc comment (line ~167):** `/// Original Mori-style GrowID login without HAR fallbacks.`
  → rewrite as "Original Adonai/Newly-style GrowID login…". (Only occurrence of the literal
  word "Mori" in this file's range.)
- **No "Cloei"/"cloei" appears in this specific file range**, but the fleet-wide rule stands:
  any upstream-author references elsewhere (repo name, credits, HAR sample filenames,
  user-agent strings referencing `cloei`) → `North`/`north`. (Per memory: mirror the upstream
  cloei/mori 22-field login POST — that reference to the upstream project name must become
  `north` in Adonai comments/identifiers, while the field set itself stays as-is.)
- **Log-line tags:** all console lines here use the neutral tag `"[Bot]"` / `"[Bot#{id}]"`,
  **not** "Mori" — keep them as `[Bot]`/`[Bot#id]` (no rename needed, but if Adonai brands the
  logger prefix, use "Adonai", never "Mori").
- **User-facing strings that a brand string could leak into:** none hardcode "Mori" in this
  range, but the `println!("=== RAW LOGIN PACKET ===")` and any window title / user-agent set
  in the app shell must read **Adonai**. Ensure the HTTP user-agent used by the login module
  (not this file) is `Adonai/…`, never `Mori/…`.
- **Config / data filenames:** `requestly_logs.har` (referenced in the `Requestly` doc
  comment) is a Growtopia/Requestly artifact name, **not** a Mori brand — keep it. Any
  Adonai-owned config file that would have been `mori.*`/`mori_config.*` → `adonai.*`.
- **The magic protocol string `OnSuperMainStartAcceptLogonHrdxs47254722215a` is a
  Growtopia server token — DO NOT rename.** Likewise `action|enter_game`, `action|logon_fail`,
  `action|request_token`, `OnSendToServer`, `OnSpawn`, field names (`ltoken`, `tankIDName`,
  `UUIDToken`, `aat`, etc.), and all numeric constants (`PROTOCOL=226`, `protocol|211`,
  `GAME_VER="5.51"`, `FHASH`, item ids) are **protocol constants**, not brand names — keep
  them exactly.

---

## 6. Implementation checklist (Adonai `bot/core`)

1. `EnetHost` wrapper over vendored C ENet (direct + SOCKS5-UDP), error-swallowing
   `next_event`, `peer_set_timeout(0,12000,30000)` on connect.
2. `GameUpdatePacket` 56-byte LE (de)serializer with the EXTENDED/extra_data rule; the outer
   `IncomingPacket::parse` with the `0x00 || >=0x80` string terminator; `make_*_packet`
   builders.
3. `Variant`/`VariantList` native binary decoder (spec 06) with `as_string/as_int32/as_vec2`.
4. `hash_string` (rotl-5 + trailing NUL, reinterpret u32→i32) and `parse_pipe_map`.
5. The `Bot` struct with every field in §1.5; fleet-wide gate singletons in §0.3.
6. `service_once` with the exact Connect / Disconnect state machine (§2.2) and Receive
   dispatch, including the full `logon_fail` pending-flag priority ladder.
7. `on_server_hello`, `build_login_packet` (3 forms + `login_token_field`),
   `build_redirect_packet` (protocol|211, conditional doorID), `on_ping_request`,
   `on_call_function` dispatch (all arms in §2.9), `send_text/send_game_message/send_game_packet`.
8. Gate helpers (`reserve_gate_slot`, `wait_for_global_gate`, `reserve_throttle_slot`,
   `pace_http_login`, `pace_dashboard`), `schedule_reconnect`, `reconnect_main`,
   `clear_login_state_flags`, `create_host`.
9. Redaction (`redact_packet_text`) applied to every logged/emitted packet body and to the
   `ltoken` OnSpawn line; high-frequency noise filter for the console/event bus.
10. All log strings, status enums, and emitted events wired to the ImGui event bus (no web
    server, no Lua). Rename per §5.
