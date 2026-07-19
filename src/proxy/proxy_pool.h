// Nxrth — proxy configuration model (ported from Mori/proxy_pool.rs;
// docs/port-specs/04-proxy.md §3-§5).
//
// Two concerns live here:
//   * The GAME/world proxy pool: a per-bot assigned SOCKS5 exit for the
//     subserver connection, capacity-limited per resolved IP and spread by
//     least-loaded or round-robin. A process-global published snapshot
//     (GAME_PROXY_POOL) + next_game_proxy() lets a bot fail over a dead tunnel.
//   * The rotating/bypass LOGIN proxy pool: a per-login-attempt clean exit IP,
//     IP-pinned so the HTTP token fetch and the ENet logon egress from ONE exit
//     (the Growtopia ltoken is bound to the minting IP). DataImpulse sticky
//     port ranges (host:START-END:user:pass) expand into per-session ports.
//
// Config persists to <cwd>/data/proxy_pool.json.
#pragma once

#include "net/socks5_udp.h"  // nxrth::net::Socks5Config

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nxrth::proxy {

using Socks5Config = nxrth::net::Socks5Config;

// active-count map: capacity_key ("ip:port") -> number of active bots on it.
using ActiveCounts = std::unordered_map<std::string, std::size_t>;

// --- spread mode -------------------------------------------------------------
enum class ProxySpreadMode { LeastLoaded, RoundRobin };  // Default = LeastLoaded

// "round_robin" -> RoundRobin; anything else -> LeastLoaded (default-on-unknown).
ProxySpreadMode spread_mode_from_str(const std::string& s);
// LeastLoaded -> "least_loaded", RoundRobin -> "round_robin".
const char* spread_mode_as_str(ProxySpreadMode m);

// --- one parsed proxy entry --------------------------------------------------
struct ProxyEntry {
    std::string host;                     // hostname OR IP literal, as typed
    std::uint16_t port = 0;
    std::optional<std::string> username;
    std::optional<std::string> password;
    std::optional<std::string> scheme;    // per-entry scheme override (URL form)
    std::string raw;                      // exact source text this was parsed from

    // Resolve host:port to a Socks5Config whose host holds the numeric IP.
    // Throws std::runtime_error("could not resolve proxy {host}:{port}").
    Socks5Config to_socks5() const;
    // Resolved "ip:port" — the capacity-map key / per-bot proxy_key.
    std::string capacity_key() const;
    // Display: "host:port" or "host:port (user)".
    std::string label() const;
    // HTTP-client proxy URL "{scheme}://[user[:pass]@]host:port" (userinfo
    // percent-encoded). scheme resolved via effective_proxy_scheme.
    std::string to_proxy_url(const std::string& default_scheme) const;
};

// --- persisted config (data/proxy_pool.json) ---------------------------------
struct ProxyPoolConfig {
    bool enabled = false;
    std::size_t max_bots_per_ip = 1;
    ProxySpreadMode spread_mode = ProxySpreadMode::LeastLoaded;
    bool shuffle_selection = false;
    std::vector<ProxyEntry> proxies;              // GAME/world pool
    std::size_t next_index = 0;                   // persisted round-robin cursor
    bool rotating_login_enabled = false;
    std::string rotating_login_scheme = "auto";
    std::uint16_t rotating_login_port_span = 2000;
    std::optional<ProxyEntry> rotating_login_proxy;  // LEGACY single (migration)
    std::vector<ProxyEntry> rotating_login_proxies;  // bypass login pool
};

// --- UI read-model DTOs ------------------------------------------------------
struct ProxyPoolEntryView {
    std::size_t index = 0;
    std::string label;
    std::string ip;            // capacity_key resolved "ip:port"
    std::size_t active = 0;
    std::size_t capacity = 0;
    bool full = false;
};

struct ProxyPoolView {
    bool enabled = false;
    std::size_t max_bots_per_ip = 0;
    std::string spread_mode;
    bool shuffle_selection = false;
    std::string proxies_text;
    std::size_t total = 0;
    std::size_t available = 0;
    std::size_t active = 0;
    std::vector<ProxyPoolEntryView> proxies;
    bool rotating_login_enabled = false;
    std::string rotating_login_scheme;
    std::string rotating_login_effective_scheme;
    std::uint16_t rotating_login_port_span = 1;
    std::string rotating_login_proxy_text;
    std::optional<std::string> rotating_login_proxy_label;
};

// --- IP-pinned pair for one login attempt ------------------------------------
struct BypassLoginSession {
    std::string http_url;    // SOCKS5 URL for the HTTP token fetch (libcurl)
    Socks5Config enet;       // resolved SOCKS5-UDP config for the ENet logon
};

// --- per-attempt bypass picker (cloned into each bot) ------------------------
class RotatingLoginProxy {
public:
    RotatingLoginProxy(std::vector<ProxyEntry> pool, std::uint16_t span,
                       std::string configured_scheme, bool shuffle_selection)
        : pool_(std::move(pool)), span_(span),
          configured_scheme_(std::move(configured_scheme)),
          shuffle_selection_(shuffle_selection) {}

    // Proxy URL for a FRESH exit IP each call (HTTP-only use).
    std::string fresh_url() const;
    // IP-pinned pair (single pick -> both halves). std::nullopt if the picked
    // entry is not SOCKS5/SOCKS5h or cannot be resolved.
    std::optional<BypassLoginSession> login_session() const;

private:
    ProxyEntry pick() const;  // core rotation primitive
    std::vector<ProxyEntry> pool_;
    std::uint16_t span_ = 1;
    std::string configured_scheme_;
    bool shuffle_selection_ = false;
};

// --- the stateful manager ----------------------------------------------------
class ProxyPool {
public:
    // Load <cwd>/data/proxy_pool.json (default config on any failure), apply the
    // legacy single-bypass migration, and publish the global game snapshot.
    static ProxyPool load_default();

    // Build the UI read-model. active_counts maps capacity_key -> active bots.
    ProxyPoolView view(const ActiveCounts& active_counts) const;

    // Edit + persist. Throws std::runtime_error (verbatim message) if any
    // non-blank proxy line fails to parse.
    void update(bool enabled, std::size_t max_bots_per_ip,
                const std::string& spread_mode, bool shuffle_selection,
                const std::string& proxies_text,
                bool rotating_login_enabled, const std::string& rotating_login_scheme,
                std::uint16_t rotating_login_port_span,
                const std::string& rotating_login_proxy_text);

    // Assign a game proxy to a bot. std::nullopt if the pool is disabled.
    // Throws std::runtime_error when enabled-but-empty or none-available.
    std::optional<Socks5Config> choose(const ActiveCounts& active_counts);

    // Snapshot of the bypass picker. std::nullopt if bypass disabled; throws if
    // enabled-but-empty.
    std::optional<RotatingLoginProxy> rotating_login_proxy() const;

    const ProxyPoolConfig& config() const { return config_; }

private:
    std::string rotating_login_label(const ProxyEntry& proxy) const;
    void save() const;

    std::filesystem::path path_;
    ProxyPoolConfig config_;
};

// --- process-global game-pool coordination -----------------------------------
// Round-robin the next *different* game proxy (compare by resolved ip:port),
// skipping any endpoint in the 403 quarantine. std::nullopt if fewer than 2
// proxies are published or every non-quarantined one equals `current`.
std::optional<Socks5Config> next_game_proxy(const Socks5Config* current);

// --- 403 quarantine registry (process-global, session-lived) -----------------
// A game/pool proxy whose exit IP returns HTTP 403 during login is quarantined
// here (key = resolved "ip:port"). While quarantined, choose() and
// next_game_proxy() never hand it out, so no bot logs in through it. The set is
// in-memory only (cleared on restart or via clear_quarantine()).
void quarantine_proxy(const std::string& ip_port);
bool is_proxy_quarantined(const std::string& ip_port);
std::vector<std::string> quarantined_proxies();
void clear_quarantine();

// --- active SOCKS5 reachability probe (for the Proxy tab "Check" button) ------
// Runs the full SOCKS5 handshake through `cfg`: method negotiation, RFC-1929
// user/pass auth, and UDP ASSOCIATE (exactly what a bot needs for the game).
// Returns true only if the proxy accepts it; false on ANY failure (host down,
// connection refused, wrong username/password, no UDP ASSOCIATE). Blocks up to
// ~25s (10s TCP connect + 15s control r/w) — always call off the UI thread.
bool probe_socks5(const Socks5Config& cfg);

// --- free parsing / scheme helpers (exposed for callers + tests) -------------
std::vector<ProxyEntry> parse_proxy_lines(const std::string& input);
std::optional<ProxyEntry> parse_optional_proxy(const std::string& input);
const char* normalize_proxy_scheme(const std::string& value);
const char* effective_proxy_scheme(const std::string& configured_scheme,
                                   const ProxyEntry& proxy);
const char* infer_proxy_scheme_from_port(std::uint16_t port);
std::uint16_t normalized_rotating_port_span(std::uint16_t base_port,
                                            std::uint16_t span);

}  // namespace nxrth::proxy
