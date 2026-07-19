# Nxrth Port Spec — Module 05: Login / Auth

**Source (Rust / "Mori"):** `login.rs`, `auth.rs`, `dashboard.rs`, `har_parser.rs`, `bot/auth.rs`
**Supporting source read for self-containment:** `server_data.rs`, `constants.rs`, `account_devices.rs`, `protocol/crypto.rs`, `bot/shared.rs`, `proxy_pool.rs` (excerpts), `bot/core.rs` (excerpts: gates, `build_login_packet`, `LoginMethod`).
**Target (C++ / "Nxrth"):** reimplement WITHOUT reading the Rust. This document is the single source of truth.

> Naming: everywhere below, apply the rename rules in §10. The identifiers in this spec are the *Rust originals* so you can cross-reference; the C++ code must use the renamed forms.

---

## 0. What this module does (big picture)

This module turns a `(username, password)` (or a raw token) into a Growtopia **`ltoken`** plus a **server address** and **`meta`** string, ready for the ENet logon packet (`protocol|226`). There are three distinct HTTP login pipelines plus two token-source shortcuts:

| Mode | Entry fn (`bot/auth.rs`) | Dashboard fn (`dashboard.rs`) | HAR fallback? | Notes |
|------|--------------------------|-------------------------------|---------------|-------|
| **Legacy** | `fetch_credentials` | `get_dashboard_proxied` | yes (Requestly HAR + growid-HAR) | classic GrowID; `platformID 0,1,1` in dashboard POST |
| **Newly** | `fetch_newly_credentials` | `get_newly_dashboard_proxied` | **NO** | current 5.51 path; **exactly 22 fields**, `platformID 15,1,0`; no HAR |
| **Requestly** | `fetch_requestly_credentials` | `get_requestly_dashboard_proxied` | uses HAR body as template | replays a captured `requestly_logs.har` POST body |
| Ltoken (direct) | `Bot::new_ltoken` (core.rs) | — | — | token supplied directly |
| HarToken | `Bot::new_har…` (core.rs) | — | — | token extracted from a local `.har` |

The canonical **newly** flow (the one to prioritize in Nxrth):
```
server_data POST  →  growid dashboard POST (22 fields)  →  parse dashboard HTML for Growtopia href
   →  GET that href (extract CSRF _token)  →  growid/login/validate POST  →  ltoken (JSON "token")
```

Two entirely separate "auth" concerns share the word *auth*; do not confuse them:
1. **`auth.rs`** = the **local operator/web-UI password** (Argon2id) that gates the Mori control panel. This is *not* Growtopia login. In Nxrth (native Dear ImGui, no web server) this becomes an optional local unlock gate. Documented in §5.
2. **`bot/auth.rs`** = the **Growtopia account login** orchestration. This is the core of the module.

---

## 1. Constants (EXACT — copy verbatim)

From `constants.rs`. GT wire is little-endian. These are current for **GT 5.51 / protocol 226 (force-update 2026-07-11)**.

```
PROTOCOL              : u32   = 226
GAME_VER              : &str  = "5.51"
FHASH                 : i32   = -716928004
DEFAULT_RID           : &str  = "025B42980AFB659E0394C846233653FF"
DEFAULT_MAC           : &str  = "74:d4:dd:6c:24:e1"
DEFAULT_WK            : &str  = "788E366E74D2D098398A35C3F6360DDA"
DEFAULT_HASH          : &str  = "381621508"          // parsed to i32 for klv
DEFAULT_HASH2         : &str  = "-332772458"
DEFAULT_FZ            : &str  = "18274296"
DEFAULT_PLATFORM_ID   : &str  = "15,1,0"             // 15,1,0 — the "newly" platformID
DEFAULT_ZF            : &str  = "1597752569"
DEFAULT_STEAM_TOKEN   : &str  = "14 00 00 00 3c 13 04 25 ... aa d6 43 ... 5d.240"   // long hex-with-spaces blob, ends ".240" (see below)
```

`DEFAULT_STEAM_TOKEN` full literal (single line, keep verbatim):
```
14 00 00 00 3c 13 04 25 a4 c5 fd 4c 8c 55 93 72 01 00 10 01 38 b9 12 6a 18 00 00 00 01 00 00 00 05 00 00 00 6d 91 7f 14 12 ca 43 3b 27 31 26 00 02 00 00 00 b8 00 00 00 38 00 00 00 04 00 00 00 8c 55 93 72 01 00 10 01 e4 36 0d 00 c0 47 54 b9 06 64 a8 c0 00 00 00 00 6a 3c 10 6a ea eb 2b 6a 01 00 11 2b 04 00 01 00 e6 41 28 00 00 00 00 00 c2 54 51 5d b7 4e 66 c6 a0 52 02 c9 67 d0 56 bd aa d6 43 6d ab 51 3b 68 b9 3c 10 71 21 37 f7 ca ec 78 eb 8b 5d 83 5a 14 91 87 b2 fa b2 c3 db 69 1e 23 11 d2 35 bd 6b a0 1c 41 fe 83 75 43 51 ce b6 71 6f da be ef 1d 09 9e 04 53 46 7c 41 62 f8 f7 e6 51 ac b8 b1 c4 24 84 a6 94 33 36 52 8c dc 3a a6 d3 80 f6 ba 2d 26 d3 eb ac ea 91 5c 52 17 60 55 f2 52 26 24 d4 c6 b9 4b 52 c5 0e ff 2d 5d.240
```

**KLV keys** (from `protocol/crypto.rs`, embedded in the GT binary — copy verbatim):
```
KEY1 = "832aac071ffbcfc15bfe1d0a7ad15221"
KEY2 = "709296ddd04fc4074a7b443ecc0799aa"
KEY3 = "623de1e8fff22a2b3e0d7e01593e7c22"
KEY4 = "bb835e5a57e6c88e2449499ca487ced2"
KEY5 = "ea76e4d6009282186063fe9465f2d9ab"
```

**Other magic strings/URLs used in this module:**
```
valKey (dashboard & checktoken)   = 40db4045f2d8c572efe8c4a060605726
server_data (primary)             = https://www.growtopia1.com/growtopia/server_data.php
server_data (alternate)           = https://www.growtopia2.com/growtopia/server_data.php
growid login/validate             = https://login.growtopiagame.com/player/growid/login/validate
checktoken                        = https://login.growtopiagame.com/player/growid/checktoken?valKey=40db4045f2d8c572efe8c4a060605726
dashboard path                    = /player/login/dashboard
LOGIN_HOST (fallback)             = login.growtopiagame.com
REQUESTLY_HAR_FILE                = requestly_logs.har
alt HAR filename (find only)      = orijinal_requestly_logs.har

User-Agent (server_data + legacy/newly dashboard + growid pages):
  "UbiServices_SDK_2022.Release.9_PC64_ansi_static"     // server_data POST & checktoken POST
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)"  // legacy/newly dashboard, growid page GET, growid/login/validate POST
User-Agent (Requestly + HAR replay dashboard):
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36 Edg/148.0.0.0"
```

**Timeouts:** server_data 20s; legacy/checktoken/growid 20s (`login.rs`); dashboard 30s (`dashboard.rs::build_agent`). All are *global* request timeouts (connect+read+write). **Dashboard agent uses `max_redirects(0)`** — redirects are followed manually (critical for token parsing). server_data agent **disables TLS verification** (`disable_verification(true)`).

---

## 2. Types

### 2.1 `LoginError` (`login.rs`)
```
enum LoginError { Exhausted, WrongCredentials, Other(String) }
```
Display strings (exact):
- `Exhausted`  → `"Login attempts exhausted. Please try again after 24 hours."`
- `WrongCredentials` → `"Account credentials mismatched."`
- `Other(msg)` → `msg` verbatim.

C++: `enum class LoginErrorKind { Exhausted, WrongCredentials, Other };` + a struct carrying kind + message. Classification (from the login page error text, case-insensitive `contains`): text contains `"exhausted"` → `Exhausted`; contains `"mismatched"` → `WrongCredentials`; else `Other`.

### 2.2 `LoginInfo` (`server_data.rs`)
```
struct LoginInfo { protocol: u32, game_version: String }
to_form_data() -> "protocol={protocol}&version={game_version}"   // NOTE: uses key "version", not "game_version"
```
Constructed everywhere as `{ protocol: PROTOCOL(226), game_version: "5.51" }`.

### 2.3 `ServerData` (`server_data.rs`)
Parsed from the `server_data.php` pipe/line response. Fields:
```
server: String, port: u16, loginurl: String, server_type: u8,
beta_server: String, beta_loginurl: String, beta_port: u16, beta_type: u8,
beta2_server, beta2_loginurl, beta2_port: u16, beta2_type: u8,
beta3_server, beta3_loginurl, beta3_port: u16, beta3_type: u8,
type2: u8, maint: Option<String>, meta: String
```
Only `server`, `port`, `loginurl`, `meta` are used downstream by the login module (`addr = server:port`, `meta`, `loginurl`).

**Wire format (text, line-based):** response body is lines. Parse each line:
- Stop entirely at a line starting with the literal `RTENDMARKERBS1001`.
- Split each line on the **first** `|` into `(key, value)`; skip lines with no `|`.
- `key` and `value` are trimmed. Map by key exactly: `server, port, loginurl, type→server_type, beta_server, beta_loginurl, beta_port, beta_type, beta2_server, beta2_loginurl, beta2_port, beta2_type, beta3_server, beta3_loginurl, beta3_port, beta3_type, type2, #maint→maint(Some), meta`. Unknown keys ignored.
- Numeric fields parsed as unsigned; a parse failure is a hard error.
- `has_required_login_fields()` (validity gate): `server` non-empty AND `port != 0` AND `loginurl` non-empty AND `meta` non-empty.

### 2.4 `DashboardLinks` (`dashboard.rs`)
```
struct DashboardLinks { apple: Option<String>, google: Option<String>, growtopia: Option<String> }
```
Only `growtopia` is consumed by login. It's the href to follow to reach the GrowID login page.

### 2.5 `NewlyDashboardIdentity<'a>` (`dashboard.rs`)
Borrowed identity for the newly dashboard POST. **NOTE:** in the *current* flow this is passed as `None` (see §4.2) — the identity is used for the ENet packet, not the dashboard POST. Fields (all `&str`):
```
cbits, player_age, gdpr, category, total_playtime, country, fz, rid, mac, wk,
hash, hash2, zf, platform_id, steam_token, klv
```
(`fz`, `hash2`, `zf`, `steam_token` are present in the struct but **must NOT be sent** in the newly POST — see §4.2.)

### 2.6 `Credentials` (`bot/auth.rs`) — the module's primary output
```
struct Credentials {
    ltoken: String,                 // the GrowID token used in the ENet logon packet
    meta: String,                   // from server_data
    addr: SocketAddr,               // server:port, IPv4/IPv6 parsed
    identity: Option<LoginIdentity>,// per-account device identity for ENet packet
    bypass_enet: Option<Socks5Config>, // pinned exit IP the ltoken is bound to (see §7 IP-binding)
}
```

### 2.7 `LoginIdentity` (`bot/auth.rs`) — per-account device fields for the ENet packet
```
struct LoginIdentity {
    game_version, cbits, player_age, gdpr, category, total_playtime, country,
    zf, rid, mac, wk, hash, hash2, fz, platform_id, steam_token, klv   // all String
}
```
Default values (`default_login_identity`, used by **newly**):
```
game_version = GAME_VER("5.51"); cbits="1536"; player_age="23"; gdpr="2";
category="_16"; total_playtime="0"; country="us"; zf=DEFAULT_ZF; rid=DEFAULT_RID;
mac=DEFAULT_MAC; wk=DEFAULT_WK; hash=DEFAULT_HASH; hash2=DEFAULT_HASH2; fz=DEFAULT_FZ;
platform_id=DEFAULT_PLATFORM_ID("15,1,0"); steam_token=DEFAULT_STEAM_TOKEN;
klv = compute_klv(GAME_VER, "226", DEFAULT_RID, DEFAULT_HASH_as_i32)
```
Then `apply_account_device_identity()` overrides `rid, mac, wk, hash, hash2, zf` from the per-account device store (§2.9) and recomputes `klv` with the device's rid+hash.

HAR-derived defaults differ (`login_identity_from_payload`): same as above except `cbits="1536", player_age="23", gdpr="2", category="_16"` still, but each field falls back to the HAR payload value if present.

### 2.8 `AuthData` (`har_parser.rs`) — extracted from a `.har`
```
struct AuthData {
    token, rid, mac, wk, klv, hash, hash2, fz, platform_id, steam_token, meta, post_data  // all String
}
```
Extraction rules in `extract_auth_data` (§4.6).

### 2.9 `AccountDevice` (`account_devices.rs`) — persistent per-account hardware identity
```
struct AccountDevice { rid, mac, wk, hash, hash2, zf }   // all String
```
On-disk store `data/account_devices.json`:
```
{ "devices": { "<lowercased-trimmed-username>": { rid, mac, wk, hash, hash2, zf }, ... } }
```
(Rust uses `BTreeMap` → keys sorted; nlohmann/json `ordered_json` or plain map is fine — order is not load-bearing.)

### 2.10 HAR partial structs (`har_parser.rs`) — deserialized from `requestly_logs.har`
```
HarFile { log: HarLog }
HarLog  { entries: Vec<HarEntry> }
HarEntry { request: HarRequest, response: HarResponse }
HarRequest { method: String, url: String, headers: Vec<HarHeader> (default []),
             postData: Option<HarPostData>, queryString: Vec<HarQueryParam> (default []) }
HarResponse { status: u16, headers: Vec<HarHeader> (default []), content: Option<HarContent> }
HarHeader { name: String, value: JSON }   // value_str(): String → self; Array → first .as_str(); else ""
HarPostData { text: String (default "") }
HarQueryParam { name: String, value: String }
HarContent { text: String (default "") }
```
Note the JSON key is `postData` and `queryString` (camelCase in HAR).

### 2.11 `Socks5Config` (`bot/shared.rs`)
```
struct Socks5Config { proxy_addr: SocketAddr, username: Option<String>, password: Option<String> }
to_url() -> "socks5://user:pass@host:port"  (or "socks5://host:port" if no creds)
```
Used both as the login HTTP proxy (via `to_url`) and the ENet SOCKS5-UDP config.

### 2.12 `BypassLoginSession` / `RotatingLoginProxy` (`proxy_pool.rs`) — pin one exit IP
```
struct BypassLoginSession { http_url: String, enet: Socks5Config }   // SAME IP for HTTP + ENet
struct RotatingLoginProxy { pool: Vec<ProxyEntry>, span: u16, configured_scheme: String }
RotatingLoginProxy::login_session() -> Option<BypassLoginSession>
    // picks next pool entry round-robin, rolls port; if effective scheme is NOT socks5/socks5h → None.
    // http_url = socks5(h) URL; enet = Socks5Config{proxy_addr = resolved host:port, user, pass}
```
`login_session()` returns `None` when the endpoint is not SOCKS5-UDP capable → login cannot pin the logon IP → caller logs and retries (see §4.2/§7).

### 2.13 Local operator auth types (`auth.rs`) — see §5
```
UserRecord { password_hash: String }                    // Argon2id PHC string, on disk data/user.json
AuthState  { inner: Arc<RwLock<AuthInner>> }
AuthInner  { password_hash: Option<String>, session_token: Option<String> }
```

### 2.14 `LoginMethod` (`bot/core.rs`) — selects flow + refresh behavior
```
enum LoginMethod {
    Legacy   { password: String },   // GrowID; on refresh-fail re-login w/ password
    Newly    { password: String },   // "Original Mori-style" GrowID, no HAR fallbacks  [rename: Nxrth-style]
    Requestly{ password: String },   // HAR-backed; on refresh-fail replay requestly_logs.har
    Ltoken,                          // token given directly; on refresh-fail STOP (no fallback)
    HarToken { har_path: String },   // token from local .har; bypasses server_data.php
}
```

---

## 3. Crypto — `compute_klv` and `hash_string` (`protocol/crypto.rs`)

Required to build `klv`. Uses **MD5 uppercase-hex** (`md5u`).

```
md5u(s)  = uppercase-hex of MD5(s as UTF-8 bytes)   // 32 hex chars, uppercase

compute_klv(game_version, protocol, rid, hash_val: i32) -> String:
    combined =  md5u(md5u(game_version))
             +  KEY1
             +  md5u(md5u(md5u(protocol)))          // triple-md5 of protocol
             +  KEY2
             +  KEY3
             +  md5u(md5u(rid))
             +  KEY4
             +  md5u(md5u( decimal_string(hash_val) ))   // hash_val.to_string(); signed decimal
             +  KEY5
    return md5u(combined)
```
- `protocol` is passed as its **decimal string** (e.g. `"226"`).
- `hash_val` is a signed i32; `.to_string()` gives its decimal (may be negative, e.g. `"381621508"`). For the default it comes from `DEFAULT_HASH.parse::<i32>()`.
- Order of concatenation is EXACT; do not reorder.

`hash_string` (GT rotate-left-5 hash; used elsewhere for item hashes, listed for completeness — not used by klv):
```
h: u32 = 0x55555555
for each byte b of s, THEN one trailing 0x00 (NUL terminator):
    h = rotate_left(h, 5) + b   (both wrapping / mod 2^32)
return h as i32
```

C++: `md5u` = your bundled MD5 → hex, then `std::toupper` each nibble. `rotate_left` = `(h<<5)|(h>>27)`. All arithmetic mod 2^32 (use `uint32_t`, cast to `int32_t` at the end).

---

## 4. Functions — behavior, step by step

### 4.1 `login.rs`

#### `get_legacy_token(url, username, password) -> Result<String>`
Thin wrapper → `get_legacy_token_proxied(url, username, password, None)`.

#### `get_legacy_token_proxied(url, username, password, proxy_url: Option<&str>) -> Result<String, LoginError>`
Fetches an `ltoken` from a GrowID login page URL. This is used by **all three** modes as the final step (the `url` is the dashboard's `growtopia` href, or a HAR-extracted growid URL).

Steps:
1. Build an HTTP agent: 20s global timeout; if `proxy_url` given, set proxy (`ureq::Proxy::new`). On proxy-parse error → `LoginError::Other(e)`.
2. **GET `url`** with header `User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)`. Read body to string. Any transport error → `LoginError::Other`.
3. `csrf_token = extract_csrf_token(html)`; if `None` → `LoginError::Other("Failed to extract CSRF token from login page")`.
4. **POST `https://login.growtopiagame.com/player/growid/login/validate`** with headers:
   - `User-Agent`: same Mac UA as above
   - `Content-Type: application/x-www-form-urlencoded`
   - `Origin: https://login.growtopiagame.com`
   - `Referer: <url>`  (the page URL from step 2)
   - form body (URL-encoded): `_token=<csrf>&growId=<username>&password=<password>`
5. If HTTP status != 200 → `LoginError::Other("Login failed with status: {status}")`.
6. Read body. Try parse as JSON:
   - If JSON parses: return `json["token"].as_str()`; if that field missing → `LoginError::Other("Missing 'token' field in login response")`.
7. If body is not JSON (server returned an HTML error page): parse HTML, select `.text-danger.text-danger-wrapper`, take first element's text, trim. If non-empty → classify: contains (lowercase) `"exhausted"` → `Exhausted`; `"mismatched"` → `WrongCredentials`; else `Other(text)`.
8. Fallthrough → `LoginError::Other("Login failed: unexpected response from server")`.

Return value on success: the raw token string (this becomes `ltoken`).

#### `extract_csrf_token(html) -> Option<String>`
Parse HTML, select `input[name='_token']`, return its `value` attribute of the first match. C++: regex/manual scan for `<input ... name="_token" ... value="(...)">` (attribute order not guaranteed — scan the tag). Return the value or none.

#### `check_token(token, login_info, proxy_url) -> anyhow::Result<String>`
Upgrades/validates a refresh token into a fresh token via GT's checktoken endpoint. (In the *current* newly/legacy flows this is **skipped for ENet login** — see `prepare_login_token`, only invoked when the dashboard token is not already a JWT-ish UbiTicket.)
1. If `token` empty → error "Token is empty".
2. Agent: 20s timeout, optional proxy.
3. **POST `https://login.growtopiagame.com/player/growid/checktoken?valKey=40db4045f2d8c572efe8c4a060605726`** with:
   - `User-Agent: UbiServices_SDK_2022.Release.9_PC64_ansi_static`
   - `Content-Type: application/x-www-form-urlencoded`
   - form body: `refreshToken=<token>&clientData=<login_info>`
4. Parse JSON response. If `response["status"] == "success"`: `new_token = response["token"]`; error if empty; else return it. Else → error `"Token validation failed: {full json}"`.

`login_info` here is the **clientData** blob built by `build_har_client_data` (§4.5).

### 4.2 `dashboard.rs`

All dashboard functions POST a **pipe-body** (`build_pipe_body`: each field on its own line `key|value\n`) and then **manually follow up to 4 redirects** (because `max_redirects=0`), then parse the final HTML for the Growtopia/Apple/Google links.

**Redirect-follow loop (shared by all four dashboard fns):**
1. After the POST, read `Location` response header → if present, `next_url = normalize_login_href(location, login_url)`.
2. Read body to `html`.
3. If `next_url` is None, `next_url = extract_html_redirect(html, login_url)` (meta-refresh or a link to `/steam/redirect` or `/player/login/dashboard`).
4. Loop **up to 4 times**: if `next_url` is Some, GET it, re-read `Location`/body, re-derive `next_url` the same way; break when None.
5. `parse_dashboard_response(html, login_url)`.

#### `get_dashboard_proxied(login_url, login_info, meta, proxy_url) -> Result<DashboardLinks>` (LEGACY)
POST to `https://{login_url}/player/login/dashboard?valKey=40db4045f2d8c572efe8c4a060605726`.
Headers: Mac UA, `Content-Type: application/x-www-form-urlencoded`.
Body fields (order EXACT, `build_pipe_body`), note **`platformID|0,1,1`** and **legacy includes `fz`+`hash2`**:
```
tankIDName|            requestedName|      protocol|<226>       fz|31631978
tankIDPass|            f|1                 game_version|<5.51>  cbits|0
player_age|25          GDPR|1              FCMToken|            category|_-5100
totalPlaytime|0        klv|<computed>      hash2|<DEFAULT_HASH2> meta|<meta>
fhash|<FHASH>          rid|<DEFAULT_RID>   platformID|0,1,1     deviceVersion|0
country|us             hash|<DEFAULT_HASH> mac|<DEFAULT_MAC>    wk|<DEFAULT_WK>
```
`klv = compute_klv(game_version, protocol_str, DEFAULT_RID, DEFAULT_HASH_i32)`. `fhash = FHASH.to_string()` = `"-716928004"`.

#### `get_newly_dashboard_proxied(login_url, login_info, meta, proxy_url, identity: Option<&NewlyDashboardIdentity>) -> Result<DashboardLinks>` (NEWLY — the current path)
POST to `https://{login_url}/player/login/dashboard?valKey=40db4045f2d8c572efe8c4a060605726`.
Headers: Mac UA, `Content-Type: application/x-www-form-urlencoded`, **`Origin: https://{login_url}`**.
Field values come from `identity` if `Some`, else defaults:
`cbits=0, player_age=25, gdpr=1, category=_-5100, total_playtime=0, country=us, rid=DEFAULT_RID, mac="02:00:00:00:00:00", wk="NONE0", hash=DEFAULT_HASH, platform_id=DEFAULT_PLATFORM_ID("15,1,0")`. `klv` = `identity.klv` if Some else `compute_klv(...DEFAULT_RID, DEFAULT_HASH_i32)`.

**EXACTLY 22 fields, order EXACT** (this is the canonical upstream POST — **DO NOT add `fz`, `hash2`, `zf`, `steamToken`**; the 5.51 server returns HTTP 500 if they are present. `klv` is NOT the culprit):
```
tankIDName|            player_age|<25>       fhash|<FHASH>
tankIDPass|            GDPR|<1>              rid|<DEFAULT_RID>
requestedName|         FCMToken|            platformID|<15,1,0>
f|1                    category|<_-5100>    deviceVersion|0
protocol|<226>         totalPlaytime|<0>    country|<us>
game_version|<5.51>    klv|<computed>       hash|<DEFAULT_HASH>
cbits|<0>              meta|<meta>          mac|<02:00:00:00:00:00>
                                            wk|<NONE0>
```
(22 lines total: tankIDName, tankIDPass, requestedName, f, protocol, game_version, cbits, player_age, GDPR, FCMToken, category, totalPlaytime, klv, meta, fhash, rid, platformID, deviceVersion, country, hash, mac, wk.)

> In the *live* flow (`fetch_newly_credentials`) this is called with `identity = None`, so ALL the defaults above are used (mac `02:00:00:00:00:00`, wk `NONE0`, platform `15,1,0`). The per-account identity is reserved for the ENet packet, NOT this POST.

#### `get_requestly_dashboard_proxied(login_url, login_info, meta, proxy_url)` (REQUESTLY)
Body = `build_requestly_dashboard_body(login_info, meta)`: reads a captured POST body from `requestly_logs.har` and patches only `protocol`, `game_version`, `meta`, `fhash` into it (see §4.4). POST to `https://{login_url}/player/login/dashboard` (**no valKey query**), headers: **Chrome/Edge Win UA**, `Content-Type: application/x-www-form-urlencoded`, `Origin: https://{login_url}`. Then same redirect loop + parse.

#### `replay_har_dashboard_proxied(login_url, post_data, proxy_url)`
POST the given `post_data` verbatim to `https://{login_url}/player/login/dashboard`, headers: Chrome/Edge Win UA, urlencoded, **`Origin: null`** (literal string). Same loop + parse. (Direct HAR replay path.)

#### `build_agent(proxy_url) -> Result<ureq::Agent>`
30s global timeout, **`max_redirects(0)`**, optional proxy. C++: one libcurl handle with `CURLOPT_FOLLOWLOCATION=0`, `CURLOPT_TIMEOUT=30`, proxy set to `socks5h://…` (use **socks5h** so DNS resolves proxy-side) when a proxy URL is given.

#### `parse_dashboard_response(html, login_url) -> Result<DashboardLinks>`
1. If `html.trim_start()` starts with `{` → it's a JSON error. Extract `["message"]` (or use the raw body) → error `"Dashboard returned error: {msg}"`.
2. Parse HTML, select all `<a>`. For each with an `onclick` attribute:
   - contains `optionChose('Apple')` → `apple = normalize_login_href(href)`
   - contains `optionChose('Google')` → `google = …`
   - contains `optionChose('Grow')` → `growtopia = …`
3. Return `DashboardLinks{apple,google,growtopia}`.

C++: manual scan of `<a ...>` tags; for each tag read `onclick` and `href` attributes; match the three literal `optionChose('X')` substrings.

#### `normalize_login_href(href, login_url) -> String`
- If `href` starts with `http://` or `https://` → return as-is.
- Else compute `host`: strip `https://`/`http://` prefix from `login_url`, trim trailing `/`; if empty → `LOGIN_HOST` (`login.growtopiagame.com`).
- If `href` starts with `/` → `https://{host}{href}`, else → `https://{host}/{href}`.

#### `extract_html_redirect(html, login_url) -> Option<String>`
1. Find `<meta http-equiv="refresh" content="...">` (http-equiv compared case-insensitively == `refresh`). In `content`, find `url=` (case-insensitive), take the rest, trim, strip surrounding `'`/`"` → `normalize_login_href`.
2. Else, first `<a href>` whose href contains `/steam/redirect` or `/player/login/dashboard` → normalize.
3. Else None.

#### `build_pipe_body(fields: &[(k,v)]) -> String`
`fields.map(|(k,v)| "{k}|{v}\n").concat()`. Order preserved. **Values are NOT URL-encoded** here (raw pipe body).

### 4.3 Requestly HAR body building (`dashboard.rs`)

#### `find_requestly_har_path() -> Result<PathBuf>`
Search `requestly_logs.har` starting from CWD and (if available) the exe's parent dir; for each start, walk it and all ancestor directories (`ancestors()`), return the first existing file. Error if none: `"requestly_logs.har not found in current directory or parents"`.

#### `extract_har_dashboard_post(path) -> Result<String>`
Read HAR JSON, iterate `log.entries`; return the first entry whose `request.method` == `POST` (case-insensitive) AND `request.url` contains `/player/login/dashboard`, returning `request.postData.text`. Error if `log.entries` missing or no matching POST.

#### `build_requestly_dashboard_body(login_info, meta) -> Result<String>`
Get the HAR dashboard POST body (URL-**encoded** form text), then `replace_encoded_pipe_field` for `protocol`, `game_version`, `meta`, `fhash` (= `"-716928004"`).

#### `replace_encoded_pipe_field(encoded, key, value) -> Result<String>`
The captured body is a **URL-encoded** pipe body (lines encoded, `|`→`%7C`, `\n`→`%0A`, optionally `\r`→`%0D`). This finds `key%7C` at a line-start and replaces the value up to the next `%0A`.
- Candidate markers tried in order: `strict_form_encode(key)+"%7C"`, `key+"%7C"`, `key.replace('_',"%5F")+"%7C"`.
- For each marker, scan for occurrences; accept only if it's at line start (offset 0, or preceded by `%0A`, or `%0D%0A`).
- Value spans from after the marker to the next `%0A` (or end). If the 3 chars before value_end are `%0D` (case-insensitive), trim them off.
- Rebuild: `encoded[..value_start] + strict_form_encode(value) + encoded[value_end..]`. Return.
- If no marker matched → error `"requestly_logs.har dashboard body is missing {key}"`.

#### `strict_form_encode(value) -> String`
Per byte: `A-Z a-z 0-9` → literal; space → `+`; everything else → `%XX` (uppercase hex). (This is application/x-www-form-urlencoded strict encoding.)

#### `pipe_field(body, key) -> Option<&str>`
Helper: split each line on first `|`; return value where key matches exactly. (Used for decoded bodies.)

### 4.4 `har_parser.rs`

#### `extract_auth_data(file_path) -> anyhow::Result<AuthData>`
1. `payload_map = extract_login_payload(file_path)` (§below).
2. `token = payload_map["token"]` (or "").
3. If `payload_map["UbiTicket"]` non-empty → `token = UbiTicket` (UbiTicket wins).
4. `steam_token = payload_map["steamToken"]`.
5. If both `token` and `steam_token` empty → error `"Token/UbiTicket not found in HAR payload"`.
6. `rid`, `mac`, `wk` are **required** (error if missing: `"RID/MAC/WK not found in HAR payload"`).
7. Re-read file, deserialize `HarFile`; find first POST to `login.growtopiagame.com/player/login/dashboard`, capture `request.postData.text` as `post_data` (raw).
8. Build `AuthData` pulling `klv, hash, hash2, fz, platformID→platform_id, meta` (default "" if absent), plus `token, rid, mac, wk, steam_token, post_data`.

#### `extract_har_growid_login_url(file_path) -> anyhow::Result<String>`
Deserialize HAR; for each entry:
- If method==GET (ci) and url contains `login.growtopiagame.com/player/growid/login` AND `token=` → return that url.
- Else if `response.content.text` contains a growid login URL via `find_growid_login_url` → return it.
Error if none: `"GrowID login URL not found in HAR"`.

#### `find_growid_login_url(text) -> Option<String>`
Find literal `https://login.growtopiagame.com/player/growid/login?token=`; from there, cut at the first `"`, `'`, `<`, or whitespace char. Return the slice.

#### `extract_login_payload(file_path) -> anyhow::Result<HashMap<String,String>>`
Parses the dashboard POST body + any `token` query param into a flat key→value map.
1. Deserialize HAR.
2. For each entry: for each `request.queryString` param named `token` → URL-decode value, insert `map["token"]`.
3. For the first (`found_dashboard` set true, but loop continues over all) POST to `…/player/login/dashboard`: URL-decode `postData.text`, split on `\n` OR `&`; for each part: split on first `|` (pipe body) else first `=` (query body); key trimmed; value: trimmed, `+`→space, then URL-decoded; insert into map.
4. If no dashboard POST found → error `"No login dashboard POST found in HAR"`.
Returns the map. (Later parts override earlier — last write wins.)

`HarHeader::value_str()`: header `value` may be a JSON string or array; string → itself, array → first element as string, else "".

### 4.5 `bot/auth.rs` — orchestration (the heart)

#### `fetch_credentials(username, password, proxy, login_proxy, stop, log) -> Option<Credentials>`
→ `fetch_credentials_with_dashboard(..., DashboardMode::Legacy)`.

#### `fetch_requestly_credentials(...)` → same with `DashboardMode::Requestly`.

#### `fetch_newly_credentials(username, password, proxy, login_proxy, stop, log) -> Option<Credentials>` (the current/primary path)
`game_proxy_url = proxy.map(to_url)`. If `login_proxy` present, log two lines: `"[Bot] rotating login proxy enabled for newly HTTP login"`, `"[Bot] newly HTTP login will not fall back to assigned game proxy"`.
`login_info = { PROTOCOL(226), GAME_VER("5.51") }`.
**Loop forever:**
1. If `stop` set → log `"[Bot] login aborted — bot was stopped"`, return None.
2. `(proxy_candidates, bypass_session) = login_attempt_proxies(login_proxy, &game_proxy_url)` (§below). If `login_proxy` set but `bypass_session` None → log `"[Bot] newly fetch: bypass login proxy is not SOCKS5-UDP capable — cannot pin logon IP; retrying in 5s"`.
3. `bypass_enet = bypass_session.map(|s| s.enet.clone())`.
4. For each `(proxy_label, proxy_url)` candidate:
   a. If `stop` → return None.
   b. `server_data = fetch_server_data_candidate(login_info, proxy_label, proxy_url, "newly", "newly fetch", log)`; if None → continue.
   c. Parse `addr = "{server}:{port}"` as SocketAddr; on parse error log `"[Bot] fetch: invalid server address '{server}:{port}' via {label} ({e}) — skipping candidate"` and continue (do NOT panic — server value is proxy-sourced/untrusted).
   d. `identity = newly_login_identity(username, log)` (default identity + account device overrides).
   e. `pace_dashboard()` — **fleet-wide gate** before the dashboard POST (§8).
   f. `dashboard = get_newly_dashboard_proxied(server_data.loginurl, login_info, server_data.meta, proxy_url, None)` — **identity=None** on purpose (canonical 22-field POST). On error: log `"[Bot] newly fetch: dashboard failed via {label}: {e} — retrying"` and continue (newly does NOT use the Requestly HAR fallback).
   g. `growtopia_url = dashboard.growtopia`; if None → log `"[Bot] newly fetch: no Growtopia URL via {label}"`, continue.
   h. `ltoken = get_legacy_token_proxied(growtopia_url, username, password, proxy_url)`:
      - `Exhausted`/`WrongCredentials` → log `"[Bot] newly fetch: login failed via {label}: {e}"`, then `handle_terminal_login_error(e, stop, log)` → returns None (which continues) after setting stop.
      - other `Err` → log same line, continue.
      - `Ok(token)` → use it.
   i. Log `"[Bot] newly using raw dashboard login token via {label}"`, `"[Bot] Got token: {ltoken}"`. If bypass, log the pin line (§7).
   j. Return `Credentials{ ltoken, meta: server_data.meta, addr, identity, bypass_enet: bypass_enet.clone() }`.
5. After all candidates fail: log `"[Bot] newly fetch: all HTTP login proxy candidates failed - retrying in 5s"`, sleep 5s, loop.

> **Newly path never calls `check_token`, never touches HAR.** It uses the raw dashboard→growid token directly as `ltoken`.

#### `fetch_credentials_with_dashboard(..., mode: Legacy|Requestly) -> Option<Credentials>`
Same loop skeleton as newly, but the token pipeline is richer:
1–3. Same setup (log lines say "HTTP login" not "newly HTTP login").
4. Per candidate: fetch server_data; parse addr (same untrusted-guard); `pace_dashboard()`.
   - `dashboard =` Legacy→`get_dashboard_proxied`, Requestly→`get_requestly_dashboard_proxied`.
   - `growtopia_url = dashboard.growtopia` (on dashboard Err: log `"[Bot] fetch: dashboard failed via {label}: {e}"`, set None).
   - **Legacy-only fallback:** if `growtopia_url` None and mode is Legacy → log `"[Bot] legacy dashboard had no Growtopia URL; trying Requestly HAR dashboard"`, try `get_requestly_dashboard_proxied`; on Ok set growtopia; on Err log `"[Bot] Requestly HAR dashboard failed via {label}: {e}"`.
   - **Token resolution:**
     - If `growtopia_url` Some: `get_legacy_token_proxied(url, user, pass, proxy)`:
       - Ok(t) → `prepare_login_token(username, t, meta, proxy, log)`; if that returns None → `try_har_growid_login(...)` (Ok(Some)→use / Ok(None)→None / Err→`handle_terminal_login_error`).
       - Err Exhausted/WrongCredentials → log fail line + `handle_terminal_login_error`.
       - other Err → log fail line + `try_har_growid_login(...)` chain.
     - Else (no growtopia_url): `try_har_growid_login(...)` chain.
   - If final `login_token` None → log `"[Bot] fetch: no usable GrowID token via {label}"`, continue.
   - Log `"[Bot] using dashboard login token directly via {label} (checktoken skipped for ENet login)"`, `"[Bot] Got token: {ltoken}"`. `identity = load_har_login_identity(username, log)`. If bypass, log pin line.
   - Return `Credentials{ltoken, meta, addr, identity, bypass_enet}`.
5. All fail → log `"[Bot] fetch: all HTTP login proxy candidates failed - retrying in 5s"`, sleep 5s.

#### `login_attempt_proxies(login_proxy, game_proxy_url) -> (Vec<(&str, Option<String>)>, Option<BypassLoginSession>)`
Decides which HTTP proxy candidate(s) to try AND mints the paired ENet bypass session.
- If `login_proxy` Some: `rot.login_session()`:
  - Some(session) → candidates = `[("bypass login proxy", Some(session.http_url))]`, return `(candidates, Some(session))`. (**Only the SOCKS5 endpoint is offered** — no http:// fallback; that only produced `ConnectionReset (10054)` spam.)
  - None → `(vec![], None)` (endpoint not SOCKS5-UDP capable; caller logs+retries).
- Else if `game_proxy_url` Some(url) → `([("assigned game proxy", Some(url))], None)`.
- Else → `([("direct", None)], None)`.

#### `alternate_scheme_url(url) -> Option<String>`
Flip scheme, preserving host/port/creds → same exit IP: `socks5h://`→`http://`, `socks5://`→`http://`, `http://`→`socks5h://`, `https://`→`socks5h://`, else None. (Recovery helper when the rotating endpoint's protocol is unknown.)

#### `fetch_server_data_candidate(login_info, proxy_label, proxy_url, login_mode, failure_label, log) -> Option<ServerData>`
Try `get_server_data_proxied(alternate, …)` for `alternate ∈ [false, true]` (primary growtopia1, then growtopia2):
- Log `"[Bot] fetching server_data (alternate={alt}, login_mode={mode}, http_proxy={label})..."`.
- On Ok → return.
- On Err → log `"[Bot] {failure_label}: server_data failed via {label}: {msg}"`. If `is_http_403_text(msg)` (contains `"403"` or `"forbidden"`, ci) → log `"[Bot] {failure_label}: proxy returned 403; skipping growtopia2 alternate for {label}"` and break (don't try alternate). Return None.

#### `newly_login_identity(username, log) -> Option<LoginIdentity>`
`Some(apply_account_device_identity(username, default_login_identity(), log))`.

#### `default_login_identity() -> LoginIdentity`
See §2.7.

#### `apply_account_device_identity(username, mut identity, log) -> LoginIdentity`
- `device = account_devices::get_or_create(username)`: `Ok(Some)`→use; `Ok(None)`→return identity unchanged; `Err`→log `"[Bot] account device identity unavailable: {e}"`, return unchanged.
- `hash_val = device.hash.parse::<i32>()`; on failure log `"[Bot] account device hash was not numeric ({hash}); using default hash"` and use `DEFAULT_HASH` i32.
- `klv = compute_klv(GAME_VER, "226", device.rid, hash_val)`.
- Override `identity.rid/mac/wk/hash/hash2/zf` from device, set `identity.klv = klv`.
- Log `"[Bot] using account device identity for {trimmed_username} (rid={first6}..., mac={mac})"`.

#### `try_har_growid_login(username, password, meta, proxy_url, log) -> Result<Option<String>, LoginError>`
- `find_har_path()`; if None → log `"[Bot] HAR GrowID fallback skipped: requestly_logs.har not found"`, return Ok(None).
- `extract_har_growid_login_url(path)`; on Err → log `"[Bot] HAR GrowID fallback skipped: {e}"`, Ok(None).
- Log `"[Bot] dashboard login failed; trying HAR GrowID login URL"`.
- `get_legacy_token_proxied(growid_url, user, pass, proxy)`: Ok→`prepare_login_token(...)`; Exhausted/WrongCredentials→Err(e); other Err→log `"[Bot] HAR GrowID login failed: {e}"`, Ok(None).

#### `prepare_login_token(username, token, meta, proxy_url, log) -> Option<String>`
- If `!token_needs_checktoken(token)` (i.e. token already looks like a UbiTicket/JWT, ≥3 dot-separated parts) → return Some(token).
- Log `"[Bot] dashboard token needs checktoken ({shape}); using HAR clientData"`.
- `token_via_checktoken(...)`: if Some(upgraded) → return it.
- Else if `token_can_fallback_to_raw_enet(token)` (len ≥ 500 and not ending in `=`) → log `"[Bot] checktoken did not yield UbiTicket; falling back to raw dashboard token"`, return Some(token).
- Else log `"[Bot] dashboard token looks unusable ({shape}); trying fallback login"`, return None.

#### `token_via_checktoken(username, refresh_token, meta, proxy_url, log) -> Option<String>`
- `client_data = build_har_client_data(username, meta, log)`; None → return None.
- `check_token(refresh_token, client_data, proxy)`:
  - Ok(t) but `token_looks_suspicious(t)` (trimmed len < 400) → log `"[Bot] checktoken returned unusable token ({shape})"`, None.
  - Ok(t) → log `"[Bot] token converted via checktoken for ENet login ({shape})"`, Some(t).
  - Err(e) → log `"[Bot] checktoken failed: {e}"`, None.

#### `build_har_client_data(username, meta, log) -> Option<String>`
- `payload = load_har_login_payload(log, Some("[Bot] HAR clientData unavailable: requestly_logs.har not found"))`; None → None.
- `identity = apply_account_device_identity(username, login_identity_from_payload(payload, log), log)`.
- `ubi_ticket = payload["UbiTicket"] or ""`; `has_ubi_ticket = !trim().is_empty()`.
- Log `"[Bot] loaded login identity/clientData from configured HAR (game_version={gv}, klv_len={n}, ubi_ticket={bool})"`.
- Build `client_data` pipe string:
  - if `has_ubi_ticket`: prepend `"UbiTicket|{ubi}\n"`.
  - then (values from `identity`/payload; `PROTOCOL` and `FHASH` interpolated):
    ```
    requestedName|{payload requestedName or ""}
    f|{payload f or "1"}
    protocol|{226}
    game_version|{identity.game_version}
    fz|{identity.fz}
    cbits|{identity.cbits}
    player_age|{identity.player_age}
    GDPR|{identity.gdpr}
    FCMToken|{payload FCMToken or ""}
    category|{identity.category}
    totalPlaytime|{identity.total_playtime}
    klv|{identity.klv}
    steamToken|{identity.steam_token}
    hash2|{identity.hash2}
    meta|{meta}
    fhash|{FHASH = -716928004}
    rid|{identity.rid}
    platformID|{identity.platform_id}
    deviceVersion|{payload deviceVersion or "0"}
    country|{identity.country}
    hash|{identity.hash}
    mac|{identity.mac}
    wk|{identity.wk}
    zf|{identity.zf}
    ```
  Return Some(client_data). (This is the `clientData` for checktoken.)

#### `load_har_login_identity(username, log) -> Option<LoginIdentity>`
`payload = load_har_login_payload(log, None)`; None→None. `identity = apply_account_device_identity(username, login_identity_from_payload(payload, log), log)`. Log `"[Bot] loaded HAR login identity for ENet packet (game_version={gv}, klv_len={n})"`. Some(identity).

#### `login_identity_from_payload(payload, log) -> LoginIdentity`
`rid = payload["rid"] or DEFAULT_RID`; `hash = payload["hash"] or DEFAULT_HASH`; `hash_val = hash.parse::<i32>()` (fallback DEFAULT_HASH with log `"[Bot] HAR hash was not numeric ({hash}); using default hash"`); `fallback_klv = compute_klv(GAME_VER,"226",rid,hash_val)`. Then each field = `payload_value(payload, key, default)`:
```
game_version=GAME_VER; cbits(default "1536"); player_age("23"); GDPR→gdpr("2");
category("_16"); totalPlaytime→total_playtime("0"); country("us"); zf(DEFAULT_ZF);
rid; mac(DEFAULT_MAC); wk(DEFAULT_WK); hash; hash2(DEFAULT_HASH2); fz(DEFAULT_FZ);
platformID→platform_id(DEFAULT_PLATFORM_ID); steamToken→steam_token(DEFAULT_STEAM_TOKEN);
klv(default=fallback_klv)
```

#### `payload_value(payload, key, default) -> String`
`payload[key]` filtered to non-blank (after trim) else `default`.

#### `load_har_login_payload(log, missing_message: Option<&str>) -> Option<HashMap>`
`find_har_path()`; None → optionally log `missing_message`, return None. Else `extract_login_payload(path)`: Ok→Some; Err→log `"[Bot] HAR login payload extraction failed: {e}"`, None.

#### `find_har_path() -> Option<PathBuf>`
Starts = [CWD, exe-parent]. For each start, walk it + all ancestors; for each dir, try filenames `["requestly_logs.har", "orijinal_requestly_logs.har"]`; return first existing file. Else None.

#### Token classification helpers
```
token_looks_suspicious(t)       = trim(t).len() < 400
token_needs_checktoken(t)       = !token_is_ubi_ticket(t)
token_can_fallback_to_raw_enet(t)= trim(t).len() >= 500 && !ends_with('=')
token_is_ubi_ticket(t)          = trim(t).split('.').count() >= 3   // JWT-ish, ≥3 dot parts
token_shape(t)                  = "len={len}, padded={ends_with('=')}"
```

#### `handle_terminal_login_error(error, stop, log) -> Option<String>`
- `Exhausted` → log `"[Bot] login attempts exhausted (24h) — stopping this bot cleanly (no crash)"`, `stop.store(true)`, return None. (24h GT login ban — do not retry.)
- `WrongCredentials` → log `"[Bot] wrong credentials — stopping this bot cleanly (no crash)"`, `stop.store(true)`, None.
- `Other(msg)` → log `"[Bot] login failed: {msg}"`, None.

### 4.6 `server_data.rs`

#### `get_server_data_proxied(alternate, login_info, proxy_url) -> Result<ServerData>`
→ `fetch_server_data_from_endpoint`. (`get_server_data` = None proxy; `get_server_data_proxied_live` identical.)

#### `fetch_server_data_from_endpoint(alternate, login_info, proxy_url)`
- `url =` alternate? `https://www.growtopia2.com/growtopia/server_data.php` : `https://www.growtopia1.com/growtopia/server_data.php`.
- Print `"[server_data] proxy_url={redacted}"` (redact user:pass → `***`).
- Agent: 20s timeout, **TLS verification disabled**, optional proxy.
- **POST** with `User-Agent: UbiServices_SDK_2022.Release.9_PC64_ansi_static`, `Content-Type: application/x-www-form-urlencoded`, body `platform=0&protocol={protocol}&version={game_version}`.
- On Ok: read body, log it, `ServerData::parse_from_response(body)`; if `has_required_login_fields()` → Ok, else Err `"server_data response from {url} is missing login fields"`.
- On Err: log and propagate.

### 4.7 `account_devices.rs`

#### `get_or_create(username) -> io::Result<Option<AccountDevice>>`
- `key = username.trim().to_ascii_lowercase()`; if empty → Ok(None).
- Lock the process-global `STORE_LOCK` mutex.
- Read `data/account_devices.json` into store. If `store.devices[key]` exists AND it's **not** the default device (rid/mac/wk not all equal to DEFAULT_* case-insensitively) → return it (clone).
- Else `device = generate_device()` (fully random), insert, write store, return it. (Self-heal: entries that are just the shared DEFAULT placeholder are replaced with a fresh unique device so bots don't all present as one device.)

#### `upsert_from_login_token(username, login_token) -> io::Result<bool>`
- `key` as above; `parse_login_token_device(login_token)` splits on `|`; must be exactly 4 parts with parts[1..3] non-empty → `(rid, mac, wk) = (parts[1], parts[2], parts[3])`; else Ok(false).
- Lock store. If `(rid,mac,wk)` is the DEFAULT placeholder: if key exists → Ok(false); else insert `generate_device()`, write, Ok(true).
- Else: take existing device or `default_device()`; if any of rid/mac/wk differ, reset hash/hash2/zf to DEFAULT_*; set rid/mac/wk; insert, write, Ok(true).

#### Generators
- `generate_device()`: rid=`random_hex32`, mac=`random_mac`, wk=`random_hex32`, hash=`random_i32`, hash2=`random_i32`, zf=`random_i32`.
- `default_device()`: rid=random_hex32, mac=random_mac, wk=random_hex32, hash=DEFAULT_HASH, hash2=DEFAULT_HASH2, zf=DEFAULT_ZF.
- `random_hex32()` = UUIDv4 simple form (32 hex) uppercased.
- `random_i32()` = first 4 bytes of a UUIDv4 as little-endian i32, decimal string.
- `random_mac()` = 6 bytes of a UUIDv4; byte0 forced to `(b & 0xFE) | 0x02` (locally-administered unicast); format `xx:xx:xx:xx:xx:xx` lowercase hex.

C++: use a CSPRNG (or `std::random_device`+`mt19937_64`); UUIDv4 not required — just generate the right shapes. Keep the MAC bit-twiddle exactly.

---

## 5. Local operator auth (`auth.rs`) — the control-panel unlock, NOT GT login

This gates access to the Mori panel/web UI. In Nxrth (native Dear ImGui, no web server) this becomes an optional local password unlock; keep the crypto identical if you want existing `data/user.json` to remain valid.

- `UserRecord{ password_hash }` persisted to `data/user.json` (JSON, pretty).
- `user_path()` = `CWD/data/user.json`.
- `AuthState::new()`: load hash from disk if present; `session_token=None`.
- `is_registered()` = hash present.
- `register(password)`: `salt = SaltString::generate(OsRng)`; `hash = Argon2::default().hash_password(pw, salt).to_string()` (Argon2**id**, default params, PHC string); save to disk; store hash; clear session.
- `login(password)`: parse stored PHC; `Argon2::default().verify_password(pw, parsed)`; on success generate a UUIDv4 session token, store it, return Some(token). On mismatch → None.
- `validate_token(token)` = session_token == token.
- `logout()` = session_token = None.

Deps: `argon2` crate → C++ `argon2` library (`argon2id_hash_encoded` / `argon2id_verify` with the reference lib). Session token: any UUIDv4/128-bit random hex. State: single-user, single-session; guard with a mutex.

---

## 6. Output → ENet logon packet (`bot/core.rs::build_login_packet`)

How the module's output (`ltoken`, `identity`, `platform_id`, `meta`) feeds the ENet logon. Included so the port produces a token that the logon step can actually use.

`login_token_field(token)` = if `token.split('.').count() >= 3` → `"UbiTicket"` else `"token"`.

Three shapes:
1. **Newly** (`LoginMethod::Newly`): minimal packet —
   ```
   protocol|226
   ltoken|<ltoken>
   platformID|<platform_id>       // 15,1,0
   ```
2. **UbiTicket-shaped token** (any mode, when `login_token_field(ltoken)=="UbiTicket"`): full field packet keyed by `UbiTicket|…` (requestedName, f|1, protocol|226, game_version, fz, cbits, player_age, GDPR, FCMToken, category, totalPlaytime, klv, steamToken, hash2, meta, fhash|-716928004, rid, platformID, deviceVersion|0, country, hash, mac, wk, zf).
3. **Plain token** (Legacy/Requestly, non-UbiTicket): full field packet keyed by `protocol|226\nltoken|<t>\nplatformID|…\nrequestedName|…` then the same identity fields (no separate UbiTicket line).

The packet is a `\n`-joined pipe body; it's sent as ENet packet type `2` (text) after ServerHello. The `bypass_enet` in `Credentials` pins the ENet host's exit IP (see §7).

---

## 7. Token IP-binding (critical correctness constraint)

The Growtopia **`ltoken` is bound to the IP that minted it.** The HTTP token fetch (growid dashboard + `/growid/login/validate`) and the subsequent ENet logon (`protocol|226/225` to the gateway) **must leave from the same IP**, or the gateway answers "Fail to login".

Mechanism in this module:
- When a rotating/bypass login proxy is configured, `login_attempt_proxies` mints **one sticky `BypassLoginSession`** = a single exit IP with two views: `http_url` (SOCKS5 for the HTTP fetch) and `enet` (`Socks5Config` for the ENet logon). Both point at the same host:port → same exit IP.
- The HTTP token fetch runs through `http_url`; `Credentials.bypass_enet = session.enet` is carried out so the ENet host is created with that same SOCKS5 config (`create_host(bypass_enet.or(proxy))` in core.rs). World/subserver traffic afterward continues on the *assigned game proxy* (only the logon is pinned).
- If `login_session()` returns None (endpoint not SOCKS5-UDP capable) there are **no candidates** — the bot logs and retries in 5s. There is no http:// fallback (it can't speak to a SOCKS5 listener; it only spammed `ConnectionReset (10054)`).
- Log line on success: `"[Bot] bypass logon: token pinned to logon IP {proxy_addr} — ENet logon will use it, world stays on game proxy"`.

**Nxrth/C++ requirement:** the vendored ENet must be patched for SOCKS5-UDP (UDP ASSOCIATE), and both the libcurl HTTP fetch and the ENet logon must be given the identical SOCKS5 endpoint for a given login attempt. Use `socks5h://` for the HTTP side so DNS also resolves proxy-side (consistent egress). See also `MEMORY.md: GT ltoken IP-binding` — a rotating-per-stage proxy breaks login; use a consistent pool proxy.

---

## 8. Threading & shared state

- **Per-bot thread model:** each bot runs on its own `std::thread`. `fetch_*credentials` is called synchronously on that thread during construction/reconnect. The functions loop-with-sleep (5s between full retry rounds) and poll a shared `stop: AtomicBool` at each iteration/candidate to allow clean cancellation. In C++: `std::atomic<bool> stop`, checked at the same points.
- **`log` callback:** `&mut dyn FnMut(String)` — a per-bot logger that appends to the bot's console ring buffer (cap 100 lines) and optionally pushes to the UI. In Nxrth this is a `std::function<void(std::string)>` writing to the bot's log buffer + Dear ImGui console; no web socket.
- **Fleet-wide dashboard gate (`pace_dashboard` / `DASHBOARD_GATE`):** a process-global `Mutex<Instant>` (via `OnceLock`) hands out time slots spaced **`DASHBOARD_STAGGER_MS = 3500`ms** apart, capped `GATE_HTTP_MAX_AHEAD_MS = 300_000`ms (5 min) ahead. Every dashboard POST across the whole fleet passes through this gate so the shared bypass subnet doesn't trip the endpoint's per-subnet rate limit ("Please try login again" — the dominant bulk-login wall). `reserve_gate_slot(gate, spacing, max_ahead)`: lock, `slot = clamp(*next_allowed, now, now+max_ahead)`, `*next_allowed = min(slot+spacing, horizon)`, return slot; `wait_global_gate` sleeps until slot **without holding the lock**. C++: a `std::mutex` + a `steady_clock::time_point next_allowed`; compute slot, release the lock, then `sleep_until(slot)`.
- **HTTP-login gate (`HTTP_LOGIN_GATE` / `HTTP_LOGIN_STAGGER_MS = 2500`):** a *separate* fleet gate wrapping the whole HTTP re-login (server_data+dashboard+growid) on constructor/reconnect/refresh paths (`pace_http_login`). Both gates use the same `reserve_gate_slot`/`wait_global_gate` machinery but distinct static instances. Port both as distinct global gate objects.
- **Account-device store (`account_devices.rs`):** guarded by a process-global `STORE_LOCK: Mutex<()>`. All reads/writes of `data/account_devices.json` take this lock (read-modify-write is not atomic otherwise). C++: one `std::mutex` guarding the JSON file; load→mutate→store under the lock.
- **`AuthState` (control-panel):** `Arc<RwLock<AuthInner>>`, single-user/single-session. Port as `std::shared_mutex` (or plain mutex) around `{optional<string> password_hash, optional<string> session_token}`.
- **Fleet awareness:** the two fleet gates + the `LOGIN_PROXY_RR` round-robin cursor (in `RotatingLoginProxy::pick`, `AtomicUsize`) are the cross-bot shared state relevant here — bots coordinate login pacing and exit-IP spreading through them. In Nxrth these belong to a shared/fleet singleton (a `LoginCoordinator`) with a mutex+condvar queue for pacing and an atomic round-robin index for proxy picking. Keep them process-global so all bot threads share one schedule.

---

## 9. Dependency mapping (Rust crate → Nxrth C++)

| Rust | Used for (here) | Nxrth C++ |
|------|-----------------|------------|
| `ureq` (agent, proxy, timeout, TLS) | all HTTP: server_data, dashboard, growid, checktoken | **libcurl**. `CURLOPT_PROXY` = `socks5h://user:pass@host:port` (proxy-side DNS). `CURLOPT_TIMEOUT` = 20/30s. `CURLOPT_FOLLOWLOCATION=0` for dashboard (manual redirects). server_data: `CURLOPT_SSL_VERIFYPEER/HOST=0` (TLS verification disabled). Enable cookie engine if needed. |
| `serde_json` / `serde` | HAR parse, account_devices.json, user.json | **nlohmann/json**. HAR: tolerate missing fields (defaults ""/[]). |
| `scraper` (`Html`, `Selector`) | CSRF `_token`, dashboard `<a onclick>`, meta-refresh | **manual HTML scan / regex**. No DOM lib. Scan `<input name="_token" value="...">`, `<a onclick href>`, `<meta http-equiv="refresh" content="...url=...">`, and `.text-danger.text-danger-wrapper` error text. |
| `md5` | `compute_klv` (`md5u`) | **bundled MD5** → uppercase hex. |
| `argon2` + `rand_core::OsRng` + `SaltString` | control-panel password (`auth.rs`) | **argon2 reference lib** (Argon2id, default params, PHC string). CSPRNG salt. |
| `uuid` | session token, account-device rid/mac/wk/i32 | any UUIDv4/128-bit random; replicate the shapes (32-hex upper, LE i32, MAC bit-twiddle). |
| `url` | redact proxy creds in log | manual parse or your URL util; just mask user:pass. |
| `urlencoding` | HAR payload decode; strict form-encode | your own encode/decode (`%XX`, `+`↔space) matching `strict_form_encode`. |
| `rusty_enet` (elsewhere) | ENet logon that consumes `ltoken`/`bypass_enet` | **vendored C ENet patched for SOCKS5-UDP**. |
| `crossbeam-channel` (elsewhere) | bot command channel | **std::mutex+condvar queue**. |
| `tokio/axum/tower` (web UI) | control panel HTTP server | **NOT ported** — native Dear ImGui. `AuthState` becomes a local unlock. |
| `mlua` (elsewhere) | scripting | **native C++** (no Lua). Not used in this module. |
| `std::sync::{Mutex,RwLock,OnceLock,AtomicBool,AtomicUsize}` | gates, store lock, stop flags | `std::mutex/shared_mutex`, function-local static / `std::once_flag`, `std::atomic`. |

---

## 10. RENAME RULES (Mori→Nxrth, mori→nxrth, Cloei→North, cloei→north)

Apply to every identifier, path, log line, window title, user-agent, config filename in the C++ port. Note: **no Growtopia protocol string may change** — UA strings sent to GT (`UbiServices_SDK_…`, the Mozilla/Chrome UAs), field names, URLs, valKey, klv keys, `RTENDMARKERBS1001`, etc. are **wire-fixed and must stay verbatim**.

**Concrete occurrences found in the read files (rename in comments/logs/docs only):**
- `dashboard.rs:196` comment: `"…canonical growid dashboard POST (upstream cloei/mori) sends exactly the 22 fields…"` → `"…(upstream north/nxrth)…"`.
- `bot/auth.rs:238` comment: `"…canonical upstream (cloei/mori) fixed web values…"` → `"…(north/nxrth)…"`.
- `bot/core.rs` `LoginMethod::Newly` doc comment: `"Original Mori-style GrowID login without HAR fallbacks."` → `"Original Nxrth-style GrowID login without HAR fallbacks."`.
- The `crate::` module path root (implicitly `mori`) and the source tree name `Mori-2.0.0` → `nxrth` / `Nxrth-*`.
- Repo/folder codename `MoriReborn` (build dir) → `NxrthReborn` (or per project convention).

**Not present in these files but rename globally if they appear elsewhere:** the binary name `Mori.exe`→`Nxrth.exe` (see `MEMORY.md: Mori build→copy convention`), window title `"Mori"`→`"Nxrth"`, any UA/config filename literally containing `mori`, and any `cloei` repo references. The log-line prefix `[Bot]` and `[server_data]` contain no Mori/Cloei tokens and stay as-is (or rebrand consistently if desired — not required). Data filenames `requestly_logs.har`, `orijinal_requestly_logs.har`, `data/user.json`, `data/account_devices.json` contain no Mori/Cloei tokens and are **kept as-is** (they are external capture / on-disk store names, and the HAR filename is matched literally).

---

## 11. Port checklist (correctness-critical, do not skip)

1. **Newly dashboard POST = EXACTLY 22 fields, order per §4.2, `platformID=15,1,0`. Never add `fz`/`hash2`/`zf`/`steamToken`** → 5.51 returns HTTP 500 otherwise. (Legacy POST *does* include `fz`+`hash2` and uses `platformID=0,1,1`.)
2. `PROTOCOL=226`, `GAME_VER="5.51"`, `FHASH=-716928004`. klv via triple/double MD5 chain in §3, exact key order.
3. Dashboard agent: `max_redirects=0` + manual follow ≤4 hops; server_data agent: TLS verify OFF.
4. growid validate: POST `_token`+`growId`+`password`, headers incl. `Origin`+`Referer`; parse JSON `token`, else HTML `.text-danger.text-danger-wrapper` → classify Exhausted/WrongCredentials/Other.
5. IP-binding (§7): same SOCKS5 exit IP for the HTTP fetch and the ENet logon; SOCKS5-only, no http fallback; retry 5s when not SOCKS5-UDP capable.
6. Fleet gates: dashboard 3500ms, http-login 2500ms, 5-min horizon, sleep-without-lock. account-device store under a global mutex.
7. Untrusted `server:port` from proxy → never crash on parse failure; skip candidate.
8. Terminal errors (Exhausted/WrongCredentials) set `stop=true` and stop the bot cleanly (no crash, no retry).
9. Per-account device identity (rid/mac/wk/hash/hash2/zf + recomputed klv) is for the **ENet packet**, not the newly dashboard POST.
