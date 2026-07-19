# Nxrth Port Spec — Module 04: Proxy

**Source (Rust / Mori):** `src/proxy_pool.rs`, `src/rotation_pool.rs`, `src/proxy_test.rs`
**Target (C++ / Nxrth):** proxy subsystem (game-world proxy pool + rotating/bypass login proxy pool + world-rotation coordination + proxy self-test)

This document is the **single source of truth** for the C++ port of this module. It is self-contained: an engineer can reimplement it without reading the Rust. Referenced external types (`Socks5Config`, `LoginInfo`, `ServerData`, `Socks5UdpSocket`) are documented at the interface level where they cross this module's boundary.

> The three source files cover three loosely related concerns:
> 1. **`proxy_pool.rs`** — the proxy configuration model: the *game/world* proxy pool (per-bot assigned SOCKS5 exit for the subserver connection) and the *rotating/bypass login* proxy pool (per-login-attempt clean exit IP, IP-pinned for the token+ENet logon pair).
> 2. **`rotation_pool.rs`** — a fleet-wide, on-disk *world*-claiming coordination table (bots claim/lock/release worlds so multiple bots don't collide on the same world). This is NOT about network proxies; it is shared-state coordination and is included here because it is fleet-wide "who is doing what" state.
> 3. **`proxy_test.rs`** — a 3-stage proxy self-test (SOCKS5 UDP associate → server_data fetch → ENet connect handshake).

---

## 0. GLOBAL CONSTANTS (referenced by this module)

From `src/constants.rs` (used by `proxy_test.rs`):

```
PROTOCOL   : u32  = 226      // Growtopia protocol version
GAME_VER   : &str = "5.51"   // Growtopia client/game version string
```

Growtopia wire data is **little-endian**. This module itself does not emit Growtopia binary packets (that is `proxy_test`'s ENet dependency), but the constants above flow into the `LoginInfo` it passes to the server-data fetch.

Magic/tuning constants that appear literally in this module (preserve exactly):

| Constant | Value | Where | Meaning |
|---|---|---|---|
| default rotating-login scheme | `"auto"` | `default_rotating_login_scheme()` | scheme when unspecified |
| default rotating-login port span | `2000` | `default_rotating_login_port_span()` | sticky-range width default |
| DataImpulse plain-HTTP gateway port | `823` | `infer_proxy_scheme_from_port` | inferred as `http` |
| DataImpulse SOCKS5 gateway port | `824` | (implicit) | inferred as `socks5h` (the `_ =>` branch) |
| DataImpulse sticky-session port range | `10000..=20000` (inclusive) | `normalized_rotating_port_span` | only range that gets a real per-session port span |
| port-range expansion cap | `2000` | `expand_proxy_line` | max entries a single `START-END` line may expand to |
| min bots-per-ip | `1` | `max(1)` clamps | capacity floor |
| lock TTL floor | `30` (seconds) | `claim_world` `lock_ttl_sec.max(30)` | minimum world-lock lifetime |
| SOCKS5 control connect timeout | `10s` | `bind_through_proxy` | TCP connect to proxy |
| SOCKS5 control r/w timeout | `15s` | `bind_through_proxy` | read/write on control stream |
| ENet connect wait deadline | `10s` | `check_enet` | wait for ServerHello/Connect event |
| ENet service poll sleep | `10ms` | `check_enet` | loop sleep |
| proxy-test server_data first-call | `alternate=false`, retry `alternate=true` | `run_proxy_test` | two endpoints |
| ENet host settings | `peer_limit=1, channel_limit=2, compressor=RangeCoder, checksum=crc32, using_new_packet=true` | `check_enet` | must match Growtopia gateway |
| ENet connect args | `host.connect(server_addr, channel_count=2, data=0)` | `check_enet` | 2 channels |

---

## 1. RENAME RULES (apply across the whole Nxrth port)

Global substitution rules for the port (identifiers, file paths, log lines, window titles, user-agents, config filenames):

- `Mori` → `Nxrth`, `mori` → `nxrth`
- `Cloei` → `North`, `cloei` → `north` (upstream author/repo name)

**Occurrences inside these three files:** none. There are **no** literal `Mori`/`mori`/`Cloei`/`cloei` tokens in `proxy_pool.rs`, `rotation_pool.rs`, or `proxy_test.rs`. The on-disk filenames are generic and are **kept as-is** (they carry no brand):
- `data/proxy_pool.json`
- `rotation_pool_state.json`

Do **not** rename `DataImpulse`, `Growtopia`, `ltoken`, `ServerHello`, `SOCKS5`, `ENet` — these are third-party/protocol terms, not Mori/Cloei branding.

> Rename obligations for this module are therefore inherited only indirectly: the *data root directory* is resolved as `current_dir()/data/...` (see §4). If the Nxrth fleet relocates its data directory or brands log lines emitted by callers of this module, apply the rules there. The module's own code emits no branded strings.

---

## 2. EXTERNAL TYPE: `Socks5Config` (boundary type, from `src/bot/shared.rs`)

This is the currency this module produces and consumes. Reproduce it verbatim in Nxrth.

```rust
pub struct Socks5Config {
    pub proxy_addr: SocketAddr,     // resolved IP:port of the SOCKS5 proxy
    pub username: Option<String>,   // SOCKS5 auth user (None = no auth)
    pub password: Option<String>,   // SOCKS5 auth pass
}
impl Socks5Config {
    pub fn to_url(&self) -> String {
        // (Some,Some) -> "socks5://{u}:{p}@{proxy_addr}"
        // otherwise   -> "socks5://{proxy_addr}"
    }
}
```

**C++ shape:**
```cpp
struct Socks5Config {
    // proxy_addr is a *resolved* endpoint (IP + port), NOT a hostname.
    // Store as (std::string ip, uint16_t port) or a sockaddr; the Rust SocketAddr
    // is already-resolved. Display form is "IP:port".
    std::string ip;
    uint16_t    port;
    std::optional<std::string> username;
    std::optional<std::string> password;

    std::string to_url() const; // socks5://[user:pass@]ip:port
    // Equality for rotation is by proxy_addr only (ip:port), see next_game_proxy.
};
```
`to_url()`: if both username and password present → `socks5://{user}:{pass}@{ip}:{port}` else `socks5://{ip}:{port}`. Note: this always uses the `socks5://` scheme literal (not `socks5h`), because it targets the ENet/UDP SOCKS5 path.

---

## 3. FILE `proxy_pool.rs` — TYPES

### 3.1 `ProxySpreadMode` (enum)

```rust
#[serde(rename_all = "snake_case")]
pub enum ProxySpreadMode { LeastLoaded, RoundRobin }   // Default = LeastLoaded
```

- **JSON serialization** (serde `rename_all="snake_case"`): `LeastLoaded` → `"least_loaded"`, `RoundRobin` → `"round_robin"`.
- `from_str(&str)`: `"round_robin"` → `RoundRobin`; **anything else** → `LeastLoaded` (default-on-unknown).
- `as_str()`: `LeastLoaded` → `"least_loaded"`, `RoundRobin` → `"round_robin"`.

C++: `enum class ProxySpreadMode { LeastLoaded, RoundRobin };` plus the two string mappers with the exact fallbacks above.

### 3.2 `ProxyEntry` (struct)

```rust
pub struct ProxyEntry {
    pub host: String,               // hostname OR IP literal, as typed
    pub port: u16,
    pub username: Option<String>,
    pub password: Option<String>,
    pub scheme: Option<String>,     // per-entry scheme override (from URL form); None otherwise
    pub raw: String,                // the exact source text this entry was parsed/expanded from
}
```
Serde: plain object, all fields serialized by their field names; `Option`=`null` when absent. `raw` is persisted (used to re-render the textarea in the UI view).

**Methods (all private except `to_proxy_url`):**

- `to_socks5() -> Result<Socks5Config>`: resolve `host:port` to a `SocketAddr`.
  1. Build `hostport = "{host}:{port}"`.
  2. Try `hostport.parse::<SocketAddr>()` (fast path, IP literal, no DNS).
  3. On parse error → `hostport.to_socket_addrs()?` (DNS resolve) → take `.next()`; if none → error `"could not resolve proxy {hostport}"`.
  4. Return `Socks5Config { proxy_addr, username.clone(), password.clone() }`.
  - **C++:** `getaddrinfo` (or `inet_pton` fast path then `getaddrinfo`); pick first result. Preserve the "IP literal first, DNS fallback" ordering — a hostname endpoint (e.g. `gw.dataimpulse.com`) must NOT be silently dropped.

- `capacity_key() -> Result<String>`: `to_socks5()` then format `"{ip}:{port}"` from the **resolved** `proxy_addr` (ip and port separately, so it's the numeric IP). This is the **key used in the active-count map** and stored per-bot as its `proxy_key`. Two entries that resolve to the same IP:port share a capacity bucket.

- `label() -> String`: if `username` present and non-empty → `"{host}:{port} ({username})"`, else `"{host}:{port}"`. (Display only.)

- `to_proxy_url(default_scheme: &str) -> Result<String>` **(pub)**: build a URL string for HTTP-client proxying.
  1. `scheme = effective_proxy_scheme(default_scheme, self)` (see §5).
  2. Parse `"{scheme}://{host}:{port}"` as a URL.
  3. If `username` present → set userinfo username (error `"invalid proxy username"` on failure).
  4. If `password` present → set password (error `"invalid proxy password"`).
  5. Return the URL string (percent-encoded userinfo as the URL library produces).
  - **C++:** construct `"{scheme}://[user[:pass]@]host:port"`, URL-encoding `user`/`pass`. The scheme here may be `http`, `socks5`, or `socks5h` (remote DNS) — libcurl uses `socks5h://` to force proxy-side DNS.

### 3.3 `ProxyPoolConfig` (struct — the persisted config; on-disk `data/proxy_pool.json`)

```rust
pub struct ProxyPoolConfig {
    pub enabled: bool,                                  // game pool on/off
    pub max_bots_per_ip: usize,                         // capacity per resolved IP:port
    pub spread_mode: ProxySpreadMode,                   // least_loaded | round_robin
    pub proxies: Vec<ProxyEntry>,                       // the GAME/world proxy pool
    pub next_index: usize,                              // persisted round-robin cursor for choose()
    #[serde(default)] pub rotating_login_enabled: bool,
    #[serde(default = default_rotating_login_scheme)]      pub rotating_login_scheme: String,   // default "auto"
    #[serde(default = default_rotating_login_port_span)]   pub rotating_login_port_span: u16,   // default 2000
    #[serde(default)] pub rotating_login_proxy: Option<ProxyEntry>,   // LEGACY single bypass proxy (migration only)
    #[serde(default)] pub rotating_login_proxies: Vec<ProxyEntry>,    // bypass login pool (one per line)
}
```

**Defaults (`Default` impl — also the values written when the file is missing/corrupt):**
```
enabled=false, max_bots_per_ip=1, spread_mode=LeastLoaded, proxies=[], next_index=0,
rotating_login_enabled=false, rotating_login_scheme="auto",
rotating_login_port_span=2000, rotating_login_proxy=None, rotating_login_proxies=[]
```

**Serde `#[serde(default)]` semantics (critical for C++ JSON parsing — treat these as optional-with-default when reading):**
- Missing `rotating_login_enabled` → `false`.
- Missing `rotating_login_scheme` → `"auto"`.
- Missing `rotating_login_port_span` → `2000`.
- Missing `rotating_login_proxy` → `null`/absent.
- Missing `rotating_login_proxies` → `[]`.
- The first five fields (`enabled`…`next_index`) have **no** `#[serde(default)]` — strictly they are required, but because the whole file falls back to `Default` on any parse error, a partial/old file that lacks them just resets everything to defaults. In practice: parse leniently; if a required field is missing, fall back to the full default config.

**Legacy migration** (performed in `load_default`): if `rotating_login_proxies` is empty AND `rotating_login_proxy` (singular) is `Some`, move the singular into the vec (`push`), leaving the field to be nulled on next save. C++ must replicate this one-time migration when loading old files.

### 3.4 `ProxyPoolView` / `ProxyPoolEntryView` (UI/serialize-only)

These are read-model DTOs the UI renders. In Nxrth (Dear ImGui, no web server) these become the data the proxy panel reads; keep the field set so the UI logic ports 1:1.

```rust
pub struct ProxyPoolView {
    pub enabled: bool,
    pub max_bots_per_ip: usize,
    pub spread_mode: String,                       // as_str()
    pub proxies_text: String,                      // game entries' `raw` joined by "\n"
    pub total: usize,                              // proxies.len()
    pub available: usize,                          // count of entries not at capacity
    pub active: usize,                             // sum of active bots across distinct IPs
    pub proxies: Vec<ProxyPoolEntryView>,
    pub rotating_login_enabled: bool,
    pub rotating_login_scheme: String,             // normalized scheme
    pub rotating_login_effective_scheme: String,   // effective scheme of first bypass entry
    pub rotating_login_port_span: u16,             // effective span (1 unless single sticky-range entry)
    pub rotating_login_proxy_text: String,         // bypass entries' `raw` joined by "\n"
    pub rotating_login_proxy_label: Option<String>,// see below
}

pub struct ProxyPoolEntryView {
    pub index: usize,       // position in proxies vec
    pub label: String,      // ProxyEntry::label()
    pub ip: String,         // capacity_key() resolved "ip:port"
    pub active: usize,      // active bots on this IP
    pub capacity: usize,    // max_bots_per_ip (>=1)
    pub full: bool,         // active >= capacity
}
```

`rotating_login_proxy_label` computed by count of `rotating_login_proxies`:
- `0` → `None`
- `1` → `Some(rotating_login_label(entry))` (see §4 method)
- `n>1` → `Some("{n} bypass proxies (random per login)")`

### 3.5 `ProxyPool` (the stateful manager)

```rust
pub struct ProxyPool {
    path: PathBuf,            // resolved to <cwd>/data/proxy_pool.json
    config: ProxyPoolConfig,
}
```
Not serialized. Owns the config + its file path. C++: a class holding `std::filesystem::path` + `ProxyPoolConfig`, with a mutex if shared across threads (see §7).

### 3.6 `RotatingLoginProxy` (the per-attempt bypass picker — cloned into each bot)

```rust
pub struct RotatingLoginProxy {
    pool: Vec<ProxyEntry>,      // snapshot of rotating_login_proxies
    span: u16,                  // effective port span (1 for multi-entry pools)
    configured_scheme: String,  // e.g. "auto"/"socks5"/"socks5h"/"http"
}
```
Cloneable value object; each bot holds its own copy. Picking uses the **process-global** round-robin cursor `LOGIN_PROXY_RR` (see §7), so clones still coordinate.

### 3.7 `BypassLoginSession` (one pinned exit IP for a single login attempt)

```rust
pub struct BypassLoginSession {
    pub http_url: String,     // SOCKS5 URL for the HTTP token fetch (libcurl)
    pub enet: Socks5Config,   // resolved SOCKS5-UDP config for the ENet logon
}
```
**Why it exists (IP pinning):** the Growtopia `ltoken` is bound to the IP that minted it. The HTTP growid/dashboard fetch and the `protocol|{PROTOCOL}` ENet logon MUST egress from the **same** exit IP. `login_session()` mints both halves from one `pick()` so they share a single sticky port/exit. C++ must keep this invariant: derive `http_url` and `enet` from **the same** picked entry, never two separate picks.

---

## 4. FILE `proxy_pool.rs` — FUNCTIONS

### 4.1 Process-global state (top of file)

```rust
static LOGIN_PROXY_RR:  AtomicUsize = 0;                        // rr cursor for bypass login pool
static GAME_PROXY_POOL: OnceLock<Mutex<Vec<Socks5Config>>>;     // published snapshot of game pool
static GAME_PROXY_RR:   AtomicUsize = 0;                        // rr cursor for game-proxy failover
```
See §7 for the C++ mapping. All three are shared across every bot in the process.

### 4.2 `publish_game_pool(config: &ProxyPoolConfig)` (private)

Rebuild the global game-proxy snapshot:
1. Map every `config.proxies` entry through `to_socks5()`, **dropping** entries that fail to resolve (`filter_map(... .ok())`).
2. Lock `GAME_PROXY_POOL` (init to empty vec on first use) and overwrite it with the resolved list.
- Called on pool **load** and on **every edit** (`update`).
- **Poison handling:** `unwrap_or_else(PoisonError::into_inner)` — i.e., ignore mutex poisoning and use the inner value anyway. C++ has no poisoning; a plain `std::mutex` lock suffices.

### 4.3 `next_game_proxy(current: Option<&Socks5Config>) -> Option<Socks5Config>` **(pub)**

Round-robin the next *different* game proxy (used to swap a dead subserver tunnel):
1. Get `GAME_PROXY_POOL`; if never published → `None`.
2. Lock; `len = pool.len()`. If `len < 2` → `None` (nothing to rotate to).
3. `cur_addr = current.map(|c| c.proxy_addr)` (compare by resolved IP:port only).
4. Loop up to `len` times: `idx = GAME_PROXY_RR.fetch_add(1) % len`; if `pool[idx].proxy_addr != cur_addr` → return `pool[idx].clone()`.
5. If all `len` slots equal `cur_addr` → `None`.
- **Edge:** advancing the shared cursor even on skipped slots is intentional (keeps global spread). C++: `std::atomic<size_t> fetch_add(1, relaxed)`; compare `Socks5Config` by `(ip,port)`.

### 4.4 `ProxyPool::load_default() -> Self` **(pub)**

1. `path = current_dir() (fallback ".") / "data" / "proxy_pool.json"`.
2. Read file to string; parse JSON to `ProxyPoolConfig`; on **any** failure (missing file, bad JSON) → `ProxyPoolConfig::default()`.
3. Legacy migration (§3.3): if `rotating_login_proxies` empty and `rotating_login_proxy` is `Some`, take it and push into the vec.
4. `publish_game_pool(&config)`.
5. Return `ProxyPool { path, config }`.

### 4.5 `ProxyPool::view(active_counts: &HashMap<String, usize>) -> ProxyPoolView` **(pub)**

Builds the read-model. `active_counts` maps `capacity_key` (`"ip:port"`) → number of currently-active bots on that IP.
1. `capacity = max(max_bots_per_ip, 1)`.
2. For each game entry (with index): compute `ip = capacity_key()` (skip entry if it fails to resolve), `active = active_counts.get(ip).unwrap_or(0)`, build `ProxyPoolEntryView { index, label(), ip, active, capacity, full = active>=capacity }`.
3. `available = count(entries where !full)`.
4. `active` (total) = sum of active over **distinct** IPs (dedupe via a temp map keyed by ip → active, then sum values). Note: because a single IP can back multiple entries, this dedup avoids double counting.
5. `proxies_text` = join of every game entry's `raw` by `"\n"`.
6. Rotating/bypass view fields:
   - `rotating_login_scheme` = `normalize_proxy_scheme(config.rotating_login_scheme)`.
   - `rotating_login_effective_scheme` = if there's a first bypass entry → `effective_proxy_scheme(config.rotating_login_scheme, first)`, else `normalize_proxy_scheme(config.rotating_login_scheme)`.
   - `rotating_login_port_span` = if **exactly one** bypass entry → `normalized_rotating_port_span(entry.port, config.rotating_login_port_span)`, else `1`.
   - `rotating_login_proxy_text` = join bypass `raw` by `"\n"`.
   - `rotating_login_proxy_label` = per §3.4 rule.

### 4.6 `ProxyPool::update(...) -> Result<()>` **(pub)** — edit + persist

Signature:
```rust
fn update(&mut self,
    enabled: bool,
    max_bots_per_ip: usize,
    spread_mode: &str,
    proxies_text: &str,
    rotating_login_enabled: bool,
    rotating_login_scheme: &str,
    rotating_login_port_span: u16,
    rotating_login_proxy_text: &str,
) -> anyhow::Result<()>
```
Steps:
1. `proxies = parse_proxy_lines(proxies_text)?` (fails whole update if any non-blank line is invalid).
2. `config.enabled = enabled`.
3. `config.max_bots_per_ip = max(max_bots_per_ip, 1)`.
4. `config.spread_mode = ProxySpreadMode::from_str(spread_mode)`.
5. `config.proxies = proxies`.
6. `publish_game_pool(&config)` (refresh global snapshot).
7. `config.next_index = min(next_index, proxies.len())` (clamp cursor after edit).
8. `config.rotating_login_enabled = rotating_login_enabled`.
9. `config.rotating_login_scheme = normalize_proxy_scheme(rotating_login_scheme).to_string()`.
10. `config.rotating_login_proxies = parse_proxy_lines(rotating_login_proxy_text)?`.
11. `config.rotating_login_proxy = None` (clear the legacy field).
12. `config.rotating_login_port_span`: if exactly one bypass entry → `normalized_rotating_port_span(only.port, rotating_login_port_span)`; else → `max(rotating_login_port_span, 1)`.
13. `self.save()` (write pretty JSON; create parent dir if needed).

### 4.7 `ProxyPool::choose(active_counts) -> Result<Option<Socks5Config>>` **(pub)** — assign a game proxy to a bot

```rust
fn choose(&mut self, active_counts: &HashMap<String, usize>) -> anyhow::Result<Option<Socks5Config>>
```
1. If `!config.enabled` → `Ok(None)` (no proxy; bot goes direct).
2. If `config.proxies.is_empty()` → **Err** `"proxy pool is enabled but empty"`.
3. `capacity = max(max_bots_per_ip, 1)`.
4. Build `candidates: Vec<(index, ip, active)>` = entries whose `capacity_key()` resolves AND `active < capacity`. (Entries failing to resolve are skipped.)
5. If `candidates` empty → **Err** `"no proxy is available under the current bots-per-proxy limit"`.
6. Pick `index` by `spread_mode`:
   - **LeastLoaded:**
     1. `min_active = min(active over candidates)`.
     2. `len = proxies.len()`, `start = min(next_index, len-1)`.
     3. Scan offsets `0..len`, `cand = (start+offset)%len`; pick the **first** `cand` that is a candidate with `active == min_active`. (Guaranteed to exist → `.unwrap()`.)
     4. `next_index = (picked+1)%len`; `save()?`.
   - **RoundRobin:**
     1. `len`, `start = min(next_index, len-1)`.
     2. Scan offsets `0..len`; pick first `cand=(start+offset)%len` that is any candidate.
     3. `next_index = (picked+1)%len`; `save()?`.
7. Return `config.proxies[index].to_socks5().map(Some)`.
- **Note:** both modes advance & persist `next_index` on each successful pick (fair rotation across restarts). LeastLoaded biases to the lowest-load IP but breaks ties by the rotating cursor so it also spreads. C++: replicate the modular scan-from-cursor exactly; the tie-break ordering is observable behavior.

### 4.8 `ProxyPool::rotating_login_proxy() -> Result<Option<RotatingLoginProxy>>` **(pub)**

1. If `!config.rotating_login_enabled` → `Ok(None)`.
2. If `config.rotating_login_proxies.is_empty()` → **Err** `"rotating login proxy is enabled but empty"`.
3. `span` = if exactly one entry → `normalized_rotating_port_span(only.port, config.rotating_login_port_span)`; else → `1`.
4. Return `Some(RotatingLoginProxy { pool: config.rotating_login_proxies.clone(), span, configured_scheme: config.rotating_login_scheme.clone() })`.

### 4.9 `ProxyPool::rotating_login_label(&self, proxy) -> String` (private, display)

`span = max(config.rotating_login_port_span, 1)`. If `span==1` → `proxy.label()`. Else `end = min(port + (span-1), u16::MAX)` (saturating add) and:
- user present & non-empty → `"{host}:{port}-{end} ({username})"`
- else → `"{host}:{port}-{end}"`

### 4.10 `ProxyPool::save(&self) -> Result<()>` (private)

1. If `path.parent()` exists → `create_dir_all(parent)`.
2. `json = serde_json::to_string_pretty(&config)`.
3. `write(path, json)`.
- C++: `std::filesystem::create_directories(parent)` then write nlohmann pretty JSON (`dump(2)` or `dump(4)`; Rust `to_string_pretty` uses 2-space indent — match if byte-for-byte fidelity matters, otherwise indentation is cosmetic).

### 4.11 `RotatingLoginProxy::pick(&self) -> ProxyEntry` (private)

The core rotation primitive:
1. `len = pool.len()`.
2. `idx = if len > 1 { LOGIN_PROXY_RR.fetch_add(1, Relaxed) % len } else { 0 }`.
3. `proxy = pool[idx].clone()`.
4. `span = if len > 1 { 1 } else { self.span }` — **multi-entry pools force span=1** (each entry is a fixed endpoint; rotation comes from the round-robin entry pick). Single entry uses the configured sticky span.
5. `proxy.port = random_rotating_port(proxy.port, span)`.
6. Return `proxy`.
- **Rationale:** with N entries, N concurrent logins land on N different ports/exit-IPs deterministically (no random collisions). With 1 entry (DataImpulse sticky range), the port is rolled randomly within the span to get a fresh sticky exit.

### 4.12 `RotatingLoginProxy::fresh_url(&self) -> String` **(pub)** — bypass HTTP proxy URL

1. `proxy = pick()`.
2. `scheme = normalize_proxy_scheme(configured_scheme)`.
3. `proxy.to_proxy_url(scheme)`; on error, **fallback** manual formatting:
   - `effective = effective_proxy_scheme(scheme, &proxy)`
   - both user&pass → `"{effective}://{u}:{p}@{host}:{port}"`, else `"{effective}://{host}:{port}"`.
- Returns a proxy URL for a *fresh exit IP each call*. Used where only an HTTP fetch is needed (no paired ENet).

### 4.13 `RotatingLoginProxy::login_session(&self) -> Option<BypassLoginSession>` **(pub)** — IP-pinned pair

1. `proxy = pick()` (single pick → single exit IP for both halves).
2. `scheme = effective_proxy_scheme(configured_scheme, &proxy)`.
3. If `scheme` is **not** `"socks5"` or `"socks5h"` → `None` (ENet/UDP requires SOCKS5 UDP ASSOCIATE; an HTTP-only gateway can't carry the logon).
4. `http_url = proxy.to_proxy_url(scheme).ok()?`.
5. `proxy_addr = "{host}:{port}".to_socket_addrs().ok()?.next()?` (resolve; `None` if unresolvable).
6. Return `BypassLoginSession { http_url, enet: Socks5Config { proxy_addr, username.clone(), password.clone() } }`.
- **Invariant:** `http_url` and `enet` come from the SAME `proxy`. C++ must not re-`pick()` for the ENet leg.

### 4.14 `parse_proxy_lines(input: &str) -> Result<Vec<ProxyEntry>>` **(pub)**

Parse a multi-line textarea into entries.
1. For each line (0-indexed `line_no`): `raw = line.trim()`. Skip if empty or starts with `#` (comment).
2. `expand_proxy_line(raw)`; on Ok append entries; on Err push `"line {line_no+1}: {e}"` into an errors list.
3. If no errors → `Ok(all entries)`; else → **Err** with errors joined by `"; "`.
- **All-or-nothing:** any single bad line fails the whole parse (and thus the whole `update`).

### 4.15 `expand_proxy_line(raw: &str) -> Result<Vec<ProxyEntry>>` (private) — port-range expansion

Handles the DataImpulse `host:START-END:user:pass` sticky-range form.
1. Only if `raw` contains neither `"://"` nor `'@'` (ranges only for the plain colon form):
   - `parts = raw.split(':')`. If `parts.len() >= 2` and `parts[1]` contains `'-'` (split_once on first `'-'` → `start_s`,`end_s`):
     - `start = start_s.trim().parse::<u16>()` (Err `"invalid range start port '{start_s}'"`).
     - `end   = end_s.trim().parse::<u16>()`   (Err `"invalid range end port '{end_s}'"`).
     - If `end < start` → Err `"port range end {end} is before start {start}"`.
     - `host = parts[0].trim()`; if empty → Err `"missing proxy host"`.
     - `username = parts.get(2).trim()` filtered to non-empty (i.e. `parts[2]` if present and non-empty).
     - `password = if parts.len() > 3 { parts[3..].join(":").trim(); Some if non-empty }` else None. **Password may itself contain colons** — everything after the 3rd colon is rejoined with `:`.
     - `count = min((end - start + 1) as u32, 2000)` — **cap at 2000 entries** (typo guard, e.g. `10001-60000`).
     - For `offset in 0..count`: `port = start + offset`; rebuild `raw_entry = "{host}:{port}"` then append `":{user}"` and (if pw) `":{pass}"`; push `ProxyEntry { host, port, username, password, scheme:None, raw: raw_entry }`.
     - Return the vec.
2. Otherwise (no range, or URL/`@` form) → `parse_proxy_line(raw)` wrapped as a 1-element vec.
- **C++:** careful `split(':')` semantics — split on every `:`, take `[0]`=host, `[1]`=port-or-range, `[2]`=user, `[3..]` rejoined by `:` = pass. The cap `min(..., 2000)` uses `u32` arithmetic to avoid `u16` overflow when `end-start+1` is large; `port = start + offset` stays within `u16` because `end<=u16::MAX`.

### 4.16 `parse_optional_proxy(input: &str) -> Result<Option<ProxyEntry>>` **(pub)**

Trim; if empty → `Ok(None)`; else `parse_proxy_line(trimmed).map(Some)`.

### 4.17 `parse_proxy_line(raw: &str) -> Result<ProxyEntry>` (private) — single entry, no ranges

Two forms:
- **URL/`@` form** (`raw` contains `"://"` or `'@'`):
  1. `had_scheme = raw.contains("://")`.
  2. `url_text = raw` if it has `://`, else `"socks5://{raw}"` (bare `user:pass@host:port` defaults to socks5).
  3. Parse as URL.
  4. `host = url.host_str()` (must be non-empty, else Err `"missing proxy host"`).
  5. `port = url.port()` (must be present, else Err `"missing proxy port"`) — NOTE: no default-port inference; port is mandatory.
  6. `username = url.username()` if non-empty; `password = url.password()`.
  7. `scheme = had_scheme ? Some(normalize_proxy_scheme(url.scheme())) : None`.
  8. Return `ProxyEntry { host, port, username, password, scheme, raw: raw.to_string() }`.
- **Colon form** (`host:port[:user[:pass]]`):
  1. `parts = raw.split(':')`; if `< 2` → Err `"expected host:port or host:port:user:pass"`.
  2. `host = parts[0].trim()`, `port = parts[1].trim().parse::<u16>()?`; if host empty → Err `"missing proxy host"`.
  3. `username = parts.get(2).trim()` if non-empty.
  4. `password = parts.len()>3 ? parts[3..].join(":").trim() (Some if non-empty) : None`.
  5. `scheme = None`.
  6. Return entry with `raw = raw.to_string()`.

### 4.18 `normalize_proxy_scheme(value: &str) -> &'static str` (private)

`value.trim().to_ascii_lowercase()` matched:
- `"auto"` → `"auto"`
- `"socks5"` → `"socks5"`
- `"socks5h"` → `"socks5h"`
- **anything else** → `"http"` (default/fallback)

### 4.19 `effective_proxy_scheme(configured_scheme: &str, proxy: &ProxyEntry) -> &'static str` (private)

1. If the **per-entry** `proxy.scheme` (normalized) is present and **not** `"auto"` → use it (entry override wins).
2. Else, `normalize_proxy_scheme(configured_scheme)`:
   - `"auto"` → `infer_proxy_scheme_from_port(proxy.port)`
   - otherwise → that scheme.

### 4.20 `infer_proxy_scheme_from_port(port: u16) -> &'static str` (private)

- `823` → `"http"` (DataImpulse plain HTTP gateway).
- everything else (incl. `824` SOCKS5 gateway and the `10000-20000` sticky range) → `"socks5h"` (remote DNS).
- Comment note (behavioral context): the bot login path *retries the alternate scheme on failure*, so an HTTP-only endpoint mis-inferred as SOCKS still recovers. (That retry lives in the login module, not here — but the inference must match.)

### 4.21 `normalized_rotating_port_span(base_port: u16, span: u16) -> u16` (private)

- If `base_port` in `10000..=20000` (inclusive) → `min(max(span,1), 20000 - base_port + 1)` (clamp so range never exceeds 20000).
- Else → `1` (any non-sticky port — the 823/824 gateways or a fixed dedicated bypass like `host:24685` — is a SINGLE endpoint; never randomize its port or you'd dial a non-existent `host:<random>`).

### 4.22 `random_rotating_port(base_port: u16, span: u16) -> u16` (private)

1. `max_span = (u16::MAX - base_port) + 1` (in `u32`).
2. `span = min(max(span,1), max_span)` (in `u32`).
3. If `span == 1` → return `base_port` (no randomization).
4. Else draw 4 random bytes (Mori uses a fresh UUIDv4's first 4 bytes as `u32` **little-endian**), `random = u32`; return `base_port + (random % span) as u16`.
- **C++:** use any uniform RNG (`std::mt19937` seeded from `random_device`, or read 4 bytes). The UUID detail is incidental — what matters is a uniform pick in `[base_port, base_port+span-1]`. No dependency on UUID needed in Nxrth.

### 4.23 `random_index(len) -> usize` (private, `#[allow(dead_code)]`)

Uniform index in `0..len` from a UUIDv4's first 4 LE bytes; `len<=1` → 0. **Dead code** (login pick is now round-robin). C++: omit, or provide a trivial uniform-index helper. Documented only for completeness.

---

## 5. FILE `proxy_pool.rs` — ON-DISK FORMAT `data/proxy_pool.json`

JSON object (pretty-printed, 2-space indent by Rust). Example (with one game entry, a DataImpulse sticky bypass, migration-cleared legacy field):

```json
{
  "enabled": true,
  "max_bots_per_ip": 1,
  "spread_mode": "least_loaded",
  "proxies": [
    {
      "host": "31.56.213.68",
      "port": 24685,
      "username": "user1",
      "password": "pass1",
      "scheme": null,
      "raw": "31.56.213.68:24685:user1:pass1"
    }
  ],
  "next_index": 0,
  "rotating_login_enabled": true,
  "rotating_login_scheme": "auto",
  "rotating_login_port_span": 2000,
  "rotating_login_proxy": null,
  "rotating_login_proxies": [
    {
      "host": "gw.dataimpulse.com",
      "port": 10001,
      "username": "diuser",
      "password": "dipass",
      "scheme": null,
      "raw": "gw.dataimpulse.com:10001:diuser:dipass"
    }
  ]
}
```

- `spread_mode` string is snake_case (`"least_loaded"`/`"round_robin"`).
- `scheme` is `null` for colon-form entries, or a normalized scheme string (`"socks5"`, `"socks5h"`, `"http"`, `"auto"`) for URL-form entries that carried a scheme.
- On read: apply `#[serde(default)]` fallbacks (§3.3); any parse failure → full default config.

---

## 6. FILE `rotation_pool.rs` — world-rotation coordination (fleet-wide shared state)

Not a network proxy. A shared, on-disk table (`rotation_pool_state.json`) where bots **claim / lock / update / release** worlds so multiple bots don't work the same world simultaneously. Directly relevant to Nxrth's "bots must be aware of each other" requirement.

### 6.1 TYPES

```rust
pub struct RotationPoolTarget { pub world: String, pub door: String }   // requested world+door

pub struct RotationWorldStatus {
    pub world: String,           // normalized (see normalize_world)
    pub door: String,
    pub status: String,          // state machine string, see below
    pub bot_id: Option<u32>,     // owning bot id (None = unclaimed)
    pub bot_name: String,
    pub ready_count: u32,
    pub seed_count: u32,
    pub capacity: u32,
    pub next_ready_at: u64,      // unix secs; 0 or <=now means eligible
    pub updated_at: u64,         // unix secs of last mutation (lock heartbeat)
    pub note: String,
}

pub struct RotationPoolSnapshot {   // Default derivable
    pub pool_id: String,
    pub updated_at: u64,
    pub worlds: Vec<RotationWorldStatus>,
}

struct RotationPoolFile {           // private, on-disk root; Default derivable
    pools: BTreeMap<String, RotationPoolSnapshot>,   // pool_id -> snapshot; BTreeMap = keys sorted
}
```
- **`BTreeMap`** → C++ `std::map<std::string, RotationPoolSnapshot>` (ordered by key; JSON object key order is sorted). Use an ordered map so serialized output is deterministic.
- All `#[derive(... Serialize, Deserialize ...)]`; `RotationPoolSnapshot` and `RotationPoolFile` also derive `Default`.

**`status` value vocabulary** (plain strings; treat as an enum in C++ if desired but persist as these exact strings):
`"none"`, `"unknown"`, `"locked"`, `"working"`, `"expired"`, `"empty"`, `"warp_failed"`, `"growing"`, `"ready"`, `"released"` (note used), plus any bot-supplied status via `update_world`.

### 6.2 Global state + helpers

```rust
static ROTATION_POOL: OnceLock<Mutex<RotationPoolFile>>;   // lazily loaded from disk
```
- `state_path()` → `<cwd (fallback ".")>/rotation_pool_state.json` (note: **not** under `data/`; sits at cwd root).
- `now_secs()` → unix seconds (`SystemTime::now().duration_since(UNIX_EPOCH)`; on error → 0).
- `normalize_world(world)` → keep only ASCII-alphanumeric chars and `'_'`, then **uppercase**. (Strips spaces/punctuation, e.g. `"my world!"` → `"MYWORLD"`.)
- `target_key(world)` → `normalize_world(world)` (the map/dedup key for worlds).
- `load_file()` → read `state_path()`, JSON-parse to `RotationPoolFile`; on any failure → `Default` (empty).
- `state()` → `ROTATION_POOL.get_or_init(|| Mutex::new(load_file()))` — loaded once, then in-memory authoritative.
- `save_file(data)` → best-effort pretty JSON write (errors ignored).
- **Poison handling:** every lock uses `unwrap_or_else(PoisonError::into_inner)` (ignore poison). C++: plain `std::mutex`.

### 6.3 `sync_pool(pool, targets, now)` (private)

Reconcile a pool's `worlds` to match the requested `targets`, preserving prior per-world state:
1. Snapshot `previous = { target_key(world) -> status }` from existing `pool.worlds`.
2. Build `next` in the **order of `targets`**: for each target, `world = normalize_world(target.world)`; skip if empty; look up `previous[target_key(world)]`; if found clone it, else create a fresh `RotationWorldStatus { world, door, status:"none", bot_id:None, bot_name:"", counts 0, next_ready_at:0, updated_at:now, note:"not scanned yet" }`. Then overwrite `entry.world = world` and `entry.door = target.door` (door always refreshed from target).
3. `pool.worlds = next`; `pool.updated_at = now`.
- **Effect:** worlds no longer in `targets` are dropped; new targets appear as `"none"`; existing keep their status but get door refreshed. Ordering follows `targets`.

### 6.4 `snapshot(pool_id: &str) -> RotationPoolSnapshot` **(pub)**

Lock state; return a clone of `pools[pool_id]` if present, else a fresh `RotationPoolSnapshot { pool_id, updated_at: now_secs(), worlds: [] }`. Read-only (no save).

### 6.5 `clear(pool_id: &str)` **(pub)**

Lock; `pools.remove(pool_id)`; `save_file`.

### 6.6 `claim_world(pool_id, targets, bot_id, bot_name, lock_ttl_sec) -> Option<RotationWorldStatus>` **(pub)**

The core mutual-exclusion primitive. Steps under the global lock:
1. `now = now_secs()`.
2. Get-or-create `pool = pools[pool_id]` (fresh empty snapshot if absent).
3. `sync_pool(pool, targets, now)` — reconcile to requested targets.
4. **Expire stale locks:** for each world with `status ∈ {"locked","working"}`: if `updated_at + max(lock_ttl_sec, 30) < now` → set `status="expired"`, `bot_id=None`, `bot_name=""`, `note="lock expired"`, `updated_at=now`. (TTL floor 30s.)
5. **Re-entrant claim:** if a world already has `bot_id == this bot_id` and `status ∈ {"locked","working"}` → refresh its `updated_at=now` (heartbeat), clone it, `save_file`, return it. (A bot re-claims its own lock instead of grabbing a new world.)
6. **Pick a new world** by status priority order — the array (exact order):
   `["ready","none","unknown","expired","empty","warp_failed","growing"]`.
   For each `wanted` in that order, find the **first** world where:
   - not locked by another bot (`bot_id.is_none() || bot_id == Some(this)`), AND
   - `status == wanted`, AND
   - `next_ready_at == 0 || next_ready_at <= now` (cooldown elapsed).
   Use the first match found scanning priorities in order.
7. If none found → `save_file`, return `None`.
8. Otherwise lock it: `status="locked"`, `bot_id=Some(this)`, `bot_name=name`, `updated_at=now`, `note="claimed"`; clone; `save_file`; return the claimed status.
- **C++/fleet note:** this is the heart of "bots aware of each other." In Rust it's a single-process global `Mutex` + a JSON file. If Nxrth runs bots as threads in one process, a `std::mutex` around an in-memory `std::map` + best-effort file persistence is a faithful port. If Nxrth ever spans processes, the file alone is NOT a cross-process lock — you'd need file locking or a shared IPC store. Preserve the priority list and TTL semantics exactly.

### 6.7 `update_world(pool_id, world, door, status, ready_count, seed_count, capacity, next_ready_at, bot_id, bot_name, note) -> RotationWorldStatus` **(pub)**

Upsert one world's full status. Under lock:
1. `now = now_secs()`; get-or-create `pool`.
2. `normalized = normalize_world(world)`, `key = target_key(normalized)`.
3. Find world index by `target_key(entry.world) == key`; if absent, push a fresh placeholder (`status:"none"`, zeros, `note:""`, `updated_at:now`) and use its index.
4. Overwrite ALL fields: `world=normalized, door, status, ready_count, seed_count, capacity, next_ready_at, bot_id, bot_name, updated_at=now, note`.
5. `pool.updated_at = now`; clone the entry; `save_file`; return it.

### 6.8 `release_world(pool_id, world, bot_id)` **(pub)**

Under lock: `now = now_secs()`. If `pools[pool_id]` exists: find world by `target_key`. If found AND `entry.bot_id == Some(bot_id)` (only the owner may release): clear `bot_id=None`, `bot_name=""`; if `status ∈ {"locked","working"}` → set `status="unknown"`; `updated_at=now`; `note="released"`. Then `pool.updated_at=now`; `save_file`. (No-op if pool/world missing or owned by another bot.)

### 6.9 ON-DISK FORMAT `rotation_pool_state.json`

```json
{
  "pools": {
    "POOL_A": {
      "pool_id": "POOL_A",
      "updated_at": 1752192000,
      "worlds": [
        {
          "world": "MYWORLD",
          "door": "MAIN",
          "status": "locked",
          "bot_id": 7,
          "bot_name": "botseven",
          "ready_count": 0,
          "seed_count": 0,
          "capacity": 0,
          "next_ready_at": 0,
          "updated_at": 1752192000,
          "note": "claimed"
        }
      ]
    }
  }
}
```
- Root object has a single `pools` map keyed by `pool_id` (sorted — BTreeMap).
- `bot_id` is `null` when unclaimed.
- All timestamps are unix seconds (`u64`).
- On read: any parse failure → empty file (`{ "pools": {} }`).

---

## 7. FILE `proxy_test.rs` — 3-stage proxy self-test

### 7.1 TYPES

```rust
pub struct CheckResult {
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")] pub error: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")] pub detail: Option<String>,
}
pub struct ProxyTestResult {
    pub socks5: CheckResult,       // stage 1: SOCKS5 UDP associate
    pub server_data: CheckResult,  // stage 2: server_data.php fetch (proxied)
    pub enet: CheckResult,         // stage 3: ENet connect handshake
}
```
Serde: `error`/`detail` omitted from JSON when `None`. C++: use `std::optional` and skip when empty in nlohmann serialization.

### 7.2 `run_proxy_test(cfg: Socks5Config) -> ProxyTestResult` **(pub)**

1. Build `login_info = LoginInfo { protocol: PROTOCOL (226), game_version: GAME_VER ("5.51") }`.
2. `proxy_url = cfg.to_url()` (`socks5://[user:pass@]ip:port`).
3. **Stage 1 — SOCKS5 check:** bind a `Socks5UdpSocket` through the proxy: `Socks5UdpSocket::bind_through_proxy(local="0.0.0.0:0", cfg.proxy_addr, cfg.username, cfg.password)`.
   - Ok → `CheckResult { ok:true }`; Err → `{ ok:false, error: e.to_string() }`.
4. **Stage 2 — server_data:** `get_server_data_proxied_live(false, &login_info, Some(&proxy_url))` and on Err retry with `alternate=true`. On success `sd`:
   - `detail = "{sd.server}:{sd.port}"`; also parse that as a `SocketAddr` (`server_addr = ...ok()`).
   - `CheckResult { ok:true, detail: Some("{server}:{port}") }`.
   - On Err → `{ ok:false, error }`, `server_addr = None`.
5. **Stage 3 — ENet:** if `server_addr` is `None` → `{ ok:false, error:"skipped: server_data check failed" }`. Else `check_enet(&cfg, addr)`.
6. Return `ProxyTestResult { socks5, server_data, enet }`.

### 7.3 `check_enet(cfg: &Socks5Config, server_addr: SocketAddr) -> CheckResult` (private)

1. Bind a fresh `Socks5UdpSocket::bind_through_proxy("0.0.0.0:0", cfg.proxy_addr, user, pass)`; Err → `{ ok:false, error }`.
2. Build ENet host settings: `peer_limit=1, channel_limit=2, compressor=Some(RangeCoder::new()), checksum=Some(crc32), using_new_packet=true` (rest default). These must match the Growtopia gateway or the handshake is rejected.
3. `enet::Host::new(socket, settings)`; Err → `{ ok:false, error }`.
4. `host.connect(server_addr, channel_count=2, data=0)`; Err → `{ ok:false, error }`.
5. Loop with `deadline = now + 10s`, sleeping `10ms` per iteration:
   - Past deadline → `{ ok:false, error:"timed out waiting for ENet connect" }`.
   - `host.service()`:
     - `Ok(Some(event))` and `event.no_ref()` is `Connect{..}` → `{ ok:true }` (handshake complete).
     - `Ok(None)` → keep looping.
     - `Err(e)` → `{ ok:false, error }`.
- **C++:** drive the vendored ENet host in a `service()` loop; success = the CONNECT event. Use `std::this_thread::sleep_for(10ms)`; overall 10s budget.

### 7.4 External dependencies crossed here (interface-level)

- `Socks5UdpSocket::bind_through_proxy(local, proxy_addr, user, pass) -> io::Result<Self>`: TCP-connects to the proxy (10s connect timeout), sets 15s r/w timeouts on the control stream, performs the SOCKS5 handshake (incl. optional user/pass auth) and a UDP ASSOCIATE, returns a UDP socket that relays datagrams through the proxy. If the relay reply is `0.0.0.0`, it substitutes the proxy's own IP for the relay address. (Full spec: SOCKS5 module.) Port to the vendored patched C ENet + SOCKS5-UDP shim in Nxrth.
- `get_server_data_proxied_live(alternate: bool, &LoginInfo, proxy_url: Option<&str>) -> Result<ServerData>`: HTTP POST to Growtopia's `server_data.php` (primary vs `alternate` endpoint) through the given proxy URL; returns `ServerData { server: String, port: u16, loginurl, server_type: u8, beta_*, ... }`. (Full spec: server_data module.) In Nxrth this is libcurl with the proxy set (`socks5h://…` or `http://…`).
- `LoginInfo { protocol: u32, game_version: String }` — the version pair posted to server_data.
- `ServerData { server: String, port: u16, loginurl: String, server_type: u8, beta_server, beta_loginurl, beta_port: u16, ... }` — only `.server`/`.port` are used here.

---

## 8. DEPENDENCY MAPPING (Rust crate → Nxrth C++)

| Rust / crate | Used for (this module) | Nxrth C++ equivalent |
|---|---|---|
| `serde` / `serde_json` | (de)serialize `ProxyPoolConfig`, `RotationPoolFile`, view/result DTOs; pretty JSON files | **nlohmann/json** (`to_json`/`from_json`, `dump(2)`); replicate `#[serde(default)]` (default-on-missing) and `skip_serializing_if=None` (omit null optionals) manually |
| `url::Url` | parse/build proxy URLs (`parse_proxy_line`, `to_proxy_url`) | manual URL assembly + a small parser; percent-encode userinfo; **libcurl** consumes the URL string (`CURLOPT_PROXY`). Use `socks5h://` for remote DNS, `http://` for the 823 gateway |
| `std::net::{SocketAddr, ToSocketAddrs}` | resolve `host:port` (`to_socks5`, `login_session`) | `getaddrinfo` (IP-literal fast path via `inet_pton`, DNS fallback); store resolved `ip:port` |
| `std::sync::{Mutex, OnceLock}`, `AtomicUsize` | global game-pool snapshot, RR cursors, rotation-pool state | `std::mutex` + a lazily-initialized singleton (function-local `static`), `std::atomic<size_t>` with `memory_order_relaxed` `fetch_add`. **No poison concept** — drop `PoisonError::into_inner` |
| `std::collections::{HashMap, BTreeMap}` | active-count map (Hash), rotation `pools` (Btree = sorted) | `std::unordered_map` for active counts; **`std::map`** for `pools` (ordered → deterministic JSON key order) |
| `uuid` | random 4 bytes for `random_rotating_port`/`random_index` | `std::mt19937` seeded from `std::random_device` (uniform `port ∈ [base, base+span)`). Drop the UUID dependency |
| `anyhow` | error propagation with string messages | exceptions or `std::expected<T,std::string>` / `tl::expected`; preserve exact message strings for parity |
| `std::fs` / `std::path` | read/write JSON, `create_dir_all` | `std::filesystem` + `std::ifstream`/`ofstream` |
| `rusty_enet` (proxy_test) | ENet host/connect/service, RangeCoder, crc32 | **vendored C ENet** (patched for SOCKS5-UDP); mirror `HostSettings` (peer 1, channels 2, range-coder compressor, crc32 checksum, new-packet flag) |
| `ureq`/`reqwest` (via server_data) | proxied HTTP `server_data.php` fetch | **libcurl** (`socks5h`, cookies) — in the server_data module |
| `crossbeam-channel`, `tokio/axum/tower`, `mlua`, `scraper`, `md5`, `argon2` | **not used in this module** | (n/a here) map per the global convention: channels → `std::mutex`+condvar queue; async/web → `std::thread` + Dear ImGui; Lua → native C++; HTML → regex/manual; md5 → bundled; argon2 → argon2 lib |

---

## 9. THREADING & SHARED STATE

### 9.1 `proxy_pool.rs`
- **Instance state** (`ProxyPool`): each `ProxyPool` owns its `config` and `path`. In Mori a single `ProxyPool` is held behind the app's shared state and mutated by `update`/`choose` (which both persist). If multiple threads touch one `ProxyPool`, guard it with a `std::mutex` in Nxrth (Rust relied on the enclosing app lock; `choose`/`update` take `&mut self`).
- **Process-global shared state** (survives across all bots/threads):
  - `LOGIN_PROXY_RR: AtomicUsize` — round-robin cursor so N concurrent bot logins spread deterministically across N bypass entries (avoids collision-hammering one exit IP). Cloned `RotatingLoginProxy` values all read/advance this same atomic.
  - `GAME_PROXY_POOL: Mutex<Vec<Socks5Config>>` — published snapshot of the resolved game pool, refreshed on load and every `update`. Read by `next_game_proxy` when a bot's subserver tunnel dies.
  - `GAME_PROXY_RR: AtomicUsize` — round-robin cursor for game-proxy failover, advanced even on skipped (same-as-current) slots.
- **Per-bot state:** each bot holds ONE assigned game `Socks5Config` (from `choose`) and, if bypass is on, a cloned `RotatingLoginProxy`. Its assigned game proxy's `capacity_key` string is the bot's `proxy_key`, and the fleet's `active_counts: HashMap<capacity_key, usize>` (owned by the bot manager, not this module) is what `choose`/`view` consult to respect `max_bots_per_ip`. Nxrth must maintain that fleet-wide active-count map (increment on assign, decrement on bot stop) so `choose` load-balances correctly.
- **IP-pinning invariant** (fleet-safety): `login_session()` pairs the HTTP token fetch and the ENet logon on ONE exit IP because the `ltoken` is IP-bound. Never split the pair across two picks or two proxies. (Matches the GT ltoken IP-binding memory: a rotating-per-stage proxy breaks GT login.)

### 9.2 `rotation_pool.rs`
- **Single process-global `Mutex<RotationPoolFile>`** loaded once from disk, then authoritative in memory; every mutating op persists best-effort to `rotation_pool_state.json`. This is the **fleet coordination table**: all bot threads claim/lock/release worlds through it, making bots aware of each other (no two bots work the same world). Port as a singleton `std::mutex`-guarded `std::map`. Lock TTL (min 30s) auto-expires dead bots' locks; `bot_id` ownership gates release. If Nxrth ever runs bots across processes, the JSON file is not a cross-process lock — add OS file locking or shared IPC.

### 9.3 `proxy_test.rs`
- Stateless & synchronous: blocks the calling thread up to ~ (SOCKS5 connect 10s + server_data HTTP + ENet 10s). Run it on a worker thread in Nxrth so the ImGui UI stays responsive. No shared state.

---

## 10. PORTING CHECKLIST (behavioral parity gotchas)

1. `to_socks5`: IP-literal parse **before** DNS; never drop hostname endpoints.
2. `capacity_key` uses the **resolved** IP (not the typed host) — two hostnames resolving to one IP share capacity.
3. `expand_proxy_line`: range only for the non-URL/non-`@` colon form; cap expansion at **2000**; password rejoins colons; `end<start` is an error.
4. `choose`: scan modularly from persisted `next_index`; LeastLoaded ties broken by cursor; persist `next_index` on every pick.
5. `pick`: multi-entry pool forces `span=1` and uses `LOGIN_PROXY_RR`; single entry uses configured span + random port.
6. `login_session`: single pick → both `http_url` and `enet`; SOCKS5-only (reject http scheme); resolve or return None.
7. Scheme inference: `823→http`, else `socks5h`; per-entry non-`auto` scheme overrides configured; unknown configured scheme → `http`.
8. `normalized_rotating_port_span`: real span only for ports `10000..=20000`, clamped to not exceed 20000; everything else → 1.
9. rotation_pool: `normalize_world` = alnum+`_`, uppercased; claim priority list order is load-bearing; TTL floor 30s; only owner releases; BTreeMap → ordered map.
10. JSON: `#[serde(default)]` fields default-on-missing; `skip_serializing_if=None` omits nulls in `CheckResult`; any file parse failure → default/empty struct.
11. proxy_test constants: PROTOCOL 226, GAME_VER "5.51", ENet settings (peer 1 / chan 2 / range-coder / crc32 / new-packet), 10s connect deadline, 10ms poll, server_data primary-then-alternate.
12. Rename rules: nothing to rename inside these three files; keep `data/proxy_pool.json` and `rotation_pool_state.json` filenames; do not rename DataImpulse/Growtopia/ltoken/ENet.
