// Adonai — server_data.php fetch + parse (ported from Mori server_data.rs).
//
// An HTTPS POST to Growtopia's server_data.php (growtopia1 primary / growtopia2
// alternate) that returns a pipe-delimited key/value blob describing the game
// server to connect to. Optionally routed through an HTTP/SOCKS proxy.
//
// See docs/port-specs/03-net-socks5.md (server_data.rs section) — byte-exact.
//
// NOTE: the fetch does NOT loop growtopia1 -> growtopia2 itself. The `alternate`
// bool only *selects* one endpoint; the growtopia1-primary/growtopia2-fallback
// policy is the caller's responsibility (call with alternate=false, retry with
// alternate=true on failure).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace adonai::net {

// Login parameters for the POST body. Growtopia wire keys (do not rename).
struct LoginInfo {
    std::uint32_t protocol = 0;
    std::string game_version;

    // "protocol={protocol}&version={game_version}".
    // CAVEAT: dead code vs the live fetch body, which is built inline with a
    // "platform=0&" prefix (see fetch). Ported for completeness only.
    std::string to_form_data() const;
};

// Parsed server_data.php response. Derives Default (all numerics zero, strings
// empty). Field name follows the parsed key except the two renames noted below.
struct ServerData {
    std::string server;                 // key: server
    std::uint16_t port = 0;             // key: port   (parse error aborts the fetch)
    std::string loginurl;               // key: loginurl
    std::uint8_t server_type = 0;       // key: type   (renamed; `type` is a keyword)
    std::string beta_server;            // key: beta_server
    std::string beta_loginurl;          // key: beta_loginurl
    std::uint16_t beta_port = 0;        // key: beta_port
    std::uint8_t beta_type = 0;         // key: beta_type
    std::string beta2_server;           // key: beta2_server
    std::string beta2_loginurl;         // key: beta2_loginurl
    std::uint16_t beta2_port = 0;       // key: beta2_port
    std::uint8_t beta2_type = 0;        // key: beta2_type
    std::string beta3_server;           // key: beta3_server
    std::string beta3_loginurl;         // key: beta3_loginurl
    std::uint16_t beta3_port = 0;       // key: beta3_port
    std::uint8_t beta3_type = 0;        // key: beta3_type
    std::uint8_t type2 = 0;             // key: type2
    std::optional<std::string> maint;   // key: #maint (present only in maintenance)
    std::string meta;                   // key: meta   (required anti-bot login token)

    // Parse a pipe-delimited server_data.php body. Stops at the first line that
    // starts with RTENDMARKERBS1001; skips lines without '|'; trims key & value.
    // Returns nullopt on a numeric parse failure (hard error, mirrors Rust `?`).
    static std::optional<ServerData> parse_from_response(const std::string& response);

    // true iff server, port(!=0), loginurl and meta are all present — the minimum
    // for a usable login.
    bool has_required_login_fields() const;
};

// Mask user:pass in a proxy URL for logging (username -> "***", password -> "***").
// Returns the input unchanged if it does not parse as a URL.
std::string redact_proxy_url(const std::string& value);

// Result of a fetch. `data` is set on success; `error` describes the failure.
struct ServerDataResult {
    std::optional<ServerData> data;
    std::string error;  // empty on success
    bool ok() const noexcept { return data.has_value(); }
};

// Public entry points (all three delegate to the same fetch).
ServerDataResult get_server_data(bool alternate, const LoginInfo& login_info);
ServerDataResult get_server_data_proxied(bool alternate, const LoginInfo& login_info,
                                         const std::optional<std::string>& proxy_url);
// Identical alias to get_server_data_proxied (kept for API parity).
ServerDataResult get_server_data_proxied_live(bool alternate, const LoginInfo& login_info,
                                              const std::optional<std::string>& proxy_url);

}  // namespace adonai::net
