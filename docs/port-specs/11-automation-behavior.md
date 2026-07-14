# Port Spec 11 — Automation & Behavior (`lua/*`)

**Adonai C++ port — single source of truth for this module.**
Ported from Mori `src/lua/{runtime.rs, webhook.rs, http.rs, geiger_stats.rs, types.rs, mod.rs}`.

> **CRITICAL FRAMING FOR ADONAI.** In Mori this entire module is a **Lua scripting runtime**: each bot spawns an `mlua` VM, exposes a `bot` userdata + helper globals, and users write Lua scripts (geiger farming, harvesting, webhook reporting). **Adonai has NO Lua.** Therefore this module is NOT ported as a scripting host. Instead:
>
> 1. The **API surface exposed to Lua** (the `bot:*` methods, `getWorld`, `getInventory`, `http.*`, `Webhook`, `geiger_stats.*`, `rotation.*`) becomes the **native C++ engine/automation API** — plain C++ methods on the bot/engine objects.
> 2. The **Lua library code embedded in `runtime.rs`** (the `autogeiger.*` and `rotation.*` functions loaded via `lua.load(...)`) is the actual **automation behavior**. Each of those becomes a **native C++ automation module** that drives the bot through the same underlying operations. These are the "what the automation does" blueprints. See §6.
> 3. `geiger_stats` and `rotation_pool` and the webhook message-id map are **process-global shared state across all bots**. In Adonai these become part of the fleet-wide **`FleetState`** singleton so bots coordinate. See §5.
>
> Everything Lua-specific (VM, instruction hooks, `sleep` as a Lua function, event dispatch tables, `read`/`write`/`append` file globals) is replaced by native C++ threading and direct calls. Those Lua mechanics are still documented below because they define exact behavior (poll intervals, stop semantics, defaults) that the native modules must reproduce.

---

## 0. File / module map

| Rust file | Responsibility | Adonai target |
|---|---|---|
| `lua/mod.rs` | Module glue; re-exports `run_script_threaded` | (dropped — no script host) |
| `lua/types.rs` | `BotProxy` (request/reply bridge) + all `Lua*` wrapper structs | Native engine handle + view structs (§1, §2) |
| `lua/runtime.rs` | Registers the whole API; embeds the automation Lua library | Native automation API + automation modules (§6) |
| `lua/http.rs` | Generic HTTP helper `http.get/post/put/delete/request` | `Adonai::Http` libcurl helper (§3) |
| `lua/webhook.rs` | Discord webhook object + fleet-shared single-message updater | `Adonai::Webhook` module (§4) |
| `lua/geiger_stats.rs` | Process-global fleet geiger find counters, disk-persisted | `FleetState::geiger_stats` (§5) |

`mod.rs` in full:
```rust
mod geiger_stats; mod http; mod runtime; mod types; mod webhook;
pub use runtime::run_script_threaded;
```
Only `run_script_threaded` is public. Everything else is `pub(super)` / private.

---

## 1. The bot ↔ automation bridge (`BotProxy`, request/reply channel)

This is the **most important interface** to port: it is the complete set of operations automation can perform against a bot. In Mori it is a synchronous request/reply over `crossbeam-channel`. In Adonai it becomes **direct method calls** on the bot object (same thread as the automation, or a mutex-guarded call into the net thread) — but the *operation set and semantics* below are the contract.

### 1.1 `BotProxy` (types.rs)
```rust
pub(super) struct BotProxy {
    pub req_tx:  crossbeam_channel::Sender<ScriptRequest>,
    pub reply_rx:crossbeam_channel::Receiver<ScriptReply>,
    pub state:   Arc<RwLock<BotState>>,
}
impl BotProxy {
    pub fn request(&self, req: ScriptRequest) -> ScriptReply {
        self.req_tx.send(req).expect("bot thread gone");
        self.reply_rx.recv().expect("bot thread gone");  // blocks until reply
    }
}
```
- `request()` is **fully synchronous**: send one request, block for exactly one reply. Panics (`"bot thread gone"`) if the channel is closed.
- `state` is a **shared `RwLock<BotState>`** the automation reads directly (bypassing the channel) for hot fields (name, status, pos, gems, ping, mac, geiger_signal, console).

**Adonai mapping:** `BotProxy` → a `Bot*` handle. `request(req)` → the corresponding method call, executed synchronously. `state` (RwLock) → a `std::shared_mutex`-guarded `BotState` the automation reads directly. Two distinct access paths in Mori (channel vs. direct state read) can collapse to one in Adonai (direct calls under the bot's lock), but preserve the read/write distinction for hot fields to avoid contending the command path.

### 1.2 `ScriptRequest` / `ScriptReply` — the full operation set

> Defined in `crate::script_channel` (not in these files). Reconstructed **exactly from every call site** in `runtime.rs`. This is the C++ engine API to implement. Each row: Lua-facing name → request variant (+ params) → reply variant consumed (+ default on wrong/absent reply).

**State getters/setters (via channel):**

| API method | Request | Params | Reply consumed | Default if mismatch |
|---|---|---|---|---|
| `auto_collect` (get) | `GetAutoCollect` | — | `Bool(v)` | `false` |
| `auto_collect` (set) / `setAutoCollect` | `SetAutoCollect` | `enabled: bool` | (ignored) | — |
| `place_delay` (get) | `GetPlaceDelay` | — | `U32(v)` | `500` |
| `place_delay` (set) | `SetPlaceDelay` | `ms: u64` | (ignored) | — |
| `walk_delay` (get) | `GetWalkDelay` | — | `U32(v)` | `500` |
| `walk_delay` (set) | `SetWalkDelay` | `ms: u64` | (ignored) | — |

**World / inventory / local snapshots:**

| Method | Request | Params | Reply | Notes |
|---|---|---|---|---|
| `getWorld` | `GetWorld` | — | `World(Option<WorldSnapshot>)` | `None` → Lua `nil`. Snapshot fields below. |
| `getInventory` | `GetInventory` | — | `Inventory(Inventory)` | fallback `Inventory::default()` |
| `isWearing` | `IsWearing` | `item_id: u16` | `Bool` | default `false` |
| `getLocal` | `GetLocal` | — | `Local(LocalSnapshot)` | on non-`Local` reply → **error** `"getLocal failed"` |
| `isInWorld` | `IsInWorld` | `name: Option<String>` | `Bool` | default `false` |
| `isInTile` | `IsInTile` | `x: u32, y: u32` | `Bool` | default `false` |

`WorldSnapshot` (fields consumed to build `LuaWorld`): `world: World`, `players: Vec<Player>`, `local_net_id: u32`, `local_user_id: u32`, `local_name: String`, `local_pos: (f32,f32)`.
`LocalSnapshot` (fields consumed to build a `Player`): `net_id: u32`, `user_id: u32`, `username: String`, `pos_x: f32`, `pos_y: f32`. (The rest of `Player` is zero-filled — see §2.6.)

**Network:**

| Method | Request | Params | Reply |
|---|---|---|---|
| `connect` | `Reconnect` | — | (ignored; returns `true`) |
| `disconnect` | `Disconnect` | — | (ignored) |
| `sendRaw` | `SendRaw` | `pkt: GameUpdatePacket` | (ignored) |
| `sendPacket` | `SendPacket` | `ptype: u8, text: String` | (ignored) |

**World actions:**

| Method | Request | Params |
|---|---|---|
| `warp` | `Warp` | `name: String, id: String` (id defaults `""` if nil) |
| `say` | `Say` | `text: String` |
| `leaveWorld` | `LeaveWorld` | — |
| `respawn` | `Respawn` | — |
| `active` | `Active` | `tile_x: i32, tile_y: i32` |
| `place` | `Place` | `x: i32, y: i32, item: u32` |
| `hit` | `Hit` | `x: i32, y: i32` |
| `wrench` | `Wrench` | `x: i32, y: i32` |
| `wrenchPlayer` | `WrenchPlayer` | `net_id: u32` |
| `enter` | `Enter` | `pass: Option<String>` |

**Inventory actions:**

| Method | Request | Params | Note |
|---|---|---|---|
| `wear` | `Wear` | `item_id: u32` | |
| `unwear` | `Unwear` | `item_id: u32` | |
| `use` | `Wear` | `item_id: u32` | **alias of wear** (same request) |
| `drop` | `Drop` | `item_id: u32, count: u32` | |
| `trash` | `Trash` | `item_id: u32, count: u32` | |
| `fastDrop` | `FastDrop` | `item_id: u32, count: u32` | |
| `fastTrash` | `FastTrash` | `item_id: u32, count: u32` | |

**Movement** (all → `Walk { tile_x: i32, tile_y: i32 }`):

| Method | Target tile computed |
|---|---|
| `moveTo(dx,dy)` | current `pos_x+dx, pos_y+dy` (reads state, casts pos to i32) |
| `moveTile(x,y)` | absolute `x, y` |
| `moveLeft(range?)` | `pos_x - range, pos_y` (range default `1`) |
| `moveRight(range?)` | `pos_x + range, pos_y` |
| `moveUp(range?)` | `pos_x, pos_y - range` |
| `moveDown(range?)` | `pos_x, pos_y + range` |
| `setDirection(facing_left)` | `SetDirection { facing_left: bool }` |
| `setMac(mac)` | `SetMac { mac: String }` |

**Pathfinding / collect:**

| Method | Request | Params | Reply | Behavior |
|---|---|---|---|---|
| `getPath` | `GetPath` | `x: u32, y: u32` | `Path(Vec<(u32,u32)>)` | returns Lua array of `{x=,y=}` (1-indexed); on mismatch → empty |
| `findPath` | `FindPath` | `x: u32, y: u32` | (ignored) | fire-and-forget walk-to |
| `collectObject` | `CollectObject` | `uid: u32, range: f32` | (ignored) | (Lua arg name `oid` → field `uid`) |
| `collect` | `Collect` | `range: f32, interval_ms: u64` | `CollectCount(usize)` | returns count collected; default `0` |

**Misc:** `getConsole` → wraps `state` (no request). `getLogin` → `{mac}` from state. `getPing` → `state.ping_ms`. `getSignal` → `state.geiger_signal` (Option). `getCaptcha` → always `nil`. `stopScript` → no-op. `hasAccess` → always `false`. `getNPC`/`getNPCs` → `nil`/`{}` (stubs).

**`ScriptReply` variants observed:** `Bool(bool)`, `U32(u32)`, `World(Option<WorldSnapshot>)`, `Inventory(Inventory)`, `Local(LocalSnapshot)`, `Path(Vec<(u32,u32)>)`, `CollectCount(usize)`. (Plus a catch-all for "ignored" replies.)

> **Adonai:** implement each of these as a bot method with the exact param types and semantics. Movement helpers read live position under the state lock, compute the tile, and issue a `Walk`. `use` is deliberately identical to `wear`. `getLocal` failing is a hard error to callers; the rest degrade to defaults.

---

## 2. TYPES — view/wrapper structs (types.rs + userdata field maps)

These `Lua*` wrappers are thin views over engine data exposed with **exact field/method names**. In Adonai these are the **read model** the automation and UI consume. Keep the public accessor names (they are the automation's vocabulary). Field name on the left is the **exposed** name; right is the underlying source.

### 2.1 `LuaWorld` (wraps `World` + player list + local snapshot)
Fields: `name`=`world.tile_map.world_name`, `x`=`tile_map.width`, `y`=`tile_map.height`, `tile_count`=`tiles.len()`, `version`=`world.version`, `public`=`(world.flags & 1) != 0`, `tiles`=array of `LuaTile`, `objects`=array of `LuaNetObject`, `players`=array of `LuaPlayer`.
Methods:
- `getTile(x:u32,y:u32)` → `world.get_tile(x,y)` (Option).
- `getTiles()` / `getTilesSafe()` → full tile array (identical implementations).
- `getObject(oid:u32)` → first object with `uid==oid`.
- `getObjects()` → all objects.
- `getPlayer(key)` → if int/number: match `net_id==key as u32`; if string: match `name.to_lowercase()==key.to_lowercase()`.
- `getPlayers()` → all players.
- `getLocal()` → synthesize a `Player` from `local_net_id/local_user_id/local_name/local_pos` (rest empty — see §2.6).
- `isValidPosition(x:i32,y:i32)` → `x>=0 && y>=0 && x<width && y<height`.
- `getTileParent(tile)` → if `tile.flags` has `HAS_PARENT`, return `tiles[tile.parent_block]`.
- `hasAccess()` → always `false` (stub).

### 2.2 `LuaInventory` (wraps `Inventory`)
Fields: `itemcount`=`item_count`, `slotcount`=`size`, `items`=array of `LuaInventoryItem` (from `items.values()`).
Methods: `getItem(id)` (int/number→`u16`; string→parse to u16, else 0) → item by key; `getItems()`; `findItem(id:u16)` → `amount as u32` or `0`; `getItemCount(id:u16)` → same as findItem; `isActive(id:u16)`→`inv.is_active(id)`; `canCollect(id:u16)`→`inv.can_collect(id)`.

`Inventory` shape (inferred): `item_count`, `size`, `items: Map<u16, InventoryItem>`, plus `is_active(u16)->bool`, `can_collect(u16)->bool`.

### 2.3 `LuaInventoryItem` (wraps `InventoryItem`)
Fields: `id`=`item.id (u16)`, `count`=`item.amount`, `isActive`=`(item.flag & 1) != 0`.
`InventoryItem` fields used: `id: u16`, `amount`, `flag: u8/u16`.

### 2.4 `LuaTile` (wraps `Tile`)
Fields: `fg`/`foreground`=`fg_item_id`, `bg`/`background`=`bg_item_id`, `x`, `y`, `flags`=`flags_raw (u16)`, `parent`=`parent_block`.
Methods:
- `hasExtra()` → `flags` has `HAS_EXTRA_DATA`.
- `getExtra()` → nil if no extra data; else a table by `tile_type`:
  - `Sign{label}` → `{type="sign", label}`
  - `Door{label,flags}` → `{type="door", label, flags}`
  - `Lock{settings,owner_uid,access_count,..}` → `{type="lock", settings, owner_uid, access_count}`
  - `Seed{age,item_on_tree}` → `{type="seed", time_passed=age, item_on_tree}`
  - `Mannequin{label,hat,shirt,pants,boots,face,hand,back,hair,neck,..}` → `{type="mannequin", label, hat, shirt, pants, boots, face, hand, back, hair, neck}`
  - `WeatherMachine{settings}` → `{type="weather_machine", settings}`
  - `Dice{symbol}` → `{type="dice", symbol}`
  - anything else → `{type="unknown"}`
- `canHarvest()` → true iff `tile_type == Seed{ item_on_tree > 0, .. }` (**this is the "ready tree" test used by geiger/rotation harvesting**).
- `hasFlag(flag:u16)` → `(flags_raw & flag) != 0`.

`TileFlags` bits referenced: `HAS_PARENT`, `HAS_EXTRA_DATA`. `TileType` enum variants as listed above.

### 2.5 `LuaNetObject` (wraps `WorldObject` = dropped item on ground)
Fields: `id`=`item_id`, `x`, `y`, `count`, `flags`, `oid`=`uid`. (These are the collectible ground objects the `collect`/`collectObject` API targets.)

### 2.6 `LuaPlayer` (wraps `Player`)
Fields: `name`, `country`, `netid`=`net_id`, `userid`=`user_id`, `posx`=`position.0`, `posy`=`position.1`, `avatarFlags`=`m_state`, `roleicon`=`title_icon`.
Full `Player` struct fields (from synthesis sites): `net_id:u32, user_id:u32, name:String, country:String, position:(f32,f32), avatar:String, online_id:String, e_id:String, ip:String, col_rect:String, title_icon:String, m_state:u32/i32, invisible:bool`. When synthesized from a local snapshot, only net_id/user_id/name/position set; strings empty, `m_state=0`, `invisible=false`.

### 2.7 `LuaItemInfo` (wraps `ItemInfo` from items.dat)
Fields: `id`, `name`, `action_type`, `collision_type`, `clothing_type`, `rarity`, `grow_time`, `drop_chance`, `texture`=`texture_file_name`, `texture_hash`, `texture_x`, `texture_y`, `seed_color`=`base_color`, `seed_overlay_color`=`overlay_color`, `null_Item`=`name.to_lowercase().contains("null")`, `strength`=`block_health`.

### 2.8 `LuaGameUpdatePacket` (wraps `GameUpdatePacket`) — read/write field map
Every field is get+set. Exposed name → struct field → type:

| Exposed | Field | Type | Notes |
|---|---|---|---|
| `type` | `packet_type` | u8 | get `as_u8()`; set `GamePacketType::from(v)` |
| `object_type` | `object_type` | u8 | |
| `count1` | `jump_count` | u8 | |
| `count2` | `animation_type` | u8 | |
| `netid` | `net_id` | u32 | |
| `item` | `target_net_id` | i32 | |
| `flags` | `flags` | u32 | get `.bits()`; set `PacketFlags::from_bits_retain(v)` |
| `float_var` | `float_variable` | f32 | |
| `int_data` | `value` | u32 | |
| `vec_x` / `pos_x` | `vector_x` | f32 | two aliases, same field |
| `vec_y` / `pos_y` | `vector_y` | f32 | |
| `vec2_x` / `pos2_x` | `vector_x2` | f32 | |
| `vec2_y` / `pos2_y` | `vector_y2` | f32 | |
| `particle_rotation` | `particle_rotation` | f32 | |
| `int_x` | `int_x` | i32 | |
| `int_y` | `int_y` | i32 | |

Constructor: `GameUpdatePacket.new()` → `GameUpdatePacket::default()`. (Wire layout of `GameUpdatePacket` is specified in the packet port-spec — not here.)

### 2.9 `LuaVariant` / `LuaVariantList`
`LuaVariant` methods: `getType()` → **1**=Float, **2**=String, **3**=Vec2, **4**=Vec3, **5**=Unsigned, **9**=Signed, **0**=Unknown. `getString()`=`as_string()`, `getInt()`=`as_int32()`, `getFloat()` (0.0 if not Float), `getVector2()`→`{x,y}` via `as_vec2()`, `getVector3()`→`{x,y,z}` (0,0,0 if not Vec3), `print()`=`as_string()`.
`LuaVariantList` methods: `get(idx:usize)` → `LuaVariant` Option; `print()` → comma-joined `as_string()` of all elements (`"a, b, c"`).

### 2.10 `LuaSignal` (wraps `GeigerSignal`) — **geiger farming core datum**
Fields: `x`=`s.x`, `y`=`s.y`, `type`=`s.area_type.as_str()` (string), `timestamp_ms`=`s.timestamp_ms`.
`GeigerSignal` fields: `x`, `y`, `area_type` (enum with `as_str()`), `timestamp_ms`. This is what a charged Geiger emits when it detects buried treasure; automation reads it via `bot:getSignal()` / `autogeiger.getSignal()`.

### 2.11 `LuaLogin` / `LuaConsole`
`LuaLogin{mac:String}` → field `mac`.
`LuaConsole(Arc<RwLock<BotState>>)`: field `contents` → array copy of `state.console` (Vec<String>); method `append(text)` → push to `state.console`, and **if len > 100 remove index 0** (ring buffer cap 100). The bot console holds GT-colored log lines (see color codes §7).

---

## 3. HTTP HELPER (`http.rs`) → `Adonai::Http`

Exposes `http.request(method,url,opts?)`, `http.get(url,opts?)`, `http.post`, `http.put`, `http.delete`. `get/post/put/delete` just call `request` with the method fixed.

### 3.1 Options parsing (`lua_http_options`)
`opts` table keys:
- `body`: nil→none; string→as-is; integer/number/boolean→stringified (`n.to_string()`, `v.to_string()`). Any other type → error `"http options.body must be a string, number, or boolean"`.
- `timeout_ms`: `Option<u64>`.
- `headers`: table of name→value; each value string/int/number/bool→string, else error `"http options.headers values must be strings, numbers, or booleans"`.

### 3.2 Request (`make_http_request`)
1. `timeout = timeout_ms.unwrap_or(10_000)` ms → global timeout.
2. Build agent: `http_status_as_error = false` (non-2xx is NOT an error — status returned to caller).
3. Build request: set method, uri, add all headers in order.
4. If body present → send with body; else send with empty body `()`.
5. On transport build/run failure → error strings `"http request build failed: {e}"` / `"http request failed: {e}"`.
6. Read response:
   - `status` = numeric code (u16), `status_text` = canonical reason or `""`.
   - `headers` table: **every response header name lowercased**, value = UTF-8 lossy string.
   - `body` = raw bytes read to vec (as Lua string; may be binary).
7. Return table: `{ ok = (200..=299 contains status), status, status_text, headers, body }`.

### 3.3 Adonai mapping
- Implement as `Adonai::Http::Response Http::request(method, url, {headers, body, timeout_ms})` using **libcurl**.
- **Must route through SOCKS5h** (per-bot proxy) exactly like the rest of the engine's HTTP — use the bot's `BotProxy`/proxy config so scripted HTTP shares the bot's proxy identity. (In Mori this generic helper used a bare `ureq::Agent` with no proxy; Adonai should bind it to the owning bot's proxy so per-bot HTTP is IP-consistent — see MEMORY: ltoken IP-binding.)
- `ok` = status in [200,300). Non-2xx must NOT throw; return status. Default timeout **10000 ms**. Lowercase all response header keys. Preserve body as raw bytes (`std::string`), not assumed UTF-8.

---

## 4. WEBHOOK (`webhook.rs`) → `Adonai::Webhook` (fleet-shared Discord reporter)

This is the Discord reporting subsystem. Its defining feature: **the whole fleet edits ONE shared Discord message in place** (persisted across restarts) instead of every bot spamming new messages. This coordination MUST be preserved in Adonai via FleetState.

### 4.1 Types

```rust
struct WebhookState {           // per-webhook-object message config
    url: String,
    content: String,
    username: String,
    avatar_url: String,
    default_message_id: Option<String>,
}
struct WebhookEmbedState {      // one embed (each webhook has embed1 + embed2)
    use_embed: bool,
    title: String,
    description: String,
    url: String,
    color: Option<u32>,
    footer_text: String,
    thumbnail_url: String,
    image_url: String,
}
struct LuaWebhook { inner: Arc<Mutex<WebhookState>>, embed1: Arc<Mutex<...>>, embed2: Arc<Mutex<...>> }
struct LuaWebhookEmbed { inner: Arc<Mutex<WebhookEmbedState>> }
```

**Process-global shared map** (the key coordination primitive):
```rust
static WEBHOOK_MESSAGE_IDS: OnceLock<Mutex<HashMap<String,String>>>;  // webhook-url-key → discord message id
```

### 4.2 Construction & URL handling
`Webhook.new(url)` → `split_webhook_message_url(url)`:
- Trim input. If it contains `"/messages/"`, split there: base = left part, `default_message_id` = right part **up to the first `?` or `#`** (empty → None). Else `(trimmed, None)`.
So a full `.../webhooks/ID/TOKEN/messages/123` seeds an edit target.

Helper URL functions:
- `append_wait_true(url)`: if url already contains `"wait="` → unchanged; else if it has `'?'` → `url + "&wait=true"`; else `url + "?wait=true"`. (Discord returns the created message JSON only with `?wait=true`.)
- `webhook_message_url(base,id)`: strip query (`base.split('?')[0]`) then `base + "/messages/" + id`.
- `webhook_message_key(base)`: `base.split('?')[0]` — the map key (query-stripped webhook URL). **All bots sharing a webhook URL share one message id.**

### 4.3 Object fields (get/set) exposed to automation
`url` (set re-runs split into url + default_message_id), `content`, `username`, `avatar_url`, `embed1` (→ `LuaWebhookEmbed` view of embed1), `embed2` (→ embed2).
Embed fields: `use` (bool `use_embed`), `title`, `description`, `url`, `color` (get returns `color.unwrap_or(0)`; set stores `Some(v)` only if `v != 0`, else `None`), `footer_text`, `thumbnail_url`, `image_url`.

### 4.4 Payload build (`payload_value` / `payload_json`)
Build JSON object:
- Insert `content`/`username`/`avatar_url` **only if non-empty after trim**.
- `embeds`: array from `[embed1, embed2]`, each via `WebhookEmbedState::to_json()`:
  - Returns `None` (skipped) if `use_embed == false`.
  - Else object with keys added only when non-empty/non-null: `title`, `description`, `url`, `color` (if `Some`), `footer` = `{ "text": footer_text }`, `thumbnail` = `{ "url": thumbnail_url }`, `image` = `{ "url": image_url }`.
- `embeds` inserted only if at least one embed produced JSON.
`payload_json` = serde `to_string`; error `"webhook json encode failed: {e}"`.

### 4.5 Low-level send (`send_request(method,url)`)
1. Error if `url` blank: `"webhook url is empty"`.
2. Body = `payload_json()`.
3. `run_json_request(method,url,body)`:
   - ureq agent, **global timeout 10 s**, `http_status_as_error=false`.
   - Headers: `Content-Type: application/json`, **`User-Agent: "Mori Lua Webhook"`** → **RENAME to `"Adonai Webhook"`** (see §8).
   - Errors: `"webhook request build failed: {e}"`, `"webhook request failed: {e}"`.
4. Read status (u16), status_text (canonical reason or `""`), body bytes → UTF-8 lossy text.
5. `message_id` = parse body as JSON, take `.id` string field if present.
6. Return table `{ ok=(200..300 contains status), status, status_text, body, message_id }`.

### 4.6 Public methods
- `makeContent()` → `payload_json()` (preview the JSON).
- `send()` → POST to `append_wait_true(url)`.
- `edit(message_id?)` → PATCH; message_id from arg (`message_id_from_lua`: string as-is, integer→decimal string, number→`"{n:.0}"`, nil→None) or fall back to `default_message_id`; error `"webhook edit requires message_id"` if none. URL = `webhook_message_url(url, id)`.
- `sendOrEdit()` → the fleet-shared updater (below).

### 4.7 `sendOrEdit` — the fleet single-message algorithm (`send_or_edit_request`)
This is the crux. Steps:
1. Clone `WebhookState`. `key = webhook_message_key(url)`.
2. **Lock the global map only for a quick read** → `message_id = ids.get(key).cloned().or(default_message_id)`. **Release the lock before any network I/O** (comment: never hold the process-global map across a ≤10 s Discord round-trip or the whole fleet serializes behind one request).
3. If a `message_id` exists:
   a. `send_request("PATCH", webhook_message_url(url, message_id))`.
   b. If `ok`: re-lock map; **only if `ids[key] != message_id`**, insert and `save_message_ids` (avoids rewriting file every edit). Return response.
   c. Else if `status != 404`: return response (some other error — surface it).
   d. Else (`404` — message deleted server-side): re-lock, `remove(key)`, save; fall through to recreate.
4. POST to `append_wait_true(url)`. If `ok` and response has `message_id`: re-lock map, insert `key→id`, save. Return response.

**Lock discipline (must replicate):** take the shared map lock only for the brief read, do network unlocked, re-lock briefly to record. Never hold across I/O.

### 4.8 Persistence (`load_message_ids` / `save_message_ids`)
- Path: `current_dir()/data/webhook_messages.json`.
- Load: read file → `serde_json::from_str` → `HashMap<String,String>`; any failure → empty map. Loaded once lazily on first `get_or_init`.
- Save: `create_dir_all(parent)`, write `to_string_pretty`. Errors ignored.

### 4.9 Adonai mapping
- `Adonai::Webhook` native class. Store `WebhookState` + two `WebhookEmbedState` (plain members; no per-field Arc<Mutex> needed unless the webhook object is shared across threads — it is per-automation, so a single object mutex suffices).
- **`WEBHOOK_MESSAGE_IDS` → `FleetState::webhook_message_ids` (a `std::unordered_map<std::string,std::string>` guarded by its own `std::mutex`), persisted to `data/webhook_messages.json`.** All bots share it. Preserve the lock-only-for-read-and-write, never-across-I/O discipline.
- HTTP via libcurl through the bot's proxy; timeout 10 s; header `User-Agent: Adonai Webhook`, `Content-Type: application/json`.
- JSON via nlohmann/json. Extract `id` from response for message id.
- URL helpers (`split`, `append_wait_true`, `message_url`, `message_key`) are pure string ops — port verbatim.
- Message-id-from-value coercion: string as-is, integers → decimal, floats → `%.0f`.

---

## 5. GEIGER STATS (`geiger_stats.rs`) → `FleetState::geiger_stats`

Process-global aggregate of geiger finds across **all bots**, disk-persisted. Each bot in Mori has its own Lua VM, so this shared store is how the single webhook status message can report combined fleet totals (e.g. "1x white crystal, 1x radioactive, found 2 times so far").

### 5.1 Type & storage
```rust
static GEIGER_STATS: OnceLock<Mutex<GeigerStats>>;
struct GeigerStats {
    counts: BTreeMap<String,u64>,   // item name → aggregated find count (sorted)
    total_geigers: u64,             // total prizes/geigers found
}
```
`stats()` lazily inits from `load_stats()`.

### 5.2 API (`geiger_stats.*`)
- `record(name:String, count:i64)` → trim name; if non-empty **and count > 0**: `counts[name] += count`, save.
- `add_total(count:i64)` → if `count > 0`: `total_geigers = total_geigers.saturating_add(count)`, save.
- `count(name:String)` → `counts[trim(name)]` or `0`.
- `total()` → `total_geigers`.
- `reset()` → clear counts, `total_geigers = 0`, save. (Fresh hunt session.)

### 5.3 Persistence
- Path `current_dir()/data/geiger_stats.json`. Load: read → `serde_json::from_str` → default on any error. Save: `create_dir_all(parent)`, write `to_string_pretty`, errors ignored. `#[serde(default)]` on both fields (tolerant of missing keys).

### 5.4 Adonai mapping
- `FleetState::geiger_stats`: `std::map<std::string,uint64_t> counts` (ordered — BTreeMap) + `uint64_t total_geigers`, guarded by a `std::mutex`. Same 5 operations. `saturating_add` = clamp at `UINT64_MAX`. Persist to `data/geiger_stats.json` via nlohmann/json, pretty-printed, tolerant of missing keys. Every bot's automation records into this one shared object → the fleet webhook shows combined totals.

---

## 6. AUTOMATION MODULES — the embedded Lua library (`runtime.rs` `lua.load(...)`)

**This is the behavior to reimplement natively.** In Mori these are Lua functions preloaded into every VM. In Adonai each becomes a native C++ automation routine operating on the bot + FleetState. They call the §1 operations and §2 read model. All `sleep(ms)` become interruptible native sleeps (see §7 stop semantics). Reproduce every default constant, target-tile pattern, and stall/limit heuristic exactly.

### 6.1 `autogeiger` module (geiger farming enablement)
```
autogeiger.hasGeiger(item_id=2204, dead_item_id=2286):
    inv = getInventory()
    return inv:getItemCount(item_id) > 0 or inv:getItemCount(dead_item_id) > 0

autogeiger.enable(opts):
    item_id      = opts.item_id  | opts.itemId     | 2204   (charged geiger)
    dead_item_id = opts.dead_item_id | opts.deadItemId | 2286 (dead/discharged geiger)
    inv = getInventory()
    charged = inv:getItem(item_id)
    if charged and charged.count > 0:
        if not isWearing(item_id): wear(item_id)
        return true
    dead = inv:getItem(dead_item_id)
    if dead and dead.count > 0:
        if not isWearing(dead_item_id): wear(dead_item_id)
        return true
    return false

autogeiger.getSignal(): return getSignal()   -- bot:getSignal() → LuaSignal|nil
```
**Constants:** charged geiger item id **2204**, dead geiger **2286**. These are Growtopia item ids — keep exact.
**Adonai module `AutoGeiger`:** `bool hasGeiger(chargedId=2204, deadId=2286)`, `bool enable(GeigerOpts)`, `optional<GeigerSignal> getSignal()`. Reads inventory, wears charged geiger if present (else dead one), returns whether a geiger got equipped. This is the *entry* to a geiger hunt; the actual dig/collect loop is user-side in Mori (script drives it via getSignal + move + collect). In Adonai, build a full `GeigerHunt` module that: equips via `AutoGeiger::enable`, polls `getSignal()`, walks toward the signal `{x,y}`, digs, and on find calls `geiger_stats.record(itemName, 1)` + `add_total(1)` and pushes the fleet webhook `sendOrEdit`.

### 6.2 `rotation` module (farming: harvest / plant / break / buy / drop)
```
rotation.itemCount(item_id): return getInventory():getItemCount(item_id)

rotation.readyTrees(seed_id):
    world = getWorld(); out = {}
    if not world: return {}
    for tile in world:getTiles():
        if tile.fg == seed_id and tile:canHarvest():   -- canHarvest = Seed & item_on_tree>0
            out += {x=tile.x, y=tile.y}
    return out

rotation.harvestReady(seed_id, limit=999999, delay_ms=200):
    auto_collect = true
    done = 0
    for tile in readyTrees(seed_id):
        if done >= limit: break
        findPath(tile.x, tile.y)
        sleep(delay_ms)
        if isInTile(tile.x, tile.y):
            hit(tile.x, tile.y)
            sleep(delay_ms)
            done += 1
    return done

rotation.plantSeeds(seed_id, delay_ms=200):
    inv = getInventory(); world = getWorld()
    if not world: return 0
    planted = 0
    for tile in world:getTiles():
        if inv:getItemCount(seed_id) <= 0: break
        below = world:getTile(tile.x, tile.y+1)
        if tile.fg == 0 and below and below.fg != 0 and below.fg != seed_id:
            findPath(tile.x, tile.y); sleep(delay_ms)
            if isInTile(tile.x, tile.y):
                place(tile.x, tile.y, seed_id); sleep(delay_ms)
                planted += 1
    return planted

rotation.breakBlocks(block_id, x, y, opts):
    delay_place = opts.delay_place | opts.placeDelay | 200
    delay_break = opts.delay_break | opts.breakDelay | 200
    max_cycles  = opts.max_cycles  | opts.maxCycles  | 500
    stalled = 0
    auto_collect = true
    findPath(x, y); sleep(1000)
    broken = 0
    while itemCount(block_id) > 0 and max_cycles > 0:
        max_cycles -= 1
        before = itemCount(block_id)
        targets = { (x-1,y), (x-1,y+1), (x-1,y-1) }
        for t in targets:                     -- place phase
            tile = getWorld():getTile(t.x,t.y)
            if tile and tile.fg == 0 and itemCount(block_id) > 0:
                place(t.x, t.y, block_id); sleep(delay_place)
        for t in targets:                     -- break phase (≤12 hits per target)
            for _=1..12:
                tile = getWorld():getTile(t.x,t.y)
                if not tile or tile.fg == 0: break
                hit(t.x, t.y); sleep(delay_break); broken += 1
        collect(4.0, 250); sleep(100)
        after = itemCount(block_id)
        if after >= before: stalled += 1 else stalled = 0
        if stalled >= 8: break
    return broken

rotation.buyPack(pack_name="world_lock", price=2000):
    bought = 0
    while (gem_count or 0) >= price:
        sendPacket(2, "action|buy\nitem|" .. pack_name)   -- packet type 2 = generic text
        sleep(3000)
        bought += 1
    return bought

rotation.dropExcess(item_id, keep_count=0):
    count = itemCount(item_id)
    if count > keep_count:
        drop(item_id, count - keep_count)
        return count - keep_count
    return 0
```
**Key constants/patterns to preserve exactly:** harvest default limit `999999`, all default delays `200 ms`, breakBlocks initial `sleep(1000)`, place/break target column is **x-1** (three tiles: same/below/above), max **12** hit attempts per target tile, `collect(range=4.0, interval=250)` each cycle then `sleep(100)`, stall detection (`after >= before` counts as stalled; `stalled >= 8` aborts), max_cycles default `500`. `buyPack` uses **packet type 2** with body `"action|buy\nitem|<pack>"`, 3 s between buys, default pack `"world_lock"` price `2000` gems.

**Adonai modules to build** (each a class driving the bot via §1 API, interruptible):
- `HarvestModule` — `int harvestReady(seedId, limit=999999, delayMs=200)`, `std::vector<TilePos> readyTrees(seedId)`.
- `PlantModule` — `int plantSeeds(seedId, delayMs=200)`.
- `BreakModule` — `int breakBlocks(blockId, x, y, BreakOpts)`.
- `BuyModule` — `int buyPack(name="world_lock", price=2000)`.
- `InventoryTrimModule` — `int dropExcess(itemId, keepCount=0)`, `int itemCount(itemId)`.

### 6.3 Event helpers (pure-Lua glue) → native event dispatch
Mori defines: `Event = {variantlist=1, gameupdate=2, gamemessage=3}`, `addEvent(type,fn)`, `removeEvent(type)`, `removeEvents()`, `unlistenEvents()`, and `listenEvents(secs?)` which polls the `event_rx` channel.
`listenEvents` behavior: loop until optional `secs` elapsed (nil = forever); break if `__listening_stop` set or global stop flag; drain all pending `BotEventRaw` via `try_recv`; dispatch by index: **1**=VariantList `f(LuaVariantList, net_id)`, **2**=GameUpdate `f(LuaGameUpdatePacket)`, **3**=GameMessage `f(text)`; then `sleep(10ms)`.
`BotEventRaw` enum: `VariantList{vl:VariantList, net_id}`, `GameUpdate{pkt:GameUpdatePacket}`, `GameMessage{text:String}`.
**Adonai:** replace with a native **callback registry** on the bot: `onVariantList(cb)`, `onGameUpdate(cb)`, `onGameMessage(cb)`, dispatched directly from the net thread (no polling channel needed). Automation modules register C++ callbacks instead of Lua functions. Keep the three event categories.

### 6.4 Misc runtime globals → native utilities
- `sleep(ms)` → interruptible sleep polling stop flag every 10 ms (§7).
- `getInfo(id|name)` → item lookup: numeric → `items.find_by_id`; string → try parse as u32 then `find_by_id`, else `find_by_name`. Returns `LuaItemInfo|nil`.
- `getInfos()` → all `ItemInfo`.
- `read(path)`/`write(path,content)`/`append(path,content)` → file I/O (append uses create+append open). **In Adonai, gate these behind engine-controlled paths, not arbitrary script FS access** (there is no user script).
- `removeColor(text)` → strip GT color codes: walk chars; on backtick `` ` `` consume it **and the next char**; else keep. (See §7 color format.)
- `clearConsole()` → `state.console.clear()`.
- `getUsername()` → the bot's username (captured at start).
- Convenience globals `getBot/getLocal/getWorld/getInventory/getSignal/getPlayer/getPlayers/getTile/getTiles/getTilesSafe/getObject/getObjects/getNPC/getNPCs/hasAccess` — thin wrappers over `bot:*`; `getNPC`/`getNPCs` are stubs (`nil`/`{}`), `hasAccess` stub `false`. In Adonai these are just the bot object's methods; no separate globals needed.

### 6.5 `rotation` pool API (cross-bot world coordination) → FleetState
Registered natively in Rust (`register_rotation_pool_api`), backed by `crate::rotation_pool` (process-global). This is **fleet coordination**: bots claim/update/release worlds in a named pool so multiple bots share a rotation of farm worlds without colliding. **Port into FleetState.**

Types (from call sites):
```
RotationPoolTarget { world:String, door:String }
RotationWorldStatus {
    world:String, door:String, status:String,
    bot_id:Option<u32>, bot_name:String,
    ready_count:u32, seed_count:u32, capacity:u32,
    next_ready_at:u64, updated_at:u64, note:String
}
Snapshot { pool_id:String, updated_at:u64, worlds:Vec<RotationWorldStatus> }
```
API (`rotation.*`, distinct from the farming `rotation.*` above — same table name, different functions added natively):
- `rotation.poolClaim(pool_id, worlds_table, bot_id, bot_name, lock_ttl_sec=180)`:
  - `worlds_table` → array of `{world, door}` rows; **skip rows with blank world** (`lua_targets`).
  - Calls `rotation_pool::claim_world(pool_id, targets, bot_id, bot_name, lock_ttl_sec)`. Returns claimed `RotationWorldStatus` table or `nil` (nothing claimable). **Default lock TTL 180 s.**
- `rotation.poolUpdate(pool_id, opts_table)`: reads `world, door, status(="unknown"), ready_count(0), seed_count(0), capacity(0), next_ready_at(0), bot_id(>0 filter → Option), bot_name, note`; calls `update_world(...)`; returns updated status table.
- `rotation.poolRelease(pool_id, world, bot_id)`: `release_world(...)`.
- `rotation.poolSnapshot(pool_id)`: `snapshot(pool_id)` → `{pool_id, updated_at, worlds=[status...]}`.
- `lua_rotation_status` serializes a status to a table with keys: `world, door, status, bot_id(unwrap_or 0), bot_name, ready_count, seed_count, capacity, next_ready_at, updated_at, note`.

**Adonai:** `FleetState::rotationPools` — a map `pool_id → { updated_at, vector<WorldStatus> }` under a mutex. Implement `claim/update/release/snapshot` with the exact field set and the **180 s default lock TTL** (a claim expires after TTL so a dead/hung bot's world frees up). Blank-world rows skipped. This is a primary fleet-coordination mechanism: bots claim worlds from a shared pool, report ready/seed counts + next-ready timestamps, and release when done. The full semantics of `claim_world/update_world/release_world/snapshot` live in the `rotation_pool` module spec — here we bind the automation surface to them.

---

## 7. THREADING, STOP SEMANTICS & SHARED STATE

### 7.1 Per-bot script thread (Mori) → per-bot automation thread (Adonai)
`run_script_threaded(req_tx, reply_rx, event_rx, items:Arc<ItemsDat>, state:Arc<RwLock<BotState>>, stop_flag:Arc<AtomicBool>, username, script)`:
- Creates a fresh `Lua` VM per bot.
- Installs an **instruction hook every 200 instructions** that returns error `"__script_stop__"` when `stop_flag` is set — cooperative cancellation.
- Builds `BotProxy{req_tx, reply_rx, state}`, sets it as global `bot`.
- Registers all APIs (http, webhook, geiger_stats, rotation pool), loads the automation library + event glue, then `lua.load(&script).exec()`.
- On setup error → push `"`4[Lua setup error] {e}"` to console, return.
- On script error → if message does **not** contain `"__script_stop__"`, push `"`4[Lua] {e}"` to console (stop errors are swallowed silently).

**Adonai:** each bot runs its automation on **`std::thread`** (no Lua VM). Cooperative stop = a per-bot `std::atomic<bool> stop_flag`. All long loops/sleeps check it (equivalent to the 200-instruction hook + the sleep poll). Error logging pushes GT-colored lines into the bot console ring buffer (cap 100). Rename log prefixes (§8): `[Adonai setup error]` / `[Adonai automation error]`.

### 7.2 Interruptible sleep
`sleep(ms)`: compute deadline = now + ms; loop: if `stop_flag` set → error `"__script_stop__"`; `thread::sleep(10ms)`; until deadline. **Port exactly**: native `sleep_interruptible(ms)` polling the stop flag every 10 ms, throwing/returning a cancellation the moment stop is set.

### 7.3 Request/reply concurrency
`BotProxy::request` blocks the automation thread until the net thread replies (one in-flight request at a time per bot). Reading hot state (`bot.name/status/pos/gems/ping/mac/geiger_signal/console`) bypasses the channel via the shared `RwLock<BotState>`. **Adonai:** the automation thread calls bot methods that lock the bot's state/command mutex; hot reads use a shared_lock. Keep command calls serialized per bot.

### 7.4 Fleet-wide shared state (bots aware of each other) — build into `FleetState`
Three process-global, disk-persisted stores in Mori that **all bots share** and that make the fleet coordinate. In Adonai consolidate into a single `FleetState` singleton (each guarded appropriately):

| Mori global | Purpose | FleetState member | File |
|---|---|---|---|
| `WEBHOOK_MESSAGE_IDS: Mutex<HashMap<String,String>>` | one Discord status message per webhook URL for the whole fleet; edited in place, survives restart | `webhook_message_ids` (mutex map) | `data/webhook_messages.json` |
| `GEIGER_STATS: Mutex<GeigerStats>` | combined geiger find counts + total across all bots | `geiger_stats` (mutex struct) | `data/geiger_stats.json` |
| `rotation_pool` (global) | named pools of farm worlds bots claim/release with TTL locks | `rotation_pools` (mutex map) | (per rotation_pool spec) |

**Lock discipline (mandatory):** never hold a fleet-shared lock across network I/O (webhook does a lock-read → unlock → HTTP → lock-write → unlock dance specifically to avoid serializing the whole fleet behind one ≤10 s Discord request — replicate this). Use `std::mutex` + brief critical sections; a `std::mutex`+`condvar` queue replaces `crossbeam-channel` for the request/reply path if you keep a threaded command channel.

### 7.5 GT text/color conventions (needed by console + removeColor)
- Console lines carry GT color codes: backtick `` ` `` followed by one code char (e.g. `` `4 `` = red used for errors, `` `2 `` green, `` `0 `` reset, etc.). `removeColor` strips each backtick + the following char.
- `sendPacket(2, text)`: **packet type 2 = generic text/action packet**; body uses GT `key|value\n...` format (e.g. `"action|buy\nitem|world_lock"`).
- All GT binary integers are **little-endian** (relevant when building `GameUpdatePacket` via the field map §2.8 — see packet spec for exact byte layout).

---

## 8. DEPENDENCY MAPPING (Rust crate → Adonai C++)

| Rust crate / item | Used for | Adonai C++ |
|---|---|---|
| `mlua` (Lua VM, `LuaUserData`, hooks, `create_function`) | entire scripting host | **REMOVED** — no Lua. API becomes native C++ methods; automation library becomes native modules (§6); instruction-hook stop → `std::atomic<bool>` checks. |
| `ureq` (`Agent`, `Config`, `http::Request`) | HTTP for `http.*` + webhook | **libcurl** (with SOCKS5h proxy + cookies per bot). `timeout_global` → `CURLOPT_TIMEOUT_MS`; `http_status_as_error=false` → don't treat non-2xx as error (default libcurl behavior). |
| `serde_json` (`Value`, `Map`, `json!`, to/from string) | webhook payloads, stats/id-map persistence | **nlohmann/json**. `to_string_pretty` → `dump(2)`. Tolerant parse (`from_str().ok()` → default) → try/catch → default. |
| `serde` derive (`Serialize/Deserialize`, `#[serde(default)]`) | `GeigerStats` on-disk | nlohmann `to_json`/`from_json`; missing keys default to 0/empty. |
| `crossbeam-channel` (`Sender/Receiver`, `send/recv/try_recv`) | script⇄bot request/reply + event stream | **`std::mutex`+`std::condition_variable` queue** (blocking `recv`, non-blocking `try_recv`). Or collapse to direct method calls under the bot mutex (§7.3). |
| `std::sync::{Arc, RwLock, Mutex, OnceLock, atomic::AtomicBool}` | shared state, lazy globals, stop flag | `std::shared_ptr`, `std::shared_mutex`, `std::mutex`, function-local `static` (Meyers singleton) for `OnceLock`, `std::atomic<bool>`. |
| `std::env::current_dir` + `std::fs` | `data/*.json` paths, `read/write/append` | `std::filesystem` + `<fstream>`; `create_dir_all` → `fs::create_directories`. |
| `md5` / `argon2` | (not in this module) | bundled md5 / argon2 lib (used elsewhere). |
| `rusty_enet` | (not here; net layer) | vendored C ENet patched for SOCKS5-UDP. |
| `scraper` | (not here) | regex / manual HTML scan. |
| `tokio`/`axum`/`tower` | (not here; this module is thread-based already) | `std::thread` + **Dear ImGui native UI** (no web server). Webhook/console surfaced in ImGui panels. |

---

## 9. RENAME RULES (Mori→Adonai, Cloei→North) — concrete occurrences in these files

Apply globally; concrete hits spotted in this module:

| Location | Original | Adonai |
|---|---|---|
| `webhook.rs` `run_json_request` User-Agent header | `"Mori Lua Webhook"` | `"Adonai Webhook"` (drop "Lua" — no Lua in Adonai) |
| `runtime.rs` setup error console line | `` "`4[Lua setup error] {e}" `` | `` "`4[Adonai setup error] {e}" `` |
| `runtime.rs` script error console line | `` "`4[Lua] {e}" `` | `` "`4[Adonai] {e}" `` or `` "`4[automation] {e}" `` |
| module path | `src/lua/*` | `src/automation/*` (or `engine/automation`); "lua" is not a brand but the module is no longer Lua |
| internal stop sentinel | `"__script_stop__"` | keep as an internal cancellation signal (rename to `"__adonai_stop__"` if surfaced; it is matched by substring in error text) |
| data files | `data/webhook_messages.json`, `data/geiger_stats.json` | keep names (not brand-specific); ensure they live under Adonai's data dir |

**No `Cloei`/`cloei` occurrences** appear in these six files. Still, apply the rule project-wide: any `Cloei`/`cloei` → `North`/`north` in identifiers, paths, log lines, window titles, user-agents, config filenames. Likewise any other `Mori`/`mori` string (window titles, config filenames, user-agents) elsewhere → `Adonai`/`adonai`.

> Note: the Lua-facing identifier names themselves (`bot`, `getWorld`, `autogeiger`, `rotation`, `geiger_stats`, `Webhook`, `http`) are the automation's public vocabulary, not brand names — keep them as the native C++ method/module names for continuity (they are not "Mori"/"Cloei").

---

## 10. C++ MODULE CHECKLIST (what to build)

Native modules operating on the bot + `FleetState`:

1. **`Bot` engine API** — every §1.2 operation as a synchronous method (movement, place/hit/wrench, warp/say/enter, inventory drop/trash/wear, getWorld/getInventory/getLocal/getSignal, collect/findPath/getPath, delays & auto_collect getters/setters). Hot-state reads under shared_lock.
2. **View structs** (§2): `World, Tile, InventoryItem, WorldObject, Player, ItemInfo, GameUpdatePacket, Variant/VariantList, GeigerSignal` with the exact exposed accessor names.
3. **`Adonai::Http`** (§3) — libcurl, proxy-bound, 10 s default, lowercased headers, ok=2xx, raw body.
4. **`Adonai::Webhook`** (§4) — payload builder (content/username/avatar + 2 embeds), send/edit/sendOrEdit, fleet single-message algorithm via `FleetState::webhook_message_ids`, disk persistence, User-Agent `Adonai Webhook`.
5. **`FleetState::geiger_stats`** (§5) — record/add_total/count/total/reset, ordered map, saturating total, `data/geiger_stats.json`.
6. **`FleetState::rotation_pools`** (§6.5) — claim/update/release/snapshot, 180 s TTL locks, blank-world skip.
7. **Automation routines** (§6.1–6.2): `AutoGeiger`(enable/hasGeiger/getSignal) + full `GeigerHunt` loop, `Harvest`, `Plant`, `Break`, `Buy`, `InventoryTrim` — each interruptible via the per-bot stop flag, reproducing every default constant and heuristic exactly.
8. **Event dispatch** (§6.3) — native `onVariantList/onGameUpdate/onGameMessage` callbacks (types 1/2/3), fired from the net thread.
9. **Per-bot automation thread + interruptible sleep** (§7.1–7.2) with cooperative `std::atomic<bool>` stop.
10. **Utilities** (§6.4): `getInfo/getInfos` item lookup, `removeColor` GT color stripper, console ring buffer (cap 100), `getUsername`.
