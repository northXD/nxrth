// Nxrth — proxy configuration model implementation.
// Ported from Nxrth/proxy_pool.rs (docs/port-specs/04-proxy.md §3-§5).
#include "proxy/proxy_pool.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace nxrth::proxy {
namespace {

// --- small string helpers ----------------------------------------------------
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Parse a u16; throws std::runtime_error(err_msg) on any failure (empty, non
// digits, out of range). Matches the Rust parse::<u16>() reject set.
std::uint16_t parse_u16(const std::string& s, const std::string& err_msg) {
    const std::string t = trim(s);
    if (t.empty()) throw std::runtime_error(err_msg);
    for (char c : t)
        if (!std::isdigit(static_cast<unsigned char>(c))) throw std::runtime_error(err_msg);
    unsigned long v = 0;
    try {
        v = std::stoul(t);
    } catch (...) {
        throw std::runtime_error(err_msg);
    }
    if (v > 0xFFFFul) throw std::runtime_error(err_msg);
    return static_cast<std::uint16_t>(v);
}

// Percent-encode userinfo (encode everything outside RFC 3986 unreserved).
std::string percent_encode_userinfo(const std::string& s) {
    static const char* HX = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        bool unreserved = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(HX[c >> 4]);
            out.push_back(HX[c & 0x0F]);
        }
    }
    return out;
}

// Resolve host:port to the first result's numeric IP. Returns the IP literal
// string, or std::nullopt if unresolvable. IP-literal fast path preserves the
// "never drop a hostname endpoint" invariant (getaddrinfo handles both).
std::optional<std::string> resolve_ip(const std::string& host, std::uint16_t port) {
    // Fast path: already a numeric IPv4/IPv6 literal.
    {
        sockaddr_in a4{};
        if (inet_pton(AF_INET, host.c_str(), &a4.sin_addr) == 1) return host;
        sockaddr_in6 a6{};
        if (inet_pton(AF_INET6, host.c_str(), &a6.sin6_addr) == 1) return host;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    const std::string port_s = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) return std::nullopt;
    char buf[INET6_ADDRSTRLEN] = {0};
    std::optional<std::string> out;
    if (res->ai_family == AF_INET) {
        auto* sa = reinterpret_cast<sockaddr_in*>(res->ai_addr);
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) out = std::string(buf);
    } else if (res->ai_family == AF_INET6) {
        auto* sa = reinterpret_cast<sockaddr_in6*>(res->ai_addr);
        if (inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof(buf))) out = std::string(buf);
    }
    freeaddrinfo(res);
    return out;
}

// Uniform port in [base_port, base_port+span-1]. span<=1 -> base_port.
std::uint16_t random_rotating_port(std::uint16_t base_port, std::uint16_t span) {
    const std::uint32_t max_span = (0xFFFFu - static_cast<std::uint32_t>(base_port)) + 1u;
    std::uint32_t eff = std::max<std::uint32_t>(span, 1u);
    eff = std::min(eff, max_span);
    if (eff == 1u) return base_port;
    static thread_local std::mt19937 eng([] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd()};
        return std::mt19937(seq);
    }());
    const std::uint32_t r = eng();
    return static_cast<std::uint16_t>(base_port + (r % eff));
}

std::size_t random_index(std::size_t count) {
    static thread_local std::mt19937 eng([] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd()};
        return std::mt19937(seq);
    }());
    return std::uniform_int_distribution<std::size_t>(0, count - 1)(eng);
}

// ============================ process-global state ===========================
std::atomic<std::size_t> LOGIN_PROXY_RR{0};
std::atomic<std::size_t> GAME_PROXY_RR{0};

std::mutex& game_pool_mutex() {
    static std::mutex m;
    return m;
}
struct PublishedGamePool {
    std::vector<Socks5Config> proxies;
    bool shuffle_selection = false;
};

// std::nullopt sentinel = "never published"; empty vector = published-but-empty.
std::optional<PublishedGamePool>& game_pool_store() {
    static std::optional<PublishedGamePool> store;
    return store;
}

// 403 quarantine: resolved "ip:port" of game proxies that returned HTTP 403.
std::mutex& quarantine_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_set<std::string>& quarantine_store() {
    static std::unordered_set<std::string> s;
    return s;
}

bool same_endpoint(const Socks5Config& a, const Socks5Config& b) {
    return a.host == b.host && a.port == b.port;
}

// ============================ JSON (de)serialization =========================
void to_json_entry(json& j, const ProxyEntry& e) {
    j = json::object();
    j["host"] = e.host;
    j["port"] = e.port;
    j["username"] = e.username ? json(*e.username) : json(nullptr);
    j["password"] = e.password ? json(*e.password) : json(nullptr);
    j["scheme"] = e.scheme ? json(*e.scheme) : json(nullptr);
    j["raw"] = e.raw;
}

std::optional<std::string> opt_str(const json& j, const char* key) {
    if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
    return j.at(key).get<std::string>();
}

ProxyEntry from_json_entry(const json& j) {
    ProxyEntry e;
    e.host = j.at("host").get<std::string>();
    e.port = j.at("port").get<std::uint16_t>();
    e.username = opt_str(j, "username");
    e.password = opt_str(j, "password");
    e.scheme = opt_str(j, "scheme");
    e.raw = j.value("raw", std::string());
    return e;
}

json to_json_config(const ProxyPoolConfig& c) {
    json j = json::object();
    j["enabled"] = c.enabled;
    j["max_bots_per_ip"] = c.max_bots_per_ip;
    j["spread_mode"] = spread_mode_as_str(c.spread_mode);
    j["shuffle_selection"] = c.shuffle_selection;
    json arr = json::array();
    for (const auto& e : c.proxies) {
        json je;
        to_json_entry(je, e);
        arr.push_back(std::move(je));
    }
    j["proxies"] = std::move(arr);
    j["next_index"] = c.next_index;
    j["rotating_login_enabled"] = c.rotating_login_enabled;
    j["rotating_login_scheme"] = c.rotating_login_scheme;
    j["rotating_login_port_span"] = c.rotating_login_port_span;
    if (c.rotating_login_proxy) {
        json je;
        to_json_entry(je, *c.rotating_login_proxy);
        j["rotating_login_proxy"] = std::move(je);
    } else {
        j["rotating_login_proxy"] = nullptr;
    }
    json rarr = json::array();
    for (const auto& e : c.rotating_login_proxies) {
        json je;
        to_json_entry(je, e);
        rarr.push_back(std::move(je));
    }
    j["rotating_login_proxies"] = std::move(rarr);
    return j;
}

// Lenient parse; throws on any structural failure so load_default can fall back
// to the full default config (mirrors serde's whole-file default-on-error).
ProxyPoolConfig from_json_config(const json& j) {
    ProxyPoolConfig c;  // holds Default values
    c.enabled = j.at("enabled").get<bool>();
    c.max_bots_per_ip = j.at("max_bots_per_ip").get<std::size_t>();
    c.spread_mode = spread_mode_from_str(j.at("spread_mode").get<std::string>());
    c.shuffle_selection = j.value("shuffle_selection", false);
    c.proxies.clear();
    for (const auto& je : j.at("proxies")) c.proxies.push_back(from_json_entry(je));
    c.next_index = j.at("next_index").get<std::size_t>();
    // #[serde(default)] fields — default-on-missing.
    c.rotating_login_enabled = j.value("rotating_login_enabled", false);
    c.rotating_login_scheme = j.value("rotating_login_scheme", std::string("auto"));
    c.rotating_login_port_span =
        j.value("rotating_login_port_span", static_cast<std::uint16_t>(2000));
    if (j.contains("rotating_login_proxy") && !j.at("rotating_login_proxy").is_null())
        c.rotating_login_proxy = from_json_entry(j.at("rotating_login_proxy"));
    if (j.contains("rotating_login_proxies") && j.at("rotating_login_proxies").is_array())
        for (const auto& je : j.at("rotating_login_proxies"))
            c.rotating_login_proxies.push_back(from_json_entry(je));
    return c;
}

// ============================ scheme helpers (fwd) ===========================
// (definitions in the public section below; used by parse/pick before them)

}  // namespace

// ============================ ProxySpreadMode ================================
ProxySpreadMode spread_mode_from_str(const std::string& s) {
    return trim(s) == "round_robin" ? ProxySpreadMode::RoundRobin : ProxySpreadMode::LeastLoaded;
}
const char* spread_mode_as_str(ProxySpreadMode m) {
    return m == ProxySpreadMode::RoundRobin ? "round_robin" : "least_loaded";
}

// ============================ scheme helpers ================================
const char* normalize_proxy_scheme(const std::string& value) {
    const std::string v = to_lower(trim(value));
    if (v == "auto") return "auto";
    if (v == "socks5") return "socks5";
    if (v == "socks5h") return "socks5h";
    return "http";  // default/fallback
}

const char* infer_proxy_scheme_from_port(std::uint16_t port) {
    if (port == 823) return "http";  // DataImpulse plain HTTP gateway
    return "socks5h";                // incl. 824 SOCKS5 + 10000-20000 sticky range
}

const char* effective_proxy_scheme(const std::string& configured_scheme,
                                   const ProxyEntry& proxy) {
    if (proxy.scheme) {
        const char* per = normalize_proxy_scheme(*proxy.scheme);
        if (std::strcmp(per, "auto") != 0) return per;  // entry override wins
    }
    const char* cfg = normalize_proxy_scheme(configured_scheme);
    if (std::strcmp(cfg, "auto") == 0) return infer_proxy_scheme_from_port(proxy.port);
    return cfg;
}

std::uint16_t normalized_rotating_port_span(std::uint16_t base_port, std::uint16_t span) {
    if (base_port >= 10000 && base_port <= 20000) {
        const std::uint16_t cap = static_cast<std::uint16_t>(20000 - base_port + 1);
        const std::uint16_t s = std::max<std::uint16_t>(span, 1);
        return std::min(s, cap);
    }
    return 1;  // non-sticky port is a SINGLE endpoint; never randomize it
}

// ============================ ProxyEntry ====================================
Socks5Config ProxyEntry::to_socks5() const {
    auto ip = resolve_ip(host, port);
    if (!ip)
        throw std::runtime_error("could not resolve proxy " + host + ":" + std::to_string(port));
    Socks5Config cfg;
    cfg.host = *ip;  // resolved numeric IP
    cfg.port = port;
    cfg.username = username;
    cfg.password = password;
    return cfg;
}

std::string ProxyEntry::capacity_key() const {
    Socks5Config cfg = to_socks5();
    return cfg.host + ":" + std::to_string(cfg.port);
}

std::string ProxyEntry::label() const {
    std::string base = host + ":" + std::to_string(port);
    if (username && !username->empty()) return base + " (" + *username + ")";
    return base;
}

std::string ProxyEntry::to_proxy_url(const std::string& default_scheme) const {
    const char* scheme = effective_proxy_scheme(default_scheme, *this);
    std::string url = std::string(scheme) + "://";
    if (username) {
        url += percent_encode_userinfo(*username);
        if (password) {
            url += ":";
            url += percent_encode_userinfo(*password);
        }
        url += "@";
    }
    url += host + ":" + std::to_string(port);
    return url;
}

namespace {

// ============================ line parsing ==================================
// Single entry, no ranges. Throws std::runtime_error(verbatim) on failure.
ProxyEntry parse_proxy_line(const std::string& raw) {
    const bool has_url = raw.find("://") != std::string::npos;
    const bool has_at = raw.find('@') != std::string::npos;

    if (has_url || has_at) {
        // URL / user:pass@host:port form.
        const bool had_scheme = has_url;
        std::string url_text = had_scheme ? raw : ("socks5://" + raw);

        const std::size_t sep = url_text.find("://");
        std::string scheme_s = url_text.substr(0, sep);
        std::string rest = url_text.substr(sep + 3);
        // Strip any path/query (proxy lines carry none, but be safe).
        std::size_t slash = rest.find_first_of("/?");
        if (slash != std::string::npos) rest = rest.substr(0, slash);

        std::optional<std::string> user, pass;
        std::string hostport = rest;
        std::size_t at = rest.find('@');  // userinfo contains no '@'
        if (at != std::string::npos) {
            std::string userinfo = rest.substr(0, at);
            hostport = rest.substr(at + 1);
            std::size_t colon = userinfo.find(':');
            if (colon == std::string::npos) {
                if (!userinfo.empty()) user = userinfo;
            } else {
                std::string u = userinfo.substr(0, colon);
                std::string p = userinfo.substr(colon + 1);
                if (!u.empty()) user = u;
                if (!p.empty()) pass = p;
            }
        }
        std::size_t hc = hostport.rfind(':');
        if (hc == std::string::npos) throw std::runtime_error("missing proxy port");
        std::string host = hostport.substr(0, hc);
        std::string port_s = hostport.substr(hc + 1);
        if (host.empty()) throw std::runtime_error("missing proxy host");
        if (port_s.empty()) throw std::runtime_error("missing proxy port");
        std::uint16_t port = parse_u16(port_s, "missing proxy port");

        ProxyEntry e;
        e.host = host;
        e.port = port;
        e.username = user;
        e.password = pass;
        if (had_scheme) e.scheme = std::string(normalize_proxy_scheme(scheme_s));
        e.raw = raw;
        return e;
    }

    // Colon form host:port[:user[:pass]].
    std::vector<std::string> parts = split(raw, ':');
    if (parts.size() < 2) throw std::runtime_error("expected host:port or host:port:user:pass");
    std::string host = trim(parts[0]);
    std::uint16_t port = parse_u16(parts[1], "expected host:port or host:port:user:pass");
    if (host.empty()) throw std::runtime_error("missing proxy host");

    ProxyEntry e;
    e.host = host;
    e.port = port;
    if (parts.size() > 2) {
        std::string u = trim(parts[2]);
        if (!u.empty()) e.username = u;
    }
    if (parts.size() > 3) {
        std::string p;
        for (std::size_t i = 3; i < parts.size(); ++i) {
            if (i > 3) p += ":";
            p += parts[i];
        }
        p = trim(p);
        if (!p.empty()) e.password = p;
    }
    e.raw = raw;
    return e;
}

// DataImpulse host:START-END:user:pass sticky-range expansion (else 1 entry).
std::vector<ProxyEntry> expand_proxy_line(const std::string& raw) {
    const bool has_url = raw.find("://") != std::string::npos;
    const bool has_at = raw.find('@') != std::string::npos;
    if (!has_url && !has_at) {
        std::vector<std::string> parts = split(raw, ':');
        if (parts.size() >= 2 && parts[1].find('-') != std::string::npos) {
            std::size_t dash = parts[1].find('-');
            std::string start_s = parts[1].substr(0, dash);
            std::string end_s = parts[1].substr(dash + 1);
            std::uint16_t start =
                parse_u16(start_s, "invalid range start port '" + start_s + "'");
            std::uint16_t end = parse_u16(end_s, "invalid range end port '" + end_s + "'");
            if (end < start)
                throw std::runtime_error("port range end " + std::to_string(end) +
                                         " is before start " + std::to_string(start));
            std::string host = trim(parts[0]);
            if (host.empty()) throw std::runtime_error("missing proxy host");
            std::optional<std::string> username, password;
            if (parts.size() > 2) {
                std::string u = trim(parts[2]);
                if (!u.empty()) username = u;
            }
            if (parts.size() > 3) {
                std::string p;
                for (std::size_t i = 3; i < parts.size(); ++i) {
                    if (i > 3) p += ":";
                    p += parts[i];
                }
                p = trim(p);
                if (!p.empty()) password = p;
            }
            std::uint32_t count = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(end) - static_cast<std::uint32_t>(start) + 1u, 2000u);
            std::vector<ProxyEntry> out;
            out.reserve(count);
            for (std::uint32_t offset = 0; offset < count; ++offset) {
                std::uint16_t port = static_cast<std::uint16_t>(start + offset);
                std::string raw_entry = host + ":" + std::to_string(port);
                if (username) raw_entry += ":" + *username;
                if (password) raw_entry += ":" + *password;
                ProxyEntry e;
                e.host = host;
                e.port = port;
                e.username = username;
                e.password = password;
                e.raw = raw_entry;
                out.push_back(std::move(e));
            }
            return out;
        }
    }
    return {parse_proxy_line(raw)};
}

// ============================ global publish ================================
void publish_game_pool(const ProxyPoolConfig& config) {
    std::vector<Socks5Config> resolved;
    resolved.reserve(config.proxies.size());
    for (const auto& e : config.proxies) {
        try {
            resolved.push_back(e.to_socks5());
        } catch (...) {
            // drop entries that fail to resolve
        }
    }
    std::lock_guard<std::mutex> lk(game_pool_mutex());
    game_pool_store() = PublishedGamePool{std::move(resolved), config.shuffle_selection};
}

}  // namespace

// ============================ free parse API ================================
std::vector<ProxyEntry> parse_proxy_lines(const std::string& input) {
    std::vector<ProxyEntry> entries;
    std::vector<std::string> errors;
    std::vector<std::string> lines = split(input, '\n');
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string raw = trim(lines[i]);
        if (raw.empty() || raw[0] == '#') continue;
        try {
            for (auto& e : expand_proxy_line(raw)) entries.push_back(std::move(e));
        } catch (const std::exception& ex) {
            errors.push_back("line " + std::to_string(i + 1) + ": " + ex.what());
        }
    }
    if (!errors.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < errors.size(); ++i) {
            if (i) joined += "; ";
            joined += errors[i];
        }
        throw std::runtime_error(joined);
    }
    return entries;
}

std::optional<ProxyEntry> parse_optional_proxy(const std::string& input) {
    std::string t = trim(input);
    if (t.empty()) return std::nullopt;
    return parse_proxy_line(t);
}

// ============================ next_game_proxy ==============================
std::optional<Socks5Config> next_game_proxy(const Socks5Config* current) {
    std::lock_guard<std::mutex> lk(game_pool_mutex());
    auto& store = game_pool_store();
    if (!store) return std::nullopt;  // never published
    const auto& pool = store->proxies;
    const std::size_t len = pool.size();
    if (len < 2) return std::nullopt;

    if (store->shuffle_selection) {
        std::vector<std::size_t> candidates;
        candidates.reserve(len);
        for (std::size_t k = 0; k < len; ++k) {
            const std::string key = pool[k].host + ":" + std::to_string(pool[k].port);
            if (is_proxy_quarantined(key)) continue;
            if (!current || !same_endpoint(pool[k], *current)) candidates.push_back(k);
        }
        if (candidates.empty()) return std::nullopt;
        return pool[candidates[random_index(candidates.size())]];
    }

    // Compare by resolved ip:port only; advance the shared cursor even on
    // skipped (same-as-current / quarantined) slots to keep global spread.
    for (std::size_t i = 0; i < len; ++i) {
        std::size_t k = GAME_PROXY_RR.fetch_add(1, std::memory_order_relaxed) % len;
        const std::string key = pool[k].host + ":" + std::to_string(pool[k].port);
        if (is_proxy_quarantined(key)) continue;  // 403'd exit IP - never reuse
        if (!current || !same_endpoint(pool[k], *current)) return pool[k];
    }
    return std::nullopt;
}

// ============================ 403 quarantine ===============================
void quarantine_proxy(const std::string& ip_port) {
    if (ip_port.empty()) return;
    std::lock_guard<std::mutex> lk(quarantine_mutex());
    quarantine_store().insert(ip_port);
}
bool is_proxy_quarantined(const std::string& ip_port) {
    std::lock_guard<std::mutex> lk(quarantine_mutex());
    return quarantine_store().count(ip_port) > 0;
}
std::vector<std::string> quarantined_proxies() {
    std::lock_guard<std::mutex> lk(quarantine_mutex());
    return {quarantine_store().begin(), quarantine_store().end()};
}
void clear_quarantine() {
    std::lock_guard<std::mutex> lk(quarantine_mutex());
    quarantine_store().clear();
}

// --- active SOCKS5 reachability probe ----------------------------------------
bool probe_socks5(const Socks5Config& cfg) {
    if (cfg.host.empty() || cfg.port == 0) return false;
    try {
        // bind_through_proxy does the whole handshake (method negotiation,
        // RFC-1929 auth, UDP ASSOCIATE) and throws on any failure. A returned,
        // valid socket means the proxy is usable; its destructor tears it down.
        nxrth::net::Socks5UdpSocket sock =
            nxrth::net::Socks5UdpSocket::bind_through_proxy(cfg);
        return sock.valid();
    } catch (...) {
        return false;
    }
}

// ============================ RotatingLoginProxy ===========================
ProxyEntry RotatingLoginProxy::pick() const {
    const std::size_t len = pool_.size();
    std::size_t idx = 0;
    if (len > 1) {
        idx = shuffle_selection_
                  ? random_index(len)
                  : LOGIN_PROXY_RR.fetch_add(1, std::memory_order_relaxed) % len;
    }
    ProxyEntry proxy = pool_[idx];
    const std::uint16_t span = (len > 1) ? 1 : span_;  // multi-entry pools force span=1
    proxy.port = random_rotating_port(proxy.port, span);
    return proxy;
}

std::string RotatingLoginProxy::fresh_url() const {
    ProxyEntry proxy = pick();
    const char* scheme = normalize_proxy_scheme(configured_scheme_);
    try {
        return proxy.to_proxy_url(scheme);
    } catch (...) {
        const char* eff = effective_proxy_scheme(scheme, proxy);
        std::string url = std::string(eff) + "://";
        if (proxy.username && proxy.password)
            url += *proxy.username + ":" + *proxy.password + "@";
        url += proxy.host + ":" + std::to_string(proxy.port);
        return url;
    }
}

std::optional<BypassLoginSession> RotatingLoginProxy::login_session() const {
    // Try several picks so we can SKIP any bypass exit IP that's been quarantined
    // — HTTP 403 (login-blocked) OR, via the game-connect watchdog, rate-limited by
    // GT's game servers (a shared-IP condition: the IP can bypass-logon but can't
    // actually play). pick() advances the round-robin each call, so on a multi-entry
    // pool this lands on a fresh, non-quarantined exit. If EVERY candidate is
    // quarantined we fall back to one anyway (better to keep trying than to strand
    // the bot; the quarantine is best-effort and session-only).
    const std::size_t tries = pool_.empty() ? std::size_t{1} : pool_.size() * 2;
    std::optional<BypassLoginSession> fallback;
    for (std::size_t a = 0; a < tries; ++a) {
        ProxyEntry proxy = pick();  // single pick -> single exit IP for both halves
        const char* scheme = effective_proxy_scheme(configured_scheme_, proxy);
        if (std::strcmp(scheme, "socks5") != 0 && std::strcmp(scheme, "socks5h") != 0)
            continue;  // ENet/UDP requires SOCKS5 UDP ASSOCIATE
        std::string http_url;
        try {
            http_url = proxy.to_proxy_url(scheme);
        } catch (...) {
            continue;
        }
        auto ip = resolve_ip(proxy.host, proxy.port);
        if (!ip) continue;
        BypassLoginSession sess;
        sess.http_url = std::move(http_url);
        sess.enet.host = *ip;  // resolved numeric IP
        sess.enet.port = proxy.port;
        sess.enet.username = proxy.username;
        sess.enet.password = proxy.password;
        if (is_proxy_quarantined(*ip + ":" + std::to_string(proxy.port))) {
            if (!fallback) fallback = sess;  // remember, in case every exit is quarantined
            continue;
        }
        return sess;
    }
    return fallback;  // std::nullopt if nothing resolved / not SOCKS5-UDP capable
}

// ============================ ProxyPool ====================================
ProxyPool ProxyPool::load_default() {
    ProxyPool pool;
    fs::path cwd;
    try {
        cwd = fs::current_path();
    } catch (...) {
        cwd = ".";
    }
    pool.path_ = cwd / "data" / "proxy_pool.json";

    ProxyPoolConfig config;  // Default
    try {
        std::ifstream in(pool.path_, std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            json j = json::parse(ss.str());
            config = from_json_config(j);
        }
    } catch (...) {
        config = ProxyPoolConfig{};  // any failure -> full default
    }

    // Legacy migration: move singular bypass into the vec.
    if (config.rotating_login_proxies.empty() && config.rotating_login_proxy) {
        config.rotating_login_proxies.push_back(*config.rotating_login_proxy);
    }

    pool.config_ = std::move(config);
    publish_game_pool(pool.config_);
    return pool;
}

ProxyPoolView ProxyPool::view(const ActiveCounts& active_counts) const {
    ProxyPoolView v;
    const std::size_t capacity = std::max<std::size_t>(config_.max_bots_per_ip, 1);

    std::unordered_map<std::string, std::size_t> distinct;  // ip -> active (dedupe)
    for (std::size_t i = 0; i < config_.proxies.size(); ++i) {
        std::string ip;
        try {
            ip = config_.proxies[i].capacity_key();
        } catch (...) {
            continue;  // skip entries that fail to resolve
        }
        std::size_t active = 0;
        auto it = active_counts.find(ip);
        if (it != active_counts.end()) active = it->second;
        ProxyPoolEntryView ev;
        ev.index = i;
        ev.label = config_.proxies[i].label();
        ev.ip = ip;
        ev.active = active;
        ev.capacity = capacity;
        ev.full = active >= capacity;
        if (!ev.full) ++v.available;
        distinct[ip] = active;
        v.proxies.push_back(std::move(ev));
    }
    for (const auto& kv : distinct) v.active += kv.second;

    v.enabled = config_.enabled;
    v.max_bots_per_ip = config_.max_bots_per_ip;
    v.spread_mode = spread_mode_as_str(config_.spread_mode);
    v.shuffle_selection = config_.shuffle_selection;
    v.total = config_.proxies.size();

    // proxies_text = join game entries' raw by "\n".
    for (std::size_t i = 0; i < config_.proxies.size(); ++i) {
        if (i) v.proxies_text += "\n";
        v.proxies_text += config_.proxies[i].raw;
    }

    v.rotating_login_enabled = config_.rotating_login_enabled;
    v.rotating_login_scheme = normalize_proxy_scheme(config_.rotating_login_scheme);
    if (!config_.rotating_login_proxies.empty()) {
        v.rotating_login_effective_scheme =
            effective_proxy_scheme(config_.rotating_login_scheme, config_.rotating_login_proxies[0]);
    } else {
        v.rotating_login_effective_scheme = normalize_proxy_scheme(config_.rotating_login_scheme);
    }
    if (config_.rotating_login_proxies.size() == 1) {
        v.rotating_login_port_span = normalized_rotating_port_span(
            config_.rotating_login_proxies[0].port, config_.rotating_login_port_span);
    } else {
        v.rotating_login_port_span = 1;
    }
    for (std::size_t i = 0; i < config_.rotating_login_proxies.size(); ++i) {
        if (i) v.rotating_login_proxy_text += "\n";
        v.rotating_login_proxy_text += config_.rotating_login_proxies[i].raw;
    }
    const std::size_t n = config_.rotating_login_proxies.size();
    if (n == 0) {
        v.rotating_login_proxy_label = std::nullopt;
    } else if (n == 1) {
        v.rotating_login_proxy_label = rotating_login_label(config_.rotating_login_proxies[0]);
    } else {
        v.rotating_login_proxy_label =
            std::to_string(n) + " bypass proxies (random per login)";
    }
    return v;
}

void ProxyPool::update(bool enabled, std::size_t max_bots_per_ip,
                       const std::string& spread_mode, bool shuffle_selection,
                       const std::string& proxies_text,
                       bool rotating_login_enabled, const std::string& rotating_login_scheme,
                       std::uint16_t rotating_login_port_span,
                       const std::string& rotating_login_proxy_text) {
    std::vector<ProxyEntry> proxies = parse_proxy_lines(proxies_text);  // fails whole update
    config_.enabled = enabled;
    config_.max_bots_per_ip = std::max<std::size_t>(max_bots_per_ip, 1);
    config_.spread_mode = spread_mode_from_str(spread_mode);
    config_.shuffle_selection = shuffle_selection;
    config_.proxies = std::move(proxies);
    publish_game_pool(config_);
    config_.next_index = std::min(config_.next_index, config_.proxies.size());
    config_.rotating_login_enabled = rotating_login_enabled;
    config_.rotating_login_scheme = normalize_proxy_scheme(rotating_login_scheme);
    config_.rotating_login_proxies = parse_proxy_lines(rotating_login_proxy_text);
    config_.rotating_login_proxy = std::nullopt;  // clear legacy
    if (config_.rotating_login_proxies.size() == 1) {
        config_.rotating_login_port_span = normalized_rotating_port_span(
            config_.rotating_login_proxies[0].port, rotating_login_port_span);
    } else {
        config_.rotating_login_port_span = std::max<std::uint16_t>(rotating_login_port_span, 1);
    }
    save();
}

std::optional<Socks5Config> ProxyPool::choose(const ActiveCounts& active_counts) {
    if (!config_.enabled) return std::nullopt;
    if (config_.proxies.empty())
        throw std::runtime_error("proxy pool is enabled but empty");
    const std::size_t capacity = std::max<std::size_t>(config_.max_bots_per_ip, 1);

    // candidates: (index, active) for entries that resolve AND active < capacity.
    struct Cand {
        std::size_t index;
        std::size_t active;
    };
    std::vector<Cand> candidates;
    for (std::size_t i = 0; i < config_.proxies.size(); ++i) {
        std::string ip;
        try {
            ip = config_.proxies[i].capacity_key();
        } catch (...) {
            continue;
        }
        if (is_proxy_quarantined(ip)) continue;  // 403'd proxy - never hand it out
        std::size_t active = 0;
        auto it = active_counts.find(ip);
        if (it != active_counts.end()) active = it->second;
        if (active < capacity) candidates.push_back({i, active});
    }
    if (candidates.empty())
        throw std::runtime_error("no proxy is available under the current bots-per-proxy limit");

    const std::size_t len = config_.proxies.size();
    const std::size_t start = std::min(config_.next_index, len - 1);

    auto candidate_at = [&](std::size_t index) -> const Cand* {
        for (const auto& c : candidates)
            if (c.index == index) return &c;
        return nullptr;
    };

    std::size_t picked = 0;
    bool found = false;
    if (config_.shuffle_selection) {
        std::vector<std::size_t> eligible;
        eligible.reserve(candidates.size());
        if (config_.spread_mode == ProxySpreadMode::LeastLoaded) {
            std::size_t min_active = candidates[0].active;
            for (const auto& c : candidates) min_active = std::min(min_active, c.active);
            for (const auto& c : candidates)
                if (c.active == min_active) eligible.push_back(c.index);
        } else {
            for (const auto& c : candidates) eligible.push_back(c.index);
        }
        picked = eligible[random_index(eligible.size())];
        found = true;
    } else if (config_.spread_mode == ProxySpreadMode::LeastLoaded) {
        std::size_t min_active = candidates[0].active;
        for (const auto& c : candidates) min_active = std::min(min_active, c.active);
        for (std::size_t offset = 0; offset < len; ++offset) {
            std::size_t cand = (start + offset) % len;
            const Cand* c = candidate_at(cand);
            if (c && c->active == min_active) {
                picked = cand;
                found = true;
                break;
            }
        }
    } else {  // RoundRobin
        for (std::size_t offset = 0; offset < len; ++offset) {
            std::size_t cand = (start + offset) % len;
            if (candidate_at(cand)) {
                picked = cand;
                found = true;
                break;
            }
        }
    }
    // A candidate is guaranteed to exist.
    (void)found;
    config_.next_index = (picked + 1) % len;
    save();
    return config_.proxies[picked].to_socks5();
}

std::optional<RotatingLoginProxy> ProxyPool::rotating_login_proxy() const {
    if (!config_.rotating_login_enabled) return std::nullopt;
    if (config_.rotating_login_proxies.empty())
        throw std::runtime_error("rotating login proxy is enabled but empty");
    std::uint16_t span;
    if (config_.rotating_login_proxies.size() == 1) {
        span = normalized_rotating_port_span(config_.rotating_login_proxies[0].port,
                                             config_.rotating_login_port_span);
    } else {
        span = 1;
    }
    return RotatingLoginProxy(config_.rotating_login_proxies, span,
                              config_.rotating_login_scheme, config_.shuffle_selection);
}

std::string ProxyPool::rotating_login_label(const ProxyEntry& proxy) const {
    const std::uint16_t span = std::max<std::uint16_t>(config_.rotating_login_port_span, 1);
    if (span == 1) return proxy.label();
    std::uint32_t end32 = static_cast<std::uint32_t>(proxy.port) + (span - 1u);
    std::uint16_t end = static_cast<std::uint16_t>(std::min<std::uint32_t>(end32, 0xFFFFu));
    std::string base =
        proxy.host + ":" + std::to_string(proxy.port) + "-" + std::to_string(end);
    if (proxy.username && !proxy.username->empty()) return base + " (" + *proxy.username + ")";
    return base;
}

void ProxyPool::save() const {
    std::error_code ec;
    if (path_.has_parent_path()) fs::create_directories(path_.parent_path(), ec);
    json j = to_json_config(config_);
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out << j.dump(2);
}

}  // namespace nxrth::proxy
