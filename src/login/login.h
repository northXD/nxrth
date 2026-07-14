// Adonai — GrowID login orchestration (ported from Mori login.rs + bot/auth.rs).
// See docs/port-specs/05-login.md.
//
// Turns (username, password) — or a raw token — into a Growtopia `ltoken`, a
// server address and a `meta` string ready for the ENet logon packet. The
// canonical NEWLY flow:
//   server_data POST -> growid dashboard POST (22 fields) -> parse Growtopia href
//     -> GET href (extract CSRF _token) -> growid/login/validate POST -> ltoken
//
// Token IP-binding (§7): when a rotating/bypass login proxy is configured, one
// sticky SOCKS5 exit IP is minted; the HTTP token fetch runs through it AND the
// pinned Socks5Config is carried in Credentials.bypass_enet so the ENet logon
// egresses from the SAME IP (the ltoken is bound to the minting IP).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "net/server_data.h"    // adonai::net::LoginInfo
#include "net/socks5_udp.h"     // adonai::net::Socks5Config
#include "proxy/proxy_pool.h"   // adonai::proxy::RotatingLoginProxy

namespace adonai::login {

// Per-bot logger sink (writes to the bot console / ImGui ring buffer).
using LogFn = std::function<void(const std::string&)>;

// --- errors ------------------------------------------------------------------
enum class LoginErrorKind { Exhausted, WrongCredentials, Other };

struct LoginError {
    LoginErrorKind kind = LoginErrorKind::Other;
    std::string message;

    // Exact display strings from §2.1.
    std::string display() const;
};

// Classify a login-page error message (case-insensitive `contains`):
// "exhausted" -> Exhausted; "mismatched" -> WrongCredentials; else Other(text).
LoginError classify_login_error(const std::string& text);

// --- per-account device fields for the ENet packet (§2.7) --------------------
struct LoginIdentity {
    std::string game_version;
    std::string cbits;
    std::string player_age;
    std::string gdpr;
    std::string category;
    std::string total_playtime;
    std::string country;
    std::string zf;
    std::string rid;
    std::string mac;
    std::string wk;
    std::string hash;
    std::string hash2;
    std::string fz;
    std::string platform_id;
    std::string steam_token;
    std::string klv;
};

// The identity defaults used by NEWLY (before per-account device overrides).
LoginIdentity default_login_identity();

// Overrides rid/mac/wk/hash/hash2/zf from the per-account device store and
// recomputes klv with the device's rid+hash. Unchanged if no device exists.
LoginIdentity apply_account_device_identity(const std::string& username, LoginIdentity identity,
                                            const LogFn& log);

// default_login_identity() + apply_account_device_identity(). Always Some.
std::optional<LoginIdentity> newly_login_identity(const std::string& username, const LogFn& log);

// --- module output (§2.6) ----------------------------------------------------
struct Credentials {
    std::string ltoken;                  // GrowID token for the ENet logon packet
    std::string meta;                    // from server_data
    std::string server;                  // game server host / IP
    std::uint16_t port = 0;              // game server port
    std::optional<LoginIdentity> identity;
    // Pinned SOCKS5 exit IP the ltoken is bound to. The ENet host MUST be created
    // with this config so the logon egresses from the same IP as the HTTP fetch.
    std::optional<adonai::net::Socks5Config> bypass_enet;
};

// --- login modes (§2.14) -----------------------------------------------------
enum class LoginMethodKind {
    Legacy,  // classic GrowID (platformID 0,1,1)
    Newly,   // current 5.51 GrowID (Adonai-style) — PRIMARY
    Ltoken,  // token supplied directly; no fallback on refresh
};

struct LoginMethod {
    LoginMethodKind kind = LoginMethodKind::Newly;
    std::string password;  // Legacy / Newly
    std::string token;     // Ltoken (direct)
};

// --- token fetch (§4.1) ------------------------------------------------------
struct LoginTokenResult {
    std::optional<std::string> token;  // set on success
    std::optional<LoginError> error;   // set on failure
    bool ok() const noexcept { return token.has_value(); }
};

// GET the GrowID login page `url`, extract the CSRF _token, POST
// growid/login/validate, return the ltoken (or a classified LoginError). Shared
// by all HTTP login modes as the final step.
LoginTokenResult get_legacy_token_proxied(const std::string& url, const std::string& username,
                                          const std::string& password,
                                          const std::optional<std::string>& proxy_url);

// input[name='_token'] value from the login page HTML.
std::optional<std::string> extract_csrf_token(const std::string& html);

// --- orchestration (§4.5) ----------------------------------------------------
// The primary path. Loops (5s between full retry rounds), polling `stop` at each
// candidate. `proxy` = assigned game proxy (world traffic); `login_proxy` = the
// rotating/bypass login proxy that pins the logon IP. Returns std::nullopt when
// stopped or on a terminal error (Exhausted / WrongCredentials set stop=true).
std::optional<Credentials> fetch_newly_credentials(
    const std::string& username, const std::string& password,
    const std::optional<adonai::net::Socks5Config>& proxy,
    const std::optional<adonai::proxy::RotatingLoginProxy>& login_proxy, std::atomic<bool>& stop,
    const LogFn& log);

// Legacy GrowID flow (platformID 0,1,1). No HAR fallbacks in Adonai.
std::optional<Credentials> fetch_credentials(
    const std::string& username, const std::string& password,
    const std::optional<adonai::net::Socks5Config>& proxy,
    const std::optional<adonai::proxy::RotatingLoginProxy>& login_proxy, std::atomic<bool>& stop,
    const LogFn& log);

// --- fleet pacing gates (§8) -------------------------------------------------
// Fleet-wide gate before every dashboard POST (3500ms spacing, 5-min horizon).
void pace_dashboard();
// Fleet-wide gate wrapping the whole HTTP re-login (2500ms spacing, 5-min horizon).
void pace_http_login();

}  // namespace adonai::login
