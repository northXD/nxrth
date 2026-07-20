// Nxrth — GrowID login orchestration implementation (see login.h / §4).
#include "login/login.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "bot/gates.h"  // nxrth::bot fleet gates — the ONE authoritative cursor
#include "core/account_devices.h"
#include "core/constants.h"
#include "login/dashboard.h"
#include "net/http_client.h"
#include "protocol/crypto.h"

namespace nxrth::login {
namespace {

namespace consts = nxrth::constants;
using nxrth::net::HttpClient;
using nxrth::net::HttpHeader;
using nxrth::net::HttpRequest;
using nxrth::net::HttpResponse;
using nxrth::net::LoginInfo;
using nxrth::net::Socks5Config;
using nxrth::proxy::BypassLoginSession;
using nxrth::proxy::RotatingLoginProxy;

// Wire-fixed UAs / URLs (do NOT rename — sent to Growtopia).
constexpr const char* kMacUA =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)";
constexpr const char* kUbiUA = "UbiServices_SDK_2022.Release.9_PC64_ansi_static";
constexpr const char* kValidateUrl = "https://login.growtopiagame.com/player/growid/login/validate";
constexpr const char* kCheckTokenUrl =
    "https://login.growtopiagame.com/player/growid/checktoken?valKey="
    "40db4045f2d8c572efe8c4a060605726";
constexpr const char* kLoginOrigin = "https://login.growtopiagame.com";

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ci_contains(const std::string& hay, const std::string& needle) {
    return to_lower(hay).find(to_lower(needle)) != std::string::npos;
}

// application/x-www-form-urlencoded strict encoding (§4.3 strict_form_encode).
std::string form_encode(const std::string& v) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(v.size());
    for (unsigned char c : v) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// SOCKS5 proxy URL (proxy-side DNS) for the assigned game proxy fallback.
std::string socks5_config_to_url(const Socks5Config& c) {
    std::string url = "socks5h://";
    if (c.username && c.password) url += *c.username + ":" + *c.password + "@";
    url += c.host + ":" + std::to_string(c.port);
    return url;
}

// A game/pool proxy returned HTTP 403 (its exit IP is login-blocked). Quarantine
// it process-wide so no bot ever logs in through it again (choose() and
// next_game_proxy() skip quarantined endpoints), then pull a *different* clean
// proxy from the pool. Returns true with `current`/`url` swapped to the
// replacement; false (and stops the bot) when the pool has no clean replacement
// left — we NEVER fall back to a leaking direct login.
bool swap_quarantined_game_proxy(std::optional<Socks5Config>& current,
                                 std::optional<std::string>& url, std::atomic<bool>& stop,
                                 const LogFn& log) {
    const std::string key = current->host + ":" + std::to_string(current->port);
    nxrth::proxy::quarantine_proxy(key);
    log("[Bot] game proxy " + key +
        " returned 403 - quarantined fleet-wide; no bot will log in through it.");
    auto repl = nxrth::proxy::next_game_proxy(&*current);
    if (repl) {
        current = repl;
        url = socks5_config_to_url(*repl);
        log("[Bot] pulled replacement game proxy " + repl->host + ":" +
            std::to_string(repl->port) + " from the pool - retrying login now.");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return true;
    }
    log("[Bot] no clean replacement proxy left in the pool (all quarantined or only one) - "
        "refusing direct login to protect the real IP; stopping bot.");
    stop.store(true);
    return false;
}

// --- token classification helpers (§ token classification) -------------------
bool token_is_ubi_ticket(const std::string& t) {
    std::string s = trim(t);
    int parts = 1;
    for (char c : s)
        if (c == '.') ++parts;
    return parts >= 3;
}
bool token_needs_checktoken(const std::string& t) { return !token_is_ubi_ticket(t); }
bool token_can_fallback_to_raw_enet(const std::string& t) {
    std::string s = trim(t);
    return s.size() >= 500 && (s.empty() || s.back() != '=');
}
std::string token_shape(const std::string& t) {
    std::string s = trim(t);
    bool padded = !s.empty() && s.back() == '=';
    return "len=" + std::to_string(s.size()) + ", padded=" + (padded ? "true" : "false");
}

bool is_http_403_text(const std::string& msg) {
    return ci_contains(msg, "403") || ci_contains(msg, "forbidden");
}

// Secret-safe classification of an HTTP body: never returns any body content, only
// a coarse shape so a 2xx-non-JSON checktoken response is diagnosable from logs.
std::string classify_body(const std::string& content_type, const std::string& body) {
    std::string b = trim(body);
    if (b.empty()) return "empty";
    char c = b.front();
    if (c == '{' || c == '[') return "json";
    if (c == '<') return "html";
    if (ci_contains(content_type, "json")) return "json?";
    if (ci_contains(content_type, "html")) return "html";
    // gzip magic (0x1f 0x8b) — a compressed body we did not decode.
    if (b.size() >= 2 && static_cast<unsigned char>(b[0]) == 0x1f &&
        static_cast<unsigned char>(b[1]) == 0x8b)
        return "gzip";
    return "other";
}

// Best-effort, content-free label for WHICH kind of HTML page came back, so a WAF
// interstitial is distinguishable from a GT error page (helps diagnose non-JSON).
std::string html_page_hint(const std::string& body) {
    auto has = [&](const char* n) { return ci_contains(body, n); };
    if (has("just a moment") || has("cf-chl") || has("cf_chl") ||
        has("challenge-platform") || has("attention required"))
        return "cloudflare-challenge";
    if (has("captcha")) return "captcha";
    if (has("cloudflare")) return "cloudflare";
    if (has("maintenance")) return "maintenance";
    std::size_t p = body.find("<title");
    if (p == std::string::npos) p = body.find("<TITLE");
    if (p != std::string::npos) {
        std::size_t gt = body.find('>', p);
        std::size_t lt = (gt == std::string::npos) ? std::string::npos : body.find('<', gt + 1);
        if (gt != std::string::npos && lt != std::string::npos && lt > gt + 1)
            return "title=\"" +
                   trim(body.substr(gt + 1, std::min<std::size_t>(lt - (gt + 1), 80))) + "\"";
    }
    return "";
}

// Extract the .text-danger.text-danger-wrapper element text from an HTML error page.
std::string extract_danger_text(const std::string& html) {
    std::size_t p = html.find("text-danger-wrapper");
    if (p == std::string::npos) return "";
    std::size_t gt = html.find('>', p);
    if (gt == std::string::npos) return "";
    std::size_t lt = html.find('<', gt + 1);
    if (lt == std::string::npos) lt = html.size();
    return trim(html.substr(gt + 1, lt - (gt + 1)));
}

// --- fleet pacing gate machinery (§8) ----------------------------------------
struct FleetGate {
    std::mutex mu;
    std::chrono::steady_clock::time_point next_allowed = std::chrono::steady_clock::now();
};

std::chrono::steady_clock::time_point reserve_gate_slot(FleetGate& g,
                                                        std::chrono::milliseconds spacing,
                                                        std::chrono::milliseconds max_ahead) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g.mu);
    auto horizon = now + max_ahead;
    auto slot = g.next_allowed;
    if (slot < now) slot = now;
    if (slot > horizon) slot = horizon;
    auto nxt = slot + spacing;
    if (nxt > horizon) nxt = horizon;
    g.next_allowed = nxt;
    return slot;
}

void wait_global_gate(FleetGate& g, std::chrono::milliseconds spacing,
                      std::chrono::milliseconds max_ahead) {
    auto slot = reserve_gate_slot(g, spacing, max_ahead);
    std::this_thread::sleep_until(slot);  // sleep WITHOUT holding the lock
}

FleetGate& dashboard_gate() {
    static FleetGate g;
    return g;
}
FleetGate& http_login_gate() {
    static FleetGate g;
    return g;
}

constexpr std::chrono::milliseconds kDashboardStagger{3500};
constexpr std::chrono::milliseconds kHttpLoginStagger{2500};
constexpr std::chrono::milliseconds kGateMaxAhead{300000};

// --- server_data candidate fetch (§4.5) --------------------------------------
std::optional<nxrth::net::ServerData> fetch_server_data_candidate(
    const LoginInfo& login_info, const std::string& proxy_label,
    const std::optional<std::string>& proxy_url, const std::string& login_mode,
    const std::string& failure_label, const LogFn& log, bool* out_403 = nullptr) {
    for (bool alternate : {false, true}) {
        log("[Bot] fetching server_data (alternate=" + std::string(alternate ? "true" : "false") +
            ", login_mode=" + login_mode + ", http_proxy=" + proxy_label + ")...");
        auto res = nxrth::net::get_server_data_proxied(alternate, login_info, proxy_url);
        if (res.ok()) return res.data;
        log("[Bot] " + failure_label + ": server_data failed via " + proxy_label + ": " +
            res.error);
        if (is_http_403_text(res.error)) {
            if (out_403) *out_403 = true;
            log("[Bot] " + failure_label +
                ": proxy returned 403; skipping growtopia2 alternate for " + proxy_label);
            break;
        }
    }
    return std::nullopt;
}

// --- proxy candidate selection + bypass session (§4.5) -----------------------
struct ProxyPlan {
    std::vector<std::pair<std::string, std::optional<std::string>>> candidates;
    std::optional<BypassLoginSession> bypass_session;
    bool refused_direct = false;  // no proxy -> we did NOT add a leaking direct candidate
};

ProxyPlan login_attempt_proxies(const std::optional<RotatingLoginProxy>& login_proxy,
                                const std::optional<std::string>& game_proxy_url) {
    ProxyPlan plan;
    if (login_proxy) {
        auto session = login_proxy->login_session();
        if (session) {
            plan.candidates.push_back({"bypass login proxy", session->http_url});
            plan.bypass_session = std::move(session);
        }
        // else: not SOCKS5-UDP capable -> no candidates, caller logs + retries.
        return plan;
    }
    if (game_proxy_url) {
        plan.candidates.push_back({"assigned game proxy", *game_proxy_url});
        return plan;
    }
    // No proxy assigned. A direct login sends the dashboard/token POST through the
    // REAL IP; at scale that trips Growtopia's per-IP 24h login ban (the "leak").
    // Refuse it unless the operator explicitly opts in.
    if (std::getenv("NXRTH_ALLOW_DIRECT_LOGIN"))
        plan.candidates.push_back({"direct", std::nullopt});
    else
        plan.refused_direct = true;
    return plan;
}

// Validate an untrusted "server:port" from server_data (never crash — §11.7).
bool valid_server_addr(const std::string& server, std::uint16_t port) {
    if (server.empty() || port == 0) return false;
    for (char c : server)
        if (std::isspace(static_cast<unsigned char>(c))) return false;
    return true;
}

// Log the IP-pin success line (§7).
void log_pin(const BypassLoginSession& session, const LogFn& log) {
    log("[Bot] bypass logon: token pinned to logon IP " + session.enet.host + ":" +
        std::to_string(session.enet.port) +
        " — ENet logon will use it, world stays on game proxy");
}

// Terminal-error handling (§handle_terminal_login_error): set stop, return none.
void handle_terminal_login_error(const LoginError& error, std::atomic<bool>& stop,
                                 const LogFn& log) {
    if (error.kind == LoginErrorKind::Exhausted) {
        log("[Bot] login attempts exhausted (24h) — stopping this bot cleanly (no crash)");
        stop.store(true);
    } else if (error.kind == LoginErrorKind::WrongCredentials) {
        log("[Bot] wrong credentials — stopping this bot cleanly (no crash)");
        stop.store(true);
    } else {
        log("[Bot] login failed: " + error.message);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
std::string LoginError::display() const {
    switch (kind) {
        case LoginErrorKind::Exhausted:
            return "Login attempts exhausted. Please try again after 24 hours.";
        case LoginErrorKind::WrongCredentials:
            return "Account credentials mismatched.";
        default:
            return message;
    }
}

LoginError classify_login_error(const std::string& text) {
    std::string low = to_lower(text);
    if (low.find("exhausted") != std::string::npos) return {LoginErrorKind::Exhausted, text};
    if (low.find("mismatched") != std::string::npos) return {LoginErrorKind::WrongCredentials, text};
    return {LoginErrorKind::Other, text};
}

std::string token_fingerprint(const std::string& secret) {
    const std::string s = trim(secret);
    if (s.empty()) return "len=0";
    return "len=" + std::to_string(s.size()) +
           " sha=" + nxrth::protocol::sha256_hex(s).substr(0, 12);
}

std::optional<std::string> extract_csrf_token(const std::string& html) {
    std::string low = to_lower(html);
    std::size_t pos = 0;
    while (true) {
        std::size_t in = low.find("<input", pos);
        if (in == std::string::npos) return std::nullopt;
        std::size_t end = html.find('>', in);
        if (end == std::string::npos) return std::nullopt;
        std::string tag = html.substr(in, end - in + 1);
        std::string ltag = to_lower(tag);
        // name="_token"
        if (ltag.find("name=\"_token\"") != std::string::npos ||
            ltag.find("name='_token'") != std::string::npos) {
            std::size_t vp = ltag.find("value");
            if (vp != std::string::npos) {
                std::size_t eq = tag.find('=', vp);
                if (eq != std::string::npos) {
                    std::size_t q = eq + 1;
                    while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
                    if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
                        char quote = tag[q];
                        std::size_t ve = tag.find(quote, q + 1);
                        if (ve != std::string::npos) return tag.substr(q + 1, ve - (q + 1));
                    }
                }
            }
        }
        pos = end + 1;
    }
}

LoginTokenResult get_legacy_token_proxied(const std::string& url, const std::string& username,
                                          const std::string& password,
                                          const std::optional<std::string>& proxy_url) {
    HttpClient client;

    // 1) GET the login page.
    HttpRequest getreq;
    getreq.user_agent = kMacUA;
    getreq.timeout_secs = 20;
    if (proxy_url) getreq.proxy = *proxy_url;
    HttpResponse page = client.Get(url, getreq);
    if (!page.ok()) return {std::nullopt, LoginError{LoginErrorKind::Other, page.error}};

    // 2) CSRF token.
    auto csrf = extract_csrf_token(page.body);
    if (!csrf) {
        return {std::nullopt,
                LoginError{LoginErrorKind::Other, "Failed to extract CSRF token from login page"}};
    }

    // 3) POST growid/login/validate.
    std::string body = "_token=" + form_encode(*csrf) + "&growId=" + form_encode(username) +
                       "&password=" + form_encode(password);
    HttpRequest post;
    post.user_agent = kMacUA;
    post.timeout_secs = 20;
    if (proxy_url) post.proxy = *proxy_url;
    post.headers = {
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"Origin", kLoginOrigin},
        {"Referer", url},
    };
    HttpResponse resp = client.Post(kValidateUrl, body, post);
    if (!resp.ok()) return {std::nullopt, LoginError{LoginErrorKind::Other, resp.error}};
    if (resp.status != 200) {
        return {std::nullopt, LoginError{LoginErrorKind::Other,
                                         "Login failed with status: " + std::to_string(resp.status)}};
    }

    // 4) JSON token, else HTML error page.
    try {
        auto j = nlohmann::json::parse(resp.body);
        if (j.contains("token") && j["token"].is_string())
            return {j["token"].get<std::string>(), std::nullopt};
        return {std::nullopt,
                LoginError{LoginErrorKind::Other, "Missing 'token' field in login response"}};
    } catch (...) {
        std::string danger = extract_danger_text(resp.body);
        if (!danger.empty()) return {std::nullopt, classify_login_error(danger)};
    }
    return {std::nullopt, LoginError{LoginErrorKind::Other,
                                     "Login failed: unexpected response from server"}};
}

LoginTokenResult check_token(const std::string& refresh_token,
                             const std::string& client_data,
                             const std::optional<std::string>& proxy_url) {
    const std::string token = trim(refresh_token);
    if (token.empty()) {
        return {std::nullopt, LoginError{LoginErrorKind::Other, "OAuth refresh token is empty"}};
    }

    HttpClient client;
    HttpRequest request;
    request.user_agent = kUbiUA;
    request.timeout_secs = 20;
    request.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    if (proxy_url) request.proxy = *proxy_url;

    const std::string body = "refreshToken=" + form_encode(token) +
                             "&clientData=" + form_encode(client_data);
    HttpResponse response = client.Post(kCheckTokenUrl, body, request);
    if (!response.ok()) {
        return {std::nullopt, LoginError{LoginErrorKind::Other,
                                         "checktoken transport failed: " + response.error}};
    }
    if (response.status < 200 || response.status >= 300) {
        return {std::nullopt,
                LoginError{LoginErrorKind::Other,
                           "checktoken returned HTTP " + std::to_string(response.status)}};
    }

    const auto parsed = nlohmann::json::parse(response.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        const std::string ctype = response.header("Content-Type").value_or("");
        // GT's login/checktoken failure is an HTML page whose .text-danger-wrapper
        // holds the human-readable reason ("Fail to login...", "...invalid", a
        // rate-limit notice, etc.). Surface it so non-JSON is actually diagnosable.
        const std::string danger = extract_danger_text(response.body);
        const std::string hint = danger.empty() ? html_page_hint(response.body) : std::string();
        return {std::nullopt,
                LoginError{LoginErrorKind::Other,
                           "checktoken returned non-JSON (HTTP " +
                               std::to_string(response.status) + ", content-type=" +
                               (ctype.empty() ? "?" : ctype) + ", body=" +
                               classify_body(ctype, response.body) + ", " +
                               std::to_string(response.body.size()) + " bytes)" +
                               (danger.empty() ? (hint.empty() ? "" : " - page: " + hint)
                                                : " - GT says: \"" + danger + "\"")}};
    }
    if (parsed.value("status", std::string{}) != "success") {
        std::string message;
        for (const char* key : {"message", "error", "status"}) {
            if (parsed.contains(key) && parsed[key].is_string()) {
                message = parsed[key].get<std::string>();
                if (!message.empty()) break;
            }
        }
        if (message.empty()) message = "request rejected";
        return {std::nullopt,
                LoginError{LoginErrorKind::Other, "checktoken failed: " + message}};
    }
    if (!parsed.contains("token") || !parsed["token"].is_string()) {
        return {std::nullopt,
                LoginError{LoginErrorKind::Other,
                           "checktoken succeeded but response token was missing"}};
    }
    std::string checked = trim(parsed["token"].get<std::string>());
    if (checked.empty()) {
        return {std::nullopt,
                LoginError{LoginErrorKind::Other,
                           "checktoken succeeded but response token was empty"}};
    }
    return {std::move(checked), std::nullopt};
}

// --- identity ---------------------------------------------------------------
LoginIdentity default_login_identity() {
    LoginIdentity id;
    id.game_version = std::string(consts::GAME_VER);
    id.cbits = "1536";
    id.player_age = "23";
    id.gdpr = "2";
    id.category = "_16";
    id.total_playtime = "0";
    id.country = "us";
    id.zf = std::string(consts::DEFAULT_ZF);
    id.rid = std::string(consts::DEFAULT_RID);
    id.mac = std::string(consts::DEFAULT_MAC);
    id.wk = std::string(consts::DEFAULT_WK);
    id.hash = std::string(consts::DEFAULT_HASH);
    id.hash2 = std::string(consts::DEFAULT_HASH2);
    id.fz = std::string(consts::DEFAULT_FZ);
    id.platform_id = std::string(consts::DEFAULT_PLATFORM_ID);
    id.steam_token = std::string(consts::DEFAULT_STEAM_TOKEN);
    int hv = 0;
    try {
        hv = std::stoi(std::string(consts::DEFAULT_HASH));
    } catch (...) {
        hv = 0;
    }
    id.klv = nxrth::protocol::compute_klv(consts::GAME_VER, "226", consts::DEFAULT_RID, hv);
    return id;
}

LoginIdentity apply_account_device_identity(const std::string& username, LoginIdentity identity,
                                            const LogFn& log) {
    std::optional<nxrth::account_devices::AccountDevice> device;
    try {
        device = nxrth::account_devices::get_or_create(username);
    } catch (const std::exception& e) {
        log(std::string("[Bot] account device identity unavailable: ") + e.what());
        return identity;
    }
    if (!device) return identity;

    int hash_val = 0;
    bool numeric = true;
    try {
        std::size_t idx = 0;
        hash_val = std::stoi(device->hash, &idx);
        if (idx != device->hash.size()) numeric = false;
    } catch (...) {
        numeric = false;
    }
    if (!numeric) {
        log("[Bot] account device hash was not numeric (" + device->hash +
            "); using default hash");
        try {
            hash_val = std::stoi(std::string(consts::DEFAULT_HASH));
        } catch (...) {
            hash_val = 0;
        }
    }
    std::string klv =
        nxrth::protocol::compute_klv(consts::GAME_VER, "226", device->rid, hash_val);

    identity.rid = device->rid;
    identity.mac = device->mac;
    identity.wk = device->wk;
    identity.hash = device->hash;
    identity.hash2 = device->hash2;
    identity.zf = device->zf;
    identity.klv = klv;

    std::string uname = trim(username);
    std::transform(uname.begin(), uname.end(), uname.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string rid6 = device->rid.substr(0, std::min<std::size_t>(6, device->rid.size()));
    log("[Bot] using account device identity for " + uname + " (rid=" + rid6 + "..., mac=" +
        device->mac + ")");
    return identity;
}

std::optional<LoginIdentity> login_identity(const std::string& username, const LogFn& log) {
    return apply_account_device_identity(username, default_login_identity(), log);
}

// --- pacing gates -----------------------------------------------------------
// Forward to the authoritative process-global gates in bot/gates.h so the WHOLE
// fleet (login HTTP fetches AND bot reconnect re-logins) shares ONE cursor per
// phase. Two separate gate instances would split the pacing and defeat the
// fleet-wide throttle (the local FleetGate machinery above is now unused).
void pace_dashboard() { nxrth::bot::pace_dashboard(); }
void pace_http_login() { nxrth::bot::pace_http_login(); }

// --- legacy orchestration (§4.5 fetch_credentials_with_dashboard, Legacy) ----
// HAR-based fallbacks (try_har_growid_login / checktoken clientData) require the
// har_parser module + requestly_logs.har, which are not part of this module's
// dependencies; they degrade to no-op here (see returned reconciliation notes).
std::optional<Credentials> fetch_credentials(
    const std::string& username, const std::string& password,
    const std::optional<Socks5Config>& proxy,
    const std::optional<RotatingLoginProxy>& login_proxy, std::atomic<bool>& stop,
    const LogFn& log) {
    std::optional<Socks5Config> current_game_proxy = proxy;  // swapped on a 403
    std::optional<std::string> game_proxy_url;
    if (current_game_proxy) game_proxy_url = socks5_config_to_url(*current_game_proxy);

    LoginInfo login_info;
    login_info.protocol = consts::PROTOCOL;
    login_info.game_version = std::string(consts::GAME_VER);

    for (;;) {
        if (stop.load()) {
            log("[Bot] login aborted — bot was stopped");
            return std::nullopt;
        }

        ProxyPlan plan = login_attempt_proxies(login_proxy, game_proxy_url);
        if (plan.refused_direct) {
            log("[Bot] no proxy assigned - refusing DIRECT login to protect the real IP; "
                "assign a game/bypass proxy (or set NXRTH_ALLOW_DIRECT_LOGIN=1). Stopping bot.");
            stop.store(true);
            return std::nullopt;
        }
        if (login_proxy && !plan.bypass_session) {
            log("[Bot] fetch: bypass login proxy is not SOCKS5-UDP capable — cannot pin logon "
                "IP; retrying in 5s");
        }
        std::optional<Socks5Config> bypass_enet;
        if (plan.bypass_session) bypass_enet = plan.bypass_session->enet;

        bool rotate_bypass = false;  // a 403 from the bypass proxy -> re-pick now
        bool rotate_game = false;    // a 403 from the game proxy -> quarantine + swap
        auto react_403 = [&]() -> bool {
            if (login_proxy) { rotate_bypass = true; return true; }
            if (current_game_proxy) { rotate_game = true; return true; }
            return false;
        };
        for (const auto& [proxy_label, proxy_url] : plan.candidates) {
            if (stop.load()) return std::nullopt;

            bool got_403 = false;
            auto server_data = fetch_server_data_candidate(login_info, proxy_label, proxy_url,
                                                           "legacy", "fetch", log, &got_403);
            if (!server_data) {
                if (got_403 && react_403()) break;
                continue;
            }
            if (!valid_server_addr(server_data->server, server_data->port)) {
                log("[Bot] fetch: invalid server address '" + server_data->server + ":" +
                    std::to_string(server_data->port) + "' via " + proxy_label +
                    " (parse failed) — skipping candidate");
                continue;
            }

            pace_dashboard();
            auto dashboard = get_dashboard_proxied(server_data->loginurl, login_info,
                                                   server_data->meta, proxy_url);
            std::optional<std::string> growtopia_url;
            if (!dashboard.ok()) {
                if (is_http_403_text(dashboard.error) && react_403()) break;
                log("[Bot] fetch: dashboard failed via " + proxy_label + ": " + dashboard.error);
            } else {
                growtopia_url = dashboard.links.growtopia;
            }

            if (!growtopia_url) {
                log("[Bot] fetch: no usable GrowID token via " + proxy_label);
                continue;
            }

            auto tok = get_legacy_token_proxied(*growtopia_url, username, password, proxy_url);
            if (!tok.ok()) {
                const LoginError& e = *tok.error;
                log("[Bot] fetch: login failed via " + proxy_label + ": " + e.display());
                if (e.kind == LoginErrorKind::Exhausted ||
                    e.kind == LoginErrorKind::WrongCredentials) {
                    handle_terminal_login_error(e, stop, log);
                }
                if (is_http_403_text(e.display()) && react_403()) break;
                continue;
            }

            // prepare_login_token: UbiTicket-shaped tokens pass through; otherwise
            // fall back to the raw dashboard token when long enough for ENet.
            std::string raw = *tok.token;
            std::string ltoken;
            if (!token_needs_checktoken(raw)) {
                ltoken = raw;
            } else if (token_can_fallback_to_raw_enet(raw)) {
                log("[Bot] checktoken did not yield UbiTicket; falling back to raw dashboard token");
                ltoken = raw;
            } else {
                log("[Bot] dashboard token looks unusable (" + token_shape(raw) +
                    "); trying fallback login");
                continue;
            }

            log("[Bot] using dashboard login token directly via " + proxy_label +
                " (checktoken skipped for ENet login)");
            log("[Bot] Got token: " + token_fingerprint(ltoken));
            if (plan.bypass_session) log_pin(*plan.bypass_session, log);

            Credentials cred;
            cred.ltoken = std::move(ltoken);
            cred.meta = server_data->meta;
            cred.server = server_data->server;
            cred.port = server_data->port;
            cred.identity = login_identity(username, log);  // per-account ENet identity
            cred.bypass_enet = bypass_enet;
            return cred;
        }

        if (rotate_bypass) {
            log("[Bot] bypass proxy hit 403 — rotating to the next bypass proxy now");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        if (rotate_game && current_game_proxy) {
            if (!swap_quarantined_game_proxy(current_game_proxy, game_proxy_url, stop, log))
                return std::nullopt;  // no clean replacement -> bot stopped (never direct)
            continue;
        }
        log("[Bot] fetch: all HTTP login proxy candidates failed - retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

}  // namespace nxrth::login
