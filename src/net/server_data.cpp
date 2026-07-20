// Nxrth — server_data.php fetch + parse (ported from Nxrth server_data.rs).
// See docs/port-specs/03-net-socks5.md (server_data.rs section).

#include "net/server_data.h"

#include <cstddef>
#include <limits>
#include <string_view>

#include "core/constants.h"
#include "core/logger.h"
#include "net/http_client.h"

namespace nxrth::net {

namespace {

// Growtopia's real client User-Agent — server_data.php rejects anything else.
// (Wire constant; DO NOT rename.)
constexpr std::string_view kUserAgent =
    "UbiServices_SDK_2022.Release.9_PC64_ansi_static";
constexpr std::string_view kEndMarker = "RTENDMARKERBS1001";

// Trim leading/trailing ASCII whitespace (incl. \r), mirroring str::trim on the
// ASCII subset the wire uses.
std::string trim(std::string_view s) {
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    std::size_t b = 0, e = s.size();
    while (b < e && is_ws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

// Parse an unsigned integer the way Rust's <uN>::from_str does: optional leading
// '+', digits only, range-checked against T. Returns false on any deviation.
template <typename T>
bool parse_uint(const std::string& s, T& out) {
    if (s.empty()) return false;
    std::size_t i = 0;
    if (s[0] == '+') {
        i = 1;
        if (s.size() == 1) return false;
    }
    unsigned long long v = 0;
    const unsigned long long limit = static_cast<unsigned long long>(std::numeric_limits<T>::max());
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        v = v * 10ULL + static_cast<unsigned long long>(c - '0');
        if (v > limit) return false;
    }
    out = static_cast<T>(v);
    return true;
}

ServerDataResult make_err(std::string msg) {
    return ServerDataResult{std::nullopt, std::move(msg)};
}

// The real work behind all three public entry points.
ServerDataResult fetch_server_data_from_endpoint(bool alternate, const LoginInfo& login_info,
                                                 const std::optional<std::string>& proxy_url) {
    // 1. URL selection (alternate => growtopia2). No self-fallback; caller retries.
    std::string url = "https://";
    url += alternate ? constants::SERVER_DATA_HOST_2 : constants::SERVER_DATA_HOST_1;
    url += constants::SERVER_DATA_PATH;

    // 2. Log the redacted proxy (Rust `{:?}` of Option<String>).
    std::string proxy_dbg = proxy_url ? ("Some(\"" + redact_proxy_url(*proxy_url) + "\")")
                                      : std::string("None");
    nxrth::log("[server_data] proxy_url=" + proxy_dbg);

    // 3/4. Build the POST: TLS-verify OFF, 20s timeout, verbatim UA + Content-Type.
    //      Body is inline with a "platform=0&" prefix (LoginInfo::to_form_data lacks it).
    HttpRequest req;
    req.method = HttpMethod::Post;
    req.url = url;
    req.user_agent = std::string(kUserAgent);
    req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    req.body = "platform=0&protocol=" + std::to_string(login_info.protocol) +
               "&version=" + login_info.game_version;
    if (proxy_url) req.proxy = *proxy_url;
    req.timeout_secs = 20;
    req.verify_tls = false;

    HttpClient client;
    HttpResponse resp = client.Request(req);

    // 6. Transport error (mirrors ureq Err(e), including non-2xx which ureq treats
    //    as an error).
    if (!resp.ok()) {
        nxrth::log("[server_data] Error fetching " + url + ": " + resp.error);
        return make_err(resp.error);
    }
    if (resp.status >= 400) {
        std::string e = "HTTP status " + std::to_string(resp.status);
        nxrth::log("[server_data] Error fetching " + url + ": " + e);
        return make_err(e);
    }

    // 5. Success: parse, then check the required login fields. The raw body carries
    // the per-session `meta` nonce, so log a size-only summary (never the full body).
    std::optional<ServerData> parsed = ServerData::parse_from_response(resp.body);
    if (!parsed) {
        // Numeric parse failure aborted the parse (mirrors the Rust `?`).
        nxrth::log("[server_data] " + url + " parse failed (" +
                    std::to_string(resp.body.size()) + " bytes)");
        return make_err("server_data response from " + url + " failed to parse");
    }
    if (parsed->has_required_login_fields()) {
        nxrth::log("[server_data] " + url + " -> " + parsed->server + ":" +
                    std::to_string(parsed->port) + " (meta " +
                    std::to_string(parsed->meta.size()) + " bytes)");
        return ServerDataResult{std::move(parsed), std::string()};
    }
    return make_err("server_data response from " + url + " is missing login fields");
}

}  // namespace

std::string LoginInfo::to_form_data() const {
    return "protocol=" + std::to_string(protocol) + "&version=" + game_version;
}

std::optional<ServerData> ServerData::parse_from_response(const std::string& response) {
    ServerData d;
    const std::size_t n = response.size();
    std::size_t pos = 0;

    // Iterate lines split on '\n' (str::lines() semantics; \r removed by trim).
    while (pos <= n) {
        std::size_t nl = response.find('\n', pos);
        std::string_view line;
        if (nl == std::string::npos) {
            line = std::string_view(response).substr(pos);
            pos = n + 1;  // last line; terminate after this iteration
        } else {
            line = std::string_view(response).substr(pos, nl - pos);
            pos = nl + 1;
        }

        if (line.starts_with(kEndMarker)) break;  // end-of-data marker

        std::size_t bar = line.find('|');
        if (bar == std::string_view::npos) continue;  // no '|' -> skip

        std::string key = trim(line.substr(0, bar));
        std::string value = trim(line.substr(bar + 1));

        if (key == "server") {
            d.server = value;
        } else if (key == "port") {
            if (!parse_uint(value, d.port)) return std::nullopt;
        } else if (key == "loginurl") {
            d.loginurl = value;
        } else if (key == "type") {  // renamed field: server_type
            if (!parse_uint(value, d.server_type)) return std::nullopt;
        } else if (key == "beta_server") {
            d.beta_server = value;
        } else if (key == "beta_loginurl") {
            d.beta_loginurl = value;
        } else if (key == "beta_port") {
            if (!parse_uint(value, d.beta_port)) return std::nullopt;
        } else if (key == "beta_type") {
            if (!parse_uint(value, d.beta_type)) return std::nullopt;
        } else if (key == "beta2_server") {
            d.beta2_server = value;
        } else if (key == "beta2_loginurl") {
            d.beta2_loginurl = value;
        } else if (key == "beta2_port") {
            if (!parse_uint(value, d.beta2_port)) return std::nullopt;
        } else if (key == "beta2_type") {
            if (!parse_uint(value, d.beta2_type)) return std::nullopt;
        } else if (key == "beta3_server") {
            d.beta3_server = value;
        } else if (key == "beta3_loginurl") {
            d.beta3_loginurl = value;
        } else if (key == "beta3_port") {
            if (!parse_uint(value, d.beta3_port)) return std::nullopt;
        } else if (key == "beta3_type") {
            if (!parse_uint(value, d.beta3_type)) return std::nullopt;
        } else if (key == "type2") {
            if (!parse_uint(value, d.type2)) return std::nullopt;
        } else if (key == "#maint") {  // renamed field: maint
            d.maint = value;
        } else if (key == "meta") {
            d.meta = value;
        }
        // Unknown keys are ignored.
    }

    return d;
}

bool ServerData::has_required_login_fields() const {
    return !trim(server).empty() && port != 0 && !trim(loginurl).empty() && !trim(meta).empty();
}

std::string redact_proxy_url(const std::string& value) {
    // Locate the authority: after "://", up to the next '/', '?' or '#'.
    std::size_t scheme = value.find("://");
    if (scheme == std::string::npos) return value;  // not a URL -> unchanged
    std::size_t auth_start = scheme + 3;
    std::size_t auth_end = value.find_first_of("/?#", auth_start);
    if (auth_end == std::string::npos) auth_end = value.size();

    // userinfo is everything before the last '@' inside the authority.
    std::string_view authority(value.data() + auth_start, auth_end - auth_start);
    std::size_t at = authority.rfind('@');
    if (at == std::string_view::npos) return value;  // no credentials -> unchanged

    std::string_view userinfo = authority.substr(0, at);
    std::size_t colon = userinfo.find(':');
    std::string_view username =
        colon == std::string_view::npos ? userinfo : userinfo.substr(0, colon);
    bool has_password = colon != std::string_view::npos;

    std::string masked;
    if (!username.empty()) {
        masked = "***";  // non-empty username -> mask
    } else {
        masked = std::string(username);  // keep empty username as-is
    }
    if (has_password) masked += ":***";

    std::string out;
    out.reserve(value.size());
    out.append(value, 0, auth_start);              // scheme + "://"
    out += masked;                                 // masked userinfo
    out.append(value, auth_start + at, std::string::npos);  // "@host..." onward
    return out;
}

ServerDataResult get_server_data(bool alternate, const LoginInfo& login_info) {
    return get_server_data_proxied(alternate, login_info, std::nullopt);
}

ServerDataResult get_server_data_proxied(bool alternate, const LoginInfo& login_info,
                                         const std::optional<std::string>& proxy_url) {
    return fetch_server_data_from_endpoint(alternate, login_info, proxy_url);
}

ServerDataResult get_server_data_proxied_live(bool alternate, const LoginInfo& login_info,
                                              const std::optional<std::string>& proxy_url) {
    return fetch_server_data_from_endpoint(alternate, login_info, proxy_url);
}

}  // namespace nxrth::net
