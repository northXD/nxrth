# Nxrth Port Spec — Module 12: UI & Glue

**Source Rust files covered:** `web.rs`, `main.rs`, `logger.rs`, `bot/mod.rs`, `lua/mod.rs`
(Mori-2.0.0 `src/`)

**Nxrth target:** Dear ImGui + DirectX11 native desktop app, **Tahoma** font, **NO web
server**, **NO Lua**. Everything the Mori web dashboard exposed becomes a native ImGui
panel or control; the HTTP/WebSocket transport disappears and handlers become direct
in-process function calls guarded by a mutex.

This spec is the single source of truth for porting this module. It is self-contained:
you do not need to read the Rust. Types owned by *other* modules (e.g. `BotInfo`,
`BotState`, `WsEvent`, `ProxyPool`, `BotManager`, `BotCommand`, `BotDelays`) are
referenced by name with a pointer to their owning module spec; every DTO **defined in
`web.rs` itself** is reproduced here field-for-field because those are the exact shapes
the UI forms must produce/consume.

---

## 0. RENAME RULES (apply everywhere in the port)

Global substitution rules for the whole Nxrth codebase:

| Mori identifier | Nxrth replacement |
|---|---|
| `Mori` / `mori` | `Nxrth` / `nxrth` |
| `Cloei` / `cloei` (upstream author/repo) | `North` / `north` |

**Concrete occurrences found in the 5 files of THIS module:**

| File | Line | Original | Rename to |
|---|---|---|---|
| `logger.rs` | 9 | `"mori_debug.log"` (open in `init_logger`) | `"nxrth_debug.log"` |
| `logger.rs` | 23 | `"mori_debug.log"` (open in `log`) | `"nxrth_debug.log"` |
| `logger.rs` | 30 (comment) | "written to mori_debug.log" | "written to nxrth_debug.log" |
| `web.rs` | 813 | injected footer HTML `Mori created with ❤︎ by Cendy` | **Panel is removed entirely** (no web frontend). If a credit line is kept in the ImGui "About" it becomes `Nxrth created with ❤ by Cendy`. `Cendy` is **not** covered by the rename rules — leave `Cendy` as-is unless the caller says otherwise. (No `Cloei/cloei` token appears in these 5 files.) |

No `Mori`/`mori` tokens appear in `main.rs`, `bot/mod.rs`, or `lua/mod.rs`. No window
title, user-agent, or config filename other than `mori_debug.log` appears in this module.
(The `growtopia_cdn` fetch uses `ureq`'s default UA; when reimplemented on libcurl set an
Nxrth UA if a UA is desired — none is set in the Rust.)

---

## 1. APP ENTRY & GLUE (`main.rs`)

### 1.1 Rust behavior (exact)

```rust
#[tokio::main]
async fn main() {
    logger::init_logger();
    let (ws_tx, _) = tokio::sync::broadcast::channel(256);
    let mgr = Arc::new(Mutex::new(BotManager::new(ws_tx.clone())));
    web::serve(mgr, ws_tx).await;
}
```

Module declaration order (for reference — each has its own module spec):
`account_devices, astar, auth, bot, bot_manager, bot_state, constants, cursor, dashboard,
events (pub), har_parser, inventory, items (pub), logger (pub), login, lua, player,
protocol, proxy_pool, proxy_test, rotation_pool, save_dat (pub), script_channel (pub),
server_data, socks5, web, world (pub)`.

Startup sequence:
1. `logger::init_logger()` — appends a session banner to the debug log file (see §3).
2. Create a **broadcast channel with capacity 256**: `ws_tx` is the sender, the initial
   receiver is dropped (`_`). `ws_tx` is the fleet-wide event bus (see §4).
3. Create the single `BotManager` (given a clone of `ws_tx`), wrap it
   `Arc<Mutex<BotManager>>` → this is `SharedManager`.
4. `web::serve(mgr, ws_tx).await` — runs forever (the axum server).

### 1.2 Nxrth port

- `#[tokio::main]` → a normal `int main()` (or WinMain) that:
  1. Calls `logger::init()` (writes the banner to `nxrth_debug.log`).
  2. Creates the **event bus** (replaces the tokio broadcast channel; see §4.2): a
     bounded, fan-out, mutex+condvar queue with a **capacity of 256** and *drop-oldest
     when a consumer lags* semantics (matches the tokio `Lagged` behavior — see §5.6).
  3. Creates the single `BotManager` guarded by `std::mutex` (this is the
     `SharedManager` equivalent) and hands it the event-bus publisher.
  4. Initializes the Win32 window + DirectX11 device + Dear ImGui context, loads the
     **Tahoma** font, and enters the UI loop (replaces `web::serve`). The UI loop each
     frame drains the event bus into the console/traffic panels and renders all panels
     described in §2.
- There is **no `.await`**; long-running/blocking calls (proxy test, CDN fetch) run on
  worker `std::thread`s and post results back through the event bus or a result promise.

---

## 2. THE DASHBOARD, AS AN IMGUI PANEL CHECKLIST (`web.rs`)

Every route below is a feature the current dashboard exposes. In Nxrth each becomes a
direct call into `BotManager`/`ProxyPool`/etc. from an ImGui widget — there is no HTTP.
The **request DTO** columns are the exact JSON the current frontend sends; in Nxrth they
become the form-state structs backing each panel.

### 2.0 Shared app state

`web.rs` defines:

```rust
pub type SharedManager = Arc<Mutex<BotManager>>;

#[derive(Clone)]
pub struct AppState {
    pub manager: SharedManager,               // Arc<Mutex<BotManager>>
    pub ws_tx: WsTx,                           // crate::events::WsTx = broadcast::Sender<WsEvent>
    pub auth: AuthState,                       // crate::auth::AuthState (Clone; Arc inside)
    pub proxy_pool: Arc<Mutex<ProxyPool>>,     // crate::proxy_pool::ProxyPool
}
```

Nxrth: a single `AppContext` struct holding `BotManager* (mutex-guarded)`, the event-bus
publisher, an `AuthState` (see §2.1 — becomes optional local gate), and a
mutex-guarded `ProxyPool`. Passed by reference to every panel's render function.

**Mutex-poison recovery:** every handler locks the manager with
`.lock().unwrap_or_else(std::sync::PoisonError::into_inner)` — i.e. a poisoned mutex is
recovered rather than panicking. In C++ `std::mutex` has no poison concept, so just lock
normally; ensure no panic-equivalent (exception) is thrown while the lock is held.

### 2.1 Auth panel (login gate) — routes `/auth/*`

> **NOTE — auth is NOT enforced server-side in this Rust.** The router (`serve()`)
> applies only a CORS layer; there is **no auth middleware** on the "Protected API"
> routes despite the comment. `AuthState` mints a token on login but no route validates
> it. In Nxrth, treat this as an **optional local unlock screen** (single-user password
> gate) that hides panels until unlocked; it is not a security boundary.

| Method / path | Handler | Request DTO (defined in web.rs) | Response | Behavior |
|---|---|---|---|---|
| `GET /auth/status` | `auth_status` | — | `{ "registered": bool }` | Returns `auth.is_registered()`. |
| `POST /auth/setup` | `auth_setup` | `SetupRequest { password: String }` | `204 No Content`, or `409 {"error":"already registered"}`, or `500 {"error": <msg>}` | If already registered → 409. Else `auth.register(&password)`: Ok→204, Err→500. Registers the single user; only works once. |
| `POST /auth/login` | `auth_login` | `LoginRequest { password: String }` | `{ "token": "…" }`, or `401 {"error":"invalid password"}` | `auth.login(&password)` → `Some(token)` → 200 with token; `None` → 401. |
| `POST /auth/logout` | `auth_logout` | — | `204 No Content` | `auth.logout()`. |

ImGui controls:
- **First-run "Set password"** field → calls `AuthState::register` (Argon2-hashed;
  password store owned by the auth module — see auth module spec; crate `argon2`).
- **"Unlock" password** field → `AuthState::login`. On success, reveal the main panels.
- **"Lock"** button → `AuthState::logout`.
- A "registered?" query decides whether to show setup vs. login on launch.

### 2.2 Bots panel — add / list / remove / login modes

**Login modes = the five spawn endpoints.** Each maps to a `BotManager::spawn*` call and
returns a new bot `id` (`u32`). The exact login flow behind each lives in
`bot_manager` / `login` (see those specs); here are the endpoints, DTOs, and the proxy
resolution glue (which IS in web.rs, §2.9).

| Method / path | Handler | Request DTO | Manager call | Returns |
|---|---|---|---|---|
| `GET /bots` | `list_bots` | — | `manager.list()` | `Vec<BotInfo>` (JSON array) |
| `POST /bots` | `spawn_bot` | `SpawnRequest` | `spawn(username, password, proxy, login_proxy_url)` | `{ "id": u32 }` |
| `POST /bots/newly` | `spawn_newly_bot` | `SpawnRequest` | `spawn_newly(username, password, proxy, login_proxy_url)` | `{ "id": u32 }` |
| `POST /bots/requestly` | `spawn_requestly_bot` | `SpawnRequest` | `spawn_requestly(username, password, proxy, login_proxy_url)` | `{ "id": u32 }` |
| `POST /bots/ltoken` | `spawn_ltoken_bot` | `SpawnLtokenRequest` | `spawn_ltoken(ltoken, proxy, login_proxy_url)` | `{ "id": u32 }` |
| `POST /bots/har_token` | `spawn_har_token_bot` | `SpawnHarTokenRequest` | `spawn_har_token(har_path, proxy, login_proxy_url)` | `{ "id": u32 }` |
| `DELETE /bots/{id}` | `stop_bot` | path `id: u32` | `stop(id)` | `204` if true, else `404` |
| `GET /bots/{id}/state` | `bot_state` | path `id: u32` | `get_state(id)` | `BotState` JSON, or `404` |

DTOs (exact, from web.rs):

```rust
struct SpawnRequest {
    username: String,
    password: String,
    proxy_host: Option<String>,
    proxy_port: Option<u16>,
    proxy_username: Option<String>,
    proxy_password: Option<String>,
    use_proxy_pool: Option<bool>,   // defaults to true when absent (see resolve_proxy)
}

struct SpawnLtokenRequest {
    ltoken: String,
    proxy_host: Option<String>,
    proxy_port: Option<u16>,
    proxy_username: Option<String>,
    proxy_password: Option<String>,
    use_proxy_pool: Option<bool>,
}

struct SpawnHarTokenRequest {
    #[serde(default = "default_har_path")]  // default = "requestly_logs.har"
    har_path: String,
    proxy_host: Option<String>,
    proxy_port: Option<u16>,
    proxy_username: Option<String>,
    proxy_password: Option<String>,
    use_proxy_pool: Option<bool>,
}
// fn default_har_path() -> String { "requestly_logs.har".to_string() }
```

For **every** spawn handler the flow is identical:
1. `resolve_proxy(...)` → `Option<Socks5Config>` or an error `Response` (see §2.9).
2. `resolve_login_proxy(&s)` → `Option<RotatingLoginProxy>` or error (see §2.9).
3. Lock manager, call the matching `spawn*`, return `{ "id": id }`.

ImGui controls (Bots panel):
- **Bot list / table**: one row per `BotInfo` (fields owned by `bot_manager` spec — id,
  name, status, world, position, ping, proxy, etc.). Row selection drives the
  Console / World / Automation / Items sub-panels for that bot.
- **"Add bot" form** with a **login-mode selector** (5 modes: Standard, Newly,
  Requestly, Ltoken, HAR-token). Fields shown depend on mode:
  - Standard / Newly / Requestly: `username`, `password`.
  - Ltoken: single `ltoken` field.
  - HAR-token: `har_path` (default `requestly_logs.har`), typically a file picker.
  - All modes share the proxy block: `proxy_host`, `proxy_port`, `proxy_username`,
    `proxy_password`, and a **"Use proxy pool"** checkbox (default ON).
- **"Remove"/"Stop" button** per row → `stop(id)`.
- **Auto-refresh** the list; in Nxrth just read `manager.list()` each frame (or on
  event) rather than polling an HTTP endpoint.

### 2.3 Account-device import panel — `POST /account-devices/import`

```rust
struct AccountDeviceImportRequest { devices: Vec<AccountDeviceImportItem> }
struct AccountDeviceImportItem  { username: String, login_token: String }
```
Handler `import_account_devices`: for each item call
`account_devices::upsert_from_login_token(&username, &login_token)`:
- `Ok(true)` → `imported += 1`
- `Ok(false)` → skip (no-op / already up to date)
- `Err(e)` → immediately return `500 {"error": <msg>}` (aborts the batch)

Response: `{ "imported": <usize> }`.

ImGui: a **bulk import** dialog — a multiline text box or table of
`username , login_token` pairs, an **"Import"** button, and a result toast showing how
many were imported. `account_devices` is disk-backed global state (see that module spec).

### 2.4 Bot command panel — `POST /bots/{id}/cmd` (the control surface)

Handler `bot_cmd(id, CmdRequest)` maps the tagged-union request to a `BotCommand` and
calls `manager.send_cmd(id, cmd)` → `204` if the bot exists, `404` otherwise.

`CmdRequest` is a Serde **internally-tagged enum**: JSON `{"type":"<snake_case>", …}`.
Exact variants → `BotCommand` mapping:

| `type` value | Extra fields (exact types) | → BotCommand | ImGui control |
|---|---|---|---|
| `move` | `x: i32`, `y: i32` | `Move { x, y }` | Raw-position nudge (pixel coords) |
| `walk_to` | `x: u32`, `y: u32` | `WalkTo { x, y }` | Click-to-walk on world view (tile coords; A* pathfind) |
| `run_script` | `content: String` | `RunScript { content }` | **Lua source** box — see §2.10 (dropped in Nxrth; NO Lua) |
| `stop_script` | — | `StopScript` | "Stop script" button |
| `wear` | `item_id: u32` | `Wear { item_id }` | Item context menu / inventory panel |
| `unwear` | `item_id: u32` | `Unwear { item_id }` | Item context menu |
| `drop` | `item_id: u32`, `count: u32` | `Drop { item_id, count }` | Inventory "Drop N" |
| `trash` | `item_id: u32`, `count: u32` | `Trash { item_id, count }` | Inventory "Trash N" |
| `set_delays` | `BotDelays` (flattened) | `SetDelays(BotDelays)` | Delay sliders (fields owned by `bot_state` spec) |
| `set_auto_collect` | `enabled: bool` | `SetAutoCollect { enabled }` | **Auto-collect toggle** |
| `set_collect_config` | `radius_tiles: u8`, `blacklist: Vec<u16>` (default `[]`) | `SetCollectConfig { radius_tiles, blacklist }` | Collect radius slider + item blacklist editor |
| `set_auto_reconnect` | `enabled: bool` | `SetAutoReconnect { enabled }` | **Auto-reconnect toggle** |
| `disconnect` | — | `Disconnect` | "Disconnect" button |
| `reconnect` | — | `Reconnect` | "Reconnect" button |
| `accept_access` | — | `AcceptAccess` | "Accept world access" button |
| `warp` | `name: String`, `id: String` | `Warp { name, id }` | Warp box (world name + id) |

> `blacklist` uses `#[serde(default)]` → absent means empty vector.
> `set_delays` uses `#[serde(...)]` tuple variant `SetDelays(BotDelays)` — the `BotDelays`
> fields are inlined into the same object; get the exact field names/types from the
> `bot_state` module spec.

**Automation toggles (called out per FOCUS):** `set_auto_collect`,
`set_collect_config` (radius_tiles + blacklist), `set_auto_reconnect`, `set_delays`.
These are the persistent per-bot automation switches → ImGui checkboxes/sliders whose
state must reflect the current `BotState` (query via `GET /bots/{id}/state` equivalent).

### 2.5 Items browser — `/items`, `/items/names`, `/items/colors`

Backs the item picker used by Wear/Unwear/Drop/Trash/blacklist controls. All read from
`manager.items_dat.items` (the parsed items.dat; owned by `items` module spec —
`ItemInfo { id: u32, name: String, base_color: u32, overlay_color: u32, … }`).

**`GET /items`** — `list_items(ItemsQuery)`:
```rust
struct ItemsQuery {
    page: Option<usize>,
    q: Option<String>,
    #[serde(rename = "get-items")] get_items: Option<String>,  // query key "get-items"
}
```
Behavior:
- **Batch-by-id mode**: if `get_items` present, it is a comma-separated id list. Parse
  each trimmed token as `u32` (silently drop unparseable), collect into a `HashSet<u32>`,
  return `Vec<ItemInfo>` of all items whose `id` is in the set (JSON array, **no
  pagination wrapper**).
- **Search/paginated mode** (otherwise):
  - `q` lowercased (default `""`). `page` = `params.page.unwrap_or(1).max(1)`.
  - Filter: if `q` empty → keep all. Else keep if (`q` parses as `u32` **and** equals
    `item.id`) **or** `item.name.to_lowercase().contains(&q)`.
  - `total` = filtered count. `start = (page-1) * 50`. Take a 50-item window
    (`ITEMS_PAGE_SIZE = 50`).
  - Return `ItemsResponse { items: Vec<ItemInfo>, total: usize, page: usize, page_size: usize }` with `page_size = 50`.

**`GET /items/names`** — `item_names`: returns `HashMap<u32, String>` mapping every
item `id` → `name`.

**`GET /items/colors`** — `item_colors`: returns `HashMap<u32, u32>` mapping `id` → an
RGB color computed as:
```
raw = if id % 2 == 0 {                       // BLOCK (even id)
          items_dat.find_by_id(id + 1)       // its paired SEED
              .map(|seed| seed.base_color)
              .unwrap_or(item.base_color)
      } else {                               // SEED (odd id)
          item.overlay_color
      };
color = items::bgra_to_rgb(raw)              // BGRA(u32) → RGB(u32)
```
Port `bgra_to_rgb` exactly (owned by `items` module: swap B/R channels, drop alpha).
Growtopia convention: even id = block, odd id = its seed (`block_id + 1`).

ImGui: an **item picker** with a search box (`q`), pagination (50/page), a name lookup
map (for labels), and a color map (for the little color swatch next to each item). Build
these maps once at load from `items_dat` instead of hitting endpoints.

### 2.6 Proxy tester — `POST /proxy/test`

```rust
struct ProxyTestRequest {
    proxy_host: String,
    proxy_port: u16,
    proxy_username: Option<String>,
    proxy_password: Option<String>,
}
```
Handler `proxy_check`: build addr `"{host}:{port}"` parsed to `SocketAddr`
(`400 Bad Request` on parse failure); build `Socks5Config { proxy_addr, username, password }`;
run `run_proxy_test(cfg)` on a **blocking thread** (`tokio::task::spawn_blocking`);
return `ProxyTestResult` (owned by `proxy_test` module spec) or `500` if the blocking task
join fails. In Nxrth run `run_proxy_test` on a worker `std::thread` and show a spinner →
result. This is the **"Test proxy"** button in both the Add-bot form and the pool editor.

### 2.7 Proxy pool editor — `GET/PUT /proxy/pool`

```rust
struct ProxyPoolUpdateRequest {
    enabled: bool,
    max_bots_per_ip: usize,
    spread_mode: String,
    proxies_text: String,
    #[serde(default)] rotating_login_enabled: bool,
    #[serde(default)] rotating_login_scheme: String,
    #[serde(default = "default_rotating_login_port_span_request")] rotating_login_port_span: u16, // default 2000
    #[serde(default)] rotating_login_proxy_text: String,
}
// fn default_rotating_login_port_span_request() -> u16 { 2000 }
```

- **`GET /proxy/pool`** — `proxy_pool_get`: `counts = manager.proxy_key_counts()`; returns
  `proxy_pool.view(&counts)` → `ProxyPoolView` (owned by `proxy_pool` spec). The counts
  are the per-proxy-key live bot counts so the view can show utilization.
- **`PUT /proxy/pool`** — `proxy_pool_put`: `counts = manager.proxy_key_counts()`; then
  `pool.update(enabled, max_bots_per_ip, &spread_mode, &proxies_text,
  rotating_login_enabled, &rotating_login_scheme, rotating_login_port_span,
  &rotating_login_proxy_text)`. On `Err` → `400 {"error": …}`; on Ok → return the fresh
  `pool.view(&counts)`.

ImGui **Proxy Pool** panel controls:
- **Enable pool** checkbox (`enabled`).
- **Max bots per IP** integer (`max_bots_per_ip`).
- **Spread mode** combo/string (`spread_mode` — valid values validated inside
  `pool.update`; see `proxy_pool` spec).
- **Proxies** multiline text (`proxies_text`, one proxy per line — format parsed by the
  pool).
- **Rotating login** group: enable checkbox (`rotating_login_enabled`),
  **scheme** field (`rotating_login_scheme`), **port span** int default **2000**
  (`rotating_login_port_span`), and a **rotating-login proxy** multiline
  (`rotating_login_proxy_text`).
- Live **utilization view** rendered from `ProxyPoolView` + `proxy_key_counts`.

> **Fleet-awareness (per FOCUS §4):** `proxy_key_counts()` is a fleet-wide tally of how
> many live bots use each proxy key; the pool's `choose(&counts)` uses it to spread bots
> under `max_bots_per_ip`. This is exactly the kind of cross-bot shared state Nxrth must
> keep centralized in `BotManager`.

### 2.8 Rotation-pool viewer — `GET/DELETE /rotation/pool/{pool_id}`

- **`GET`** — `rotation_pool_get(pool_id)`: returns `rotation_pool::snapshot(&pool_id)` →
  `RotationPoolSnapshot` (owned by `rotation_pool` spec).
- **`DELETE`** — `rotation_pool_delete(pool_id)`: `rotation_pool::clear(&pool_id)` → `204`.

Note `rotation_pool` is accessed via **free functions** (module-global state), not through
`AppState` — i.e. a process-global registry keyed by `pool_id: String`. In Nxrth keep it
as a singleton owned by `BotManager` or a global service; expose a **"Rotation pools"**
viewer panel with a per-pool snapshot table and a **"Clear"** button.

### 2.9 Proxy-resolution glue (used by every spawn) — `resolve_proxy` / `resolve_login_proxy`

`resolve_proxy(s, proxy_host, proxy_port, proxy_username, proxy_password, use_proxy_pool)
-> Result<Option<Socks5Config>, Response>`:
1. **Manual proxy** (if `proxy_host` OR `proxy_port` is `Some`):
   - Require non-empty trimmed host else `400 "proxy host is required"`.
   - Require `proxy_port` else `400 "proxy port is required"`.
   - `addr = "{host.trim()}:{port}".parse::<SocketAddr>()` else `400 "invalid proxy address"`.
   - `username`/`password`: kept only if non-empty after trim (`filter(|v| !v.trim().is_empty())`).
   - Return `Some(Socks5Config { proxy_addr: addr, username, password })`.
2. **No pool**: if `use_proxy_pool.unwrap_or(true) == false` → `Ok(None)` (direct, no proxy).
3. **Pool pick** (default): `counts = manager.proxy_key_counts()`;
   `proxy_pool.choose(&counts)` → `Ok(Option<Socks5Config>)` or map `Err` → `400`.

`resolve_login_proxy(s) -> Result<Option<RotatingLoginProxy>, Response>`:
`proxy_pool.rotating_login_proxy()` → map `Err` → `400`. Returns the rotating login proxy
(used for the GT login HTTP leg; see the IP-binding memory note — login token binds to the
minting IP, so a *consistent* login proxy matters).

`Socks5Config` (from `bot::Socks5Config`, `bot/shared.rs`):
`{ proxy_addr: SocketAddr, username: Option<String>, password: Option<String> }`.

`json_error(status, msg) -> Response`: `(status, Json({"error": msg}))`.

Nxrth: these become plain helper functions returning `std::optional<Socks5Config>` (or a
`tl::expected`/error string) that the Add-bot panel calls before dispatching a spawn. The
`400`/error responses become inline form validation errors / toasts.

### 2.10 Console stream + script runner (Lua) — dropped/rewritten

- **`run_script` / `stop_script`** commands carry a **Lua source string** and drive the
  Lua subsystem (`lua/mod.rs` → `run_script_threaded`; see §6). **Nxrth has NO Lua.**
  The "script content" box and Lua console are **removed**; automations become native C++
  routines selected/toggled from the Automation panel. Keep `stop_script`/`StopScript`
  semantics as a generic "stop current automation" control.

### 2.11 Geiger traffic monitor / capture saver — `POST /geiger/captures`

The traffic monitor ("geiger") captures ENet packets and can persist a capture to disk.

DTOs (exact):
```rust
struct GeigerCapturePacket {
    id: u64, direction: String, kind: String, size: usize,
    summary: String, detail: String, timestamp_ms: u64,
}
struct GeigerCaptureRequest {
    bot_id: u32, stage: String, x: Option<f32>, y: Option<f32>,
    filter: String, started_at_ms: u64, ended_at_ms: u64,
    packets: Vec<GeigerCapturePacket>,
}
struct GeigerCaptureResponse { json_path: String, txt_path: String, packet_count: usize }
```

Handler `save_geiger_capture(req)`:
1. If `req.stage` is empty/whitespace → `400 "stage is required"`.
2. `dir = <cwd>/data/geiger_captures`; `create_dir_all(dir)` (→ `500` on IO error).
3. `stamp = chrono::Local::now().format("%Y%m%d-%H%M%S-%3f")` (`%3f` = milliseconds,
   3 digits). E.g. `20260711-134501-372`.
4. `stage = sanitize_file_part(&req.stage)` (see below).
5. `base = format!("geiger_bot{bot_id}_{stage}_{stamp}")`.
6. Write `dir/{base}.json` = `serde_json::to_string_pretty(&req)` (pretty JSON of the whole
   request; `400` if serialization fails, `500` on write error).
7. Write `dir/{base}.txt` = `format_geiger_capture_txt(&req)` (`500` on write error).
8. Return `GeigerCaptureResponse { json_path, txt_path, packet_count: req.packets.len() }`
   (paths are `Display` of the absolute PathBufs).

`sanitize_file_part(value) -> String`: map each char → keep if
`is_ascii_alphanumeric() || '-' || '_'`, else replace with `'_'`; then `trim_matches('_')`;
if the result is empty → `"stage"`.

`format_geiger_capture_txt(req) -> String` (pipe-delimited; reproduce byte-for-byte):
```
stage|<stage>\n
bot_id|<bot_id>\n
x|<x>\n            (only if req.x is Some)
y|<y>\n            (only if req.y is Some)
filter|<filter>\n
started_at_ms|<started_at_ms>\n
ended_at_ms|<ended_at_ms>\n
packet_count|<packets.len()>\n
\n
```
then for each packet (index from 0, printed as `index+1`):
```
--- packet <n> ---\n
timestamp_ms|<timestamp_ms>\n
direction|<direction>\n
kind|<kind>\n
size|<size>\n
summary|<summary>\n
detail|\n
<detail>\n\n
```

ImGui **Traffic / Geiger** panel:
- Live packet table: `id`, `direction` (in/out), `kind`, `size`, `summary`, expandable
  `detail`, `timestamp_ms`. A `filter` string box. A stage label field + optional `x`,`y`.
- **"Save capture"** button → writes the `.json` + `.txt` files under
  `data/geiger_captures/` using the exact naming/format above, then shows the two paths.
- The packet capture itself is produced by the geiger subsystem (`lua/geiger_stats.rs` in
  Mori; in Nxrth this is native ENet packet tapping — no Lua). The DTOs above are the
  serialization contract to preserve so old captures remain readable.

### 2.12 Growtopia CDN proxy — `GET /growtopia-cdn/{*path}`

Handler `growtopia_cdn(path)`: fetches `https://growserver-cache.netlify.app/{path}` via
blocking `ureq::get(url).call()`:
- On success: read `content-type` header (default `"application/octet-stream"`), read body
  to bytes, return `200` with that content-type. If body read fails → `502 Bad Gateway`.
- On request error or join error → `502 Bad Gateway`.

Purpose: the web frontend proxied item render assets through the app to avoid CORS. In
**Nxrth (native), CORS is a non-issue** — fetch item textures directly with libcurl if
needed, or bake/ship them. This endpoint can be **dropped**; keep only the upstream URL
constant `https://growserver-cache.netlify.app/{path}` if you still fetch render assets.

### 2.13 Static frontend serving + footer injection — `GET /` and fallback

- `index_html()`: reads `<cwd>/dist/index.html`, injects before `</body>` a fixed footer:
  `Mori created with ❤︎ by Cendy` (styled overlay div). Returns HTML.
- Fallback: `ServeDir::new(dist).fallback(ServeFile::new(dist/index.html))` — serves the
  built SPA and SPA-routes to index.html.

**Nxrth: entirely removed.** There is no `dist/`, no static serving, no HTML footer
injection. The UI is the compiled ImGui app. (If a credit is desired, put
`Nxrth created with ❤ by Cendy` in an About window; see §0.)

### 2.14 Full route table (server wiring, for completeness)

`serve(manager, ws_tx)` builds the router and binds `0.0.0.0:3000`:

```
GET    /                       index_html
GET    /auth/status            auth_status
POST   /auth/setup             auth_setup
POST   /auth/login             auth_login
POST   /auth/logout            auth_logout
POST   /account-devices/import import_account_devices
GET    /bots                   list_bots
POST   /bots                   spawn_bot
POST   /bots/newly             spawn_newly_bot
POST   /bots/requestly         spawn_requestly_bot
POST   /bots/ltoken            spawn_ltoken_bot
POST   /bots/har_token         spawn_har_token_bot
DELETE /bots/{id}              stop_bot
GET    /bots/{id}/state        bot_state
POST   /bots/{id}/cmd          bot_cmd
GET    /items                  list_items
GET    /items/names            item_names
GET    /items/colors           item_colors
POST   /proxy/test             proxy_check
GET    /proxy/pool             proxy_pool_get
PUT    /proxy/pool             proxy_pool_put
GET    /rotation/pool/{pool_id}    rotation_pool_get
DELETE /rotation/pool/{pool_id}    rotation_pool_delete
POST   /geiger/captures        save_geiger_capture
GET    /growtopia-cdn/{*path}  growtopia_cdn
GET    /ws                     ws_handler   (WebSocket)
(fallback) ServeDir(dist) → ServeFile(dist/index.html)
```
CORS layer: `allow_origin(Any)`, methods `[GET, POST, PUT, DELETE]`, `allow_headers(Any)`.
Startup prints (stdout):
```
Dashboard  http://localhost:3000
WebSocket  ws://localhost:3000/ws
API        http://localhost:3000/bots
```
Bind `.unwrap()` and `axum::serve(...).unwrap()` — panics on failure. **Nxrth drops all
of this**; the "server" is the ImGui window. (Optionally keep an internal "listening"
log-line noting the UI started.)

---

## 3. LOGGING (`logger.rs`)

Two free functions + one helper. File name **`mori_debug.log` → `nxrth_debug.log`**.

### `init_logger()`
- Open (`create`, `append`) `mori_debug.log` (unwrap → panic on error).
- Write a banner: `"\n--- Log Started at {YYYY-MM-DD HH:MM:SS} ---\n"` using
  `chrono::Local::now().format("%Y-%m-%d %H:%M:%S")`.

### `log(msg: &str)`
- Open (`create`, `append`) `mori_debug.log`.
- Write `"[{YYYY-MM-DD HH:MM:SS}] {msg}\n"` (local time, same format).
- **Also print to stdout** *unless* `is_console_spam(msg)` is true (still written to file).

### `is_console_spam(msg) -> bool`
- Strip a leading `"[Bot"` prefix; then within the remainder find `"] "` and take the
  substring after it (handles both `[Bot] ...` and `[Bot#12] ...`). If no such prefix,
  use the whole msg.
- Return `body.starts_with("GameUpdatePacket")`.
- Rationale (from the code comment): per-frame `GameUpdatePacket` lines flood the console;
  suppress from stdout but keep in the file.

### Nxrth port
- `logger::init()` writes the banner to `nxrth_debug.log`.
- `logger::log(msg)` appends `[timestamp] msg\n` to the file. In C++ format time with
  `std::chrono` / `strftime("%Y-%m-%d %H:%M:%S")`.
- **The stdout print becomes an ImGui console feed**: push the line into the console
  panel's ring buffer *unless* `is_console_spam(msg)` — spam lines go to the file (and,
  optionally, a "verbose"/debug tab) but not the main live console. Preserve the exact
  `[Bot` / `] ` parsing and the `GameUpdatePacket` prefix test.
- **Thread-safety:** the Rust reopens the file per call (implicitly serialized by the OS).
  In C++, guard the log file + console buffer with a `std::mutex` since many bot threads
  call `log()` concurrently. Do not open/close the file per call in the hot path if it
  hurts; a single mutex-guarded stream is fine, but keep append semantics and never
  truncate.
- Do **not** panic on log IO errors in Nxrth (the Rust `.unwrap()`s do); degrade quietly.

---

## 4. THREADING & SHARED STATE

### 4.1 Mori model
- **Runtime:** single Tokio multithreaded runtime (`#[tokio::main]`). All HTTP handlers
  are async tasks; blocking work (`run_proxy_test`, `ureq` CDN fetch) is offloaded with
  `tokio::task::spawn_blocking`.
- **`BotManager`:** one instance, `Arc<Mutex<BotManager>>`, shared by every handler
  (`AppState` is `Clone`; the `Arc` is cloned). It owns the bot registry, `items_dat`,
  `proxy_key_counts()`, and spawns each bot (bots run on their own threads — see `bot`
  spec).
- **Event bus:** `tokio::sync::broadcast::channel::<WsEvent>(256)`. `ws_tx` is cloned into
  `BotManager` (producers = bots/manager) and into `AppState`. Each `/ws` connection calls
  `ws_tx.subscribe()` → its own `broadcast::Receiver`. Fan-out to all connected clients.
- **`ProxyPool`:** `Arc<Mutex<ProxyPool>>` shared by the pool endpoints and spawn glue.
- **`AuthState`:** `Clone` (internally `Arc`), shared.
- **Process-global registries (no `AppState`):** `rotation_pool::{snapshot,clear,…}` and
  `account_devices::upsert_from_login_token` are module-level global/disk state.

### 4.2 Fleet-wide shared state (Nxrth must keep bots aware of each other)
The following are inherently cross-bot and must live in **one central, mutex-guarded
place** (`BotManager` or dedicated singletons) so every bot/panel sees the same view:
- **`proxy_key_counts()`** — live per-proxy bot tally; drives `ProxyPool::choose` and
  `max_bots_per_ip` spreading. Must update on every spawn/stop.
- **`ProxyPool`** (incl. rotating login proxy) — shared config + selection state.
- **`rotation_pool`** — per-`pool_id` rotation state (global).
- **`account_devices`** — shared credential/device store (disk-backed).
- **The event bus** — a single fan-out feed all UI panels subscribe to.

### 4.3 Nxrth model
- Replace Tokio with **`std::thread`**: each bot runs on its own thread (as in Mori's
  `bot` module); the **UI runs on the main thread** (Win32 message pump + DX11 present
  loop). There is no server accept loop.
- **`BotManager`** stays a single object guarded by `std::mutex`. UI panels lock it
  briefly to read `list()`/`get_state()`/counts and to call `spawn*`/`stop`/`send_cmd`.
  Keep lock hold-time short (copy out what you render) to avoid stalling bot threads.
- **Event bus** = the crossbeam-channel replacement mandated project-wide: a
  `std::mutex + std::condition_variable` bounded queue with **fan-out** to N consumers and
  **capacity 256, drop-oldest-on-lag** to mirror tokio broadcast `Lagged` (see §5.6). In
  practice for a single-process UI you can have **one** consumer (the UI thread) that
  drains the queue each frame into panel ring buffers; producers are bot threads +
  `logger::log`. If you keep the multi-consumer shape, each consumer holds its own read
  cursor.
- **Blocking work** (`run_proxy_test`, optional CDN fetch) → detached worker
  `std::thread`s that post results back via the event bus or a `std::future`/promise the
  panel polls.
- **No poison semantics** in `std::mutex`; just don't throw while locked.

---

## 5. DEPENDENCY MAPPING (Rust crate → Nxrth C++)

| Rust (this module) | Used for | Nxrth C++ replacement |
|---|---|---|
| `axum` (Router, routing, extract, ws, Json, Query, Path, State) | HTTP+WS server, routing, request parsing | **Removed.** Dear ImGui panels call handler-equivalent functions directly. Path/Query/JSON body parsing → ImGui form state structs. |
| `tower` / `tower-http` (`CorsLayer`, `ServeDir`, `ServeFile`) | CORS + static SPA serving | **Removed.** No CORS (in-process). No static file serving — UI is compiled in. |
| `tokio` (`#[tokio::main]`, `broadcast::channel(256)`, `TcpListener`, `net`, `fs`, `task::spawn_blocking`) | async runtime, event bus, socket bind, async fs, offload blocking | `std::thread` + a **mutex+condvar bounded(256) fan-out queue** for the bus; `std::filesystem`/`std::ofstream` for fs; worker `std::thread`s for blocking calls. No socket bind. |
| `tokio::sync::broadcast` | fan-out `WsEvent` to `/ws` clients | mutex+condvar queue, drop-oldest at cap 256 (mirrors `Lagged`). |
| `crossbeam-channel` (project-wide) | inter-thread queues | `std::mutex + std::condition_variable` queue (mandated). |
| `serde` / `serde_json` (`Json`, `json!`, `to_string`, `to_string_pretty`) | (de)serialize DTOs, WsEvent, geiger files | **`nlohmann/json`** for the JSON that survives (geiger `.json`, account-device import parsing, any config). DTOs become plain C++ structs. |
| `ureq` (`get(url).call()`, `into_body().read_to_vec()`) | CDN asset fetch | **libcurl** (with `socks5h`, cookie support per project convention). This specific fetch can be dropped in native. |
| `chrono` (`Local::now().format(...)`) | timestamps (log, geiger filename, banner) | `std::chrono` + `strftime`/`std::format`. Formats: log/banner `"%Y-%m-%d %H:%M:%S"`; geiger stamp `"%Y%m%d-%H%M%S-%3f"` (3-digit ms). |
| `std::sync::{Arc, Mutex}`, `PoisonError::into_inner` | shared state | `std::shared_ptr` + `std::mutex` (no poison recovery needed). |
| **Internal crates referenced** (own module specs): `bot_manager::BotManager`/`BotInfo`, `bot::Socks5Config`, `bot_state::{BotCommand,BotDelays,BotState}`, `events::{WsTx,WsEvent}`, `items::{ItemInfo,bgra_to_rgb}`, `proxy_pool::{ProxyPool,ProxyPoolView,RotatingLoginProxy}`, `proxy_test::{ProxyTestResult,run_proxy_test}`, `rotation_pool::{snapshot,clear,RotationPoolSnapshot}`, `account_devices::upsert_from_login_token`, `auth::AuthState`, `lua::run_script_threaded` | see their specs | Port per their own module specs. `AuthState` → `argon2` lib. `run_script_threaded` → **removed (no Lua)**. |

---

## 6. LUA SUBSYSTEM ENTRY (`lua/mod.rs`) — REMOVED in Nxrth

`lua/mod.rs` is a module aggregator:
```rust
mod geiger_stats; mod http; mod runtime; mod types; mod webhook;
pub use runtime::run_script_threaded;
```
It re-exports **`run_script_threaded`** — the entry point that runs a Lua script on its
own thread; this is what the `RunScript`/`run_script` command ultimately drives, and
`geiger_stats` is the traffic/geiger stats collector exposed to Lua.

**Nxrth has NO Lua** (mandated). Therefore:
- Drop `mlua` and the entire `lua/` tree.
- `run_script`/`stop_script` commands → native automation start/stop (no script source).
- Geiger stats/traffic capture (`geiger_stats`) → native C++ ENet packet tap feeding the
  Traffic panel and the `POST /geiger/captures`-equivalent save (§2.11).
- Lua `http`/`webhook` helpers → native libcurl calls if those automations are re-created.

---

## 7. BOT MODULE ENTRY (`bot/mod.rs`) — re-exports only

```rust
mod auth; mod core; mod shared;
pub use core::Bot;
pub use shared::{BotEventRaw, Socks5Config};
```
No logic here. The UI/glue only needs `Socks5Config` (see §2.9). `Bot`, `BotEventRaw`, and
the auth/core internals are covered by the `bot` module spec. In Nxrth keep the same
split: a `Bot` class (its own thread), a shared types header (`Socks5Config`,
`BotEventRaw`), and the login/auth code.

---

## 8. IMGUI PANEL CHECKLIST (implementation punch-list)

Each item = one ImGui panel/control replacing a dashboard feature. `▢` = to build.

- ▢ **Login gate** (optional): setup password (first run) / unlock / lock. §2.1
- ▢ **Bots table**: rows from `manager.list()` (`BotInfo`); select row to focus. §2.2
- ▢ **Add-bot form** with 5-mode login selector (Standard / Newly / Requestly / Ltoken /
  HAR-token) + proxy block + "Use proxy pool" checkbox. §2.2/§2.9
- ▢ **Stop/Remove bot** button (per row). §2.2
- ▢ **Account-device bulk import** dialog (username+login_token pairs). §2.3
- ▢ **Bot control bar**: Disconnect, Reconnect, Accept access, Warp (name+id). §2.4
- ▢ **Movement**: raw Move (x,y i32) + click-to-WalkTo (tile x,y u32) on world view. §2.4
- ▢ **Automation toggles**: Auto-collect on/off; Collect config (radius_tiles u8 slider +
  item blacklist `Vec<u16>` editor); Auto-reconnect on/off; Delays (`SetDelays`). §2.4
- ▢ **Inventory/items panel**: Wear/Unwear/Drop(count)/Trash(count) using the item picker. §2.4/§2.5
- ▢ **Item picker**: search (`q`), 50/page pagination, name map, color swatches
  (`bgra_to_rgb`, even=block/odd=seed rule). §2.5
- ▢ **World view**: render from `BotState` (via `get_state(id)`) + live `WsEvent`s. §2.4 / events spec
- ▢ **Console stream**: live feed from the event bus + `logger::log`, with
  `is_console_spam` suppression of `GameUpdatePacket` lines from the main view. §3/§5
- ▢ **Traffic / Geiger monitor**: live packet table (id/direction/kind/size/summary/detail
  /timestamp) + `filter` + **Save capture** → `data/geiger_captures/geiger_bot{id}_{stage}_{stamp}.{json,txt}`. §2.11
- ▢ **Proxy tester**: host/port/user/pass + "Test" → `run_proxy_test` on worker thread. §2.6
- ▢ **Proxy pool editor**: enable / max_bots_per_ip / spread_mode / proxies_text /
  rotating-login (enable, scheme, port_span=2000, proxy_text) + live utilization view. §2.7
- ▢ **Rotation pool viewer**: per-`pool_id` snapshot + Clear. §2.8
- ▢ **About** (optional): credit line. §0
- ▢ **REMOVED**: Lua script console (`run_script` source box), CDN proxy, static SPA
  serving, HTML footer injection, HTTP/WS transport. §2.10/§2.12/§2.13

---

## 9. EXACT CONSTANTS & MAGIC STRINGS TO PRESERVE

- Log file: `mori_debug.log` → **`nxrth_debug.log`**. Banner:
  `\n--- Log Started at {YYYY-MM-DD HH:MM:SS} ---`. Line: `[{YYYY-MM-DD HH:MM:SS}] {msg}`.
  Spam prefix test: strip `[Bot` … `] `, then `starts_with("GameUpdatePacket")`.
- Event-bus capacity: **256**.
- HTTP bind (removed): `0.0.0.0:3000`. Startup prints: `Dashboard http://localhost:3000`,
  `WebSocket ws://localhost:3000/ws`, `API http://localhost:3000/bots`.
- Items page size: **50** (`ITEMS_PAGE_SIZE`). Items query param rename: `get-items`.
- Default HAR path: **`requestly_logs.har`**.
- Rotating-login default port span: **2000**.
- Geiger dir: `<cwd>/data/geiger_captures/`; filename base
  `geiger_bot{bot_id}_{stage}_{stamp}`; stamp `%Y%m%d-%H%M%S-%3f`; sanitize keeps
  `[A-Za-z0-9_-]`, trims `_`, empty→`stage`; stage empty → `400 "stage is required"`.
- CDN base (removed/optional): `https://growserver-cache.netlify.app/{path}`; default
  content-type `application/octet-stream`; upstream errors → `502`.
- CORS (removed): origins `Any`, methods `GET/POST/PUT/DELETE`, headers `Any`.
- Command tag: internally-tagged `{"type": "<snake_case>", …}`; 17 variants (§2.4).
- Error envelope: `{"error": "<message>"}`; validation → `400`, conflict → `409`,
  auth fail → `401`, missing bot → `404`, IO/internal → `500`, upstream → `502`.

---

## 10. FILES / PATHS TOUCHED BY THIS MODULE

- Reads/serves (removed in Nxrth): `<cwd>/dist/index.html` and `<cwd>/dist/**` (SPA).
- Writes: `<cwd>/mori_debug.log` → `nxrth_debug.log`;
  `<cwd>/data/geiger_captures/*.json` and `*.txt`.
- Reads (HAR spawn): `requestly_logs.har` (default; passed to `spawn_har_token`).
- Global/disk state via free functions: `account_devices` store, `rotation_pool` registry,
  `proxy_pool` default load (`ProxyPool::load_default()` in `serve()`).
