// Nxrth — GrowID login orchestration (ported from Nxrth login.rs + bot/auth.rs).
// See docs/port-specs/05-login.md.
//
// Supports classic GrowID credentials, Google OAuth refresh-token records, and
// provider-formatted gateway tokens.
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

#include "net/server_data.h"    // nxrth::net::LoginInfo
#include "net/socks5_udp.h"     // nxrth::net::Socks5Config
#include "proxy/proxy_pool.h"   // nxrth::proxy::RotatingLoginProxy

namespace nxrth::login {

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
    std::string vid;  // provider validate-token device id (naae build_protocol_body vid|)
    std::string tank_id_name;  // OAuth checktoken clientData tankIDName (the account name)
};

// Secret-safe fingerprint for logs: "len=<n> sha=<first 12 hex of SHA-256>".
// NEVER contains the raw value — the ONLY representation of a token allowed in logs.
std::string token_fingerprint(const std::string& secret);

// Default ENet identity used by credential-based login.
LoginIdentity default_login_identity();

// Overrides rid/mac/wk/hash/hash2/zf from the per-account device store and
// recomputes klv with the device's rid+hash. Unchanged if no device exists.
LoginIdentity apply_account_device_identity(const std::string& username, LoginIdentity identity,
                                            const LogFn& log);

// default_login_identity() + apply_account_device_identity(). Always Some.
std::optional<LoginIdentity> login_identity(const std::string& username, const LogFn& log);

// --- module output (§2.6) ----------------------------------------------------
struct Credentials {
    std::string ltoken;                  // GrowID token for the ENet logon packet
    std::string meta;                    // from server_data
    std::string server;                  // game server host / IP
    std::uint16_t port = 0;              // game server port
    std::optional<LoginIdentity> identity;
    // Pinned SOCKS5 exit shared by provider server_data and ENet.
    std::optional<nxrth::net::Socks5Config> bypass_enet;
};

// --- login modes (§2.14) -----------------------------------------------------
enum class LoginMethodKind {
    Legacy,  // classic GrowID (platformID 0,1,1)
    Ltoken,  // Google OAuth refresh token validated through checktoken
    ProviderLtoken,  // provider gateway token sent through a pinned exit
};

struct LoginMethod {
    LoginMethodKind kind = LoginMethodKind::Legacy;
    std::string password;      // Legacy only
    std::string source_token;  // Ltoken/ProviderLtoken: original refresh token for re-exchange
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

// Exchange/validate a Google OAuth refresh token through Growtopia's checktoken
// endpoint. The proxy must use the same egress as the ENet gateway login.
LoginTokenResult check_token(const std::string& refresh_token,
                             const std::string& client_data,
                             const std::optional<std::string>& proxy_url);

// input[name='_token'] value from the login page HTML.
std::optional<std::string> extract_csrf_token(const std::string& html);

// --- orchestration -----------------------------------------------------------
// Legacy GrowID flow (platformID 0,1,1). No HAR fallbacks in Nxrth.
std::optional<Credentials> fetch_credentials(
    const std::string& username, const std::string& password,
    const std::optional<nxrth::net::Socks5Config>& proxy,
    const std::optional<nxrth::proxy::RotatingLoginProxy>& login_proxy, std::atomic<bool>& stop,
    const LogFn& log);

// --- fleet pacing gates (§8) -------------------------------------------------
// Fleet-wide gate before every dashboard POST (3500ms spacing, 5-min horizon).
void pace_dashboard();
// Fleet-wide gate wrapping the whole HTTP re-login (2500ms spacing, 5-min horizon).
void pace_http_login();

}  // namespace nxrth::login
