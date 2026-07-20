// Nxrth — Bot connection/login half (port spec 06 "Bot Core & State").
//
// This translation unit owns the CONNECTION + LOGIN machinery of the Bot class:
//   * the five spawn factories + the private full constructor (§2.4)
//   * the run() driver loop (§2.7) incl. the 30 s login watchdog
//   * create_host() wiring (§2.3, via net/enet_host.h)
//   * the Connect/Disconnect state machine (on_connect/on_disconnect, §2.8)
//   * reconnect_main / schedule_reconnect / refresh_token / apply_credentials (§2.13-2.15)
//   * wait_for_global_gate — the connected-phase gate wait that KEEPS ENET SERVICED (§2.9)
//   * fetch_server_data_loop (§2.6) + the login/connection free helpers (§2.1)
//
// Packet parsing, GameMessage scanning + the login-packet builders live in the
// handlers TU (service_once, on_receive, on_server_hello, handle_game_message,
// on_call_function); the emit/log/collect primitives live in their own units.
// Those methods are declared in bot.h and called cross-TU from here.
//
// GT wire data is little-endian; all timing here is std::chrono::steady_clock
// (monotonic) except now_millis() which is the one wall-clock use.
#include "bot/bot.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "core/constants.h"
#include "core/logger.h"
#include "net/server_data.h"
#include "protocol/crypto.h"

namespace consts = nxrth::constants;

// Nxrth uses legacy GrowID and Google OAuth ltoken login. Retired dashboard
// variants and HAR paths are intentionally absent.

namespace nxrth::bot {

using Clock = std::chrono::steady_clock;
using nxrth::login::Credentials;
using nxrth::login::LoginIdentity;
using nxrth::login::LoginMethod;
using nxrth::login::LoginMethodKind;
using nxrth::login::LogFn;
using nxrth::net::ServerData;
using nxrth::net::kHostChannelLimit;
using nxrth::net::kPeerTimeoutLimit;
using nxrth::net::kPeerTimeoutMaxMs;
using nxrth::net::kPeerTimeoutMinMs;

namespace {

// --- small string utilities --------------------------------------------------
std::string trim_copy(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && static_cast<unsigned char>(s[b]) <= ' ') ++b;
    while (e > b && static_cast<unsigned char>(s[e - 1]) <= ' ') --e;
    return s.substr(b, e - b);
}

std::string to_lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// HTTP side of the login uses remote proxy DNS (`socks5h`) and the same proxy
// endpoint as ENet, keeping the OAuth-derived token on one egress IP.
std::string socks5_to_url(const Socks5Config& c) {
    std::string u = "socks5h://";
    if (c.username && c.password) u += *c.username + ":" + *c.password + "@";
    u += c.host + ":" + std::to_string(c.port);
    return u;
}

struct LtokenServerData {
    ServerData data;
    std::optional<Socks5Config> provider_enet;
    std::optional<std::string> http_proxy_url;
};

std::optional<LtokenServerData> fetch_ltoken_server_data(
    const std::optional<Socks5Config>& proxy,
    const std::optional<nxrth::proxy::RotatingLoginProxy>& login_proxy,
    bool google_refresh_token, std::atomic<bool>& stop, const LogFn& log) {
    nxrth::net::LoginInfo login_info;
    login_info.protocol = consts::PROTOCOL;
    login_info.game_version = std::string(consts::GAME_VER);

    bool alternate = false;
    while (!stop.load()) {
        std::optional<std::string> proxy_url;
        std::optional<Socks5Config> provider_enet;
        if (!google_refresh_token && login_proxy) {
            if (auto session = login_proxy->login_session()) {
                proxy_url = session->http_url;
                provider_enet = std::move(session->enet);
            }
        } else if (proxy) {
            proxy_url = socks5_to_url(*proxy);
        }
        if (!proxy_url)
            log("[Bot] no proxy configured - logging in DIRECT via the real IP.");

        log("[Bot] fetching server_data for " +
            std::string(google_refresh_token ? "Google OAuth" : "provider ltoken") +
            " (alternate=" +
            std::string(alternate ? "true" : "false") + ")...");
        auto result = nxrth::net::get_server_data_proxied(alternate, login_info, proxy_url);
        if (result.ok())
            return LtokenServerData{std::move(*result.data), std::move(provider_enet),
                                    std::move(proxy_url)};
        log("[Bot] ltoken server_data failed: " + result.error + " - retrying in 5s");
        alternate = !alternate;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    log("[Bot] ltoken login aborted - bot was stopped");
    return std::nullopt;
}

// Toggle the http/https scheme of a login-proxy URL (the "alt scheme" fallback
// in reconnect_main §2.13.5). std::nullopt for non-HTTP (e.g. socks5://) URLs.
std::optional<std::string> alternate_scheme_url(const std::string& url) {
    if (url.rfind("http://", 0) == 0) return "https://" + url.substr(7);
    if (url.rfind("https://", 0) == 0) return "http://" + url.substr(8);
    return std::nullopt;
}

// Render a resolved endpoint as "ip:port" for log lines.
std::string ep_str(const SockEndpoint& e) {
    char host[NI_MAXHOST] = {0};
    char serv[NI_MAXSERV] = {0};
    if (::getnameinfo(e.addr(), e.len, host, sizeof host, serv, sizeof serv,
                      NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        return std::string(host) + ":" + serv;
    }
    return "?:?";
}

// Build the pre-construction login LogFn: logger::log + BotState.console ring
// (cap 100, drop oldest) + EventSink::console. Mirrors log_console for the phase
// before the Bot object exists (spec §2.4 "same shape as constructors").
LogFn make_login_log_fn(std::shared_ptr<SharedBotState> state, std::uint32_t bot_id,
                        EventSinkPtr sink) {
    return [state, bot_id, sink](const std::string& message) {
        nxrth::log(message, static_cast<int>(bot_id));
        if (state) {
            state->write([&](BotState& s) {
                s.console.push_back(message);
                while (s.console.size() > CONSOLE_RING_CAP) s.console.pop_front();
            });
        }
        if (sink) sink->console(bot_id, message);
    };
}

// §2.4 tail: fleet-stagger the HTTP login phase (dead-sleep gate), logging the
// wait if it was non-zero.
void stagger_http_login(const LogFn& log) {
    std::uint64_t waited = wait_global_gate(http_login_gate(), HTTP_LOGIN_STAGGER_MS);
    if (waited > 0) {
        log("[Bot] staggering HTTP login by " + std::to_string(waited) +
            " ms (avoids simultaneous-login throttle)");
    }
}

// §2.6 server_data fetch loop, shared by the ltoken/har factories + (via the Bot
// member) reconnect_main's simple path. Loops until success or stop.
std::optional<ServerData> fetch_server_data_free(
    const std::optional<Socks5Config>& proxy,
    const std::optional<nxrth::proxy::RotatingLoginProxy>& login_proxy, std::atomic<bool>& stop,
    const LogFn& log) {
    nxrth::net::LoginInfo login_info;
    login_info.protocol = consts::PROTOCOL;
    login_info.game_version = std::string(consts::GAME_VER);
    std::optional<Socks5Config> current = proxy;  // swapped on a 403
    bool alternate = false;
    for (;;) {
        if (stop.load()) {
            log("[Bot] login aborted — bot was stopped");
            return std::nullopt;
        }
        std::optional<std::string> proxy_url;
        if (login_proxy)
            proxy_url = login_proxy->fresh_url();
        else if (current)
            proxy_url = socks5_to_url(*current);
        // No proxy -> direct server_data fetch (uses the real IP). Not blocked: the
        // user chose to run without proxies.

        log("[Bot] fetching server_data (alternate=" + std::string(alternate ? "true" : "false") +
            ")...");
        auto res = nxrth::net::get_server_data_proxied(alternate, login_info, proxy_url);
        if (res.ok()) return res.data;

        // A 403 means THIS game-proxy exit IP is blocked. Quarantine it fleet-wide
        // (no bot logs in through it) and pull a clean replacement from the pool
        // instead of hammering the same dead IP every 5s.
        std::string err_low = res.error;
        std::transform(err_low.begin(), err_low.end(), err_low.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool is_403 =
            err_low.find("403") != std::string::npos || err_low.find("forbidden") != std::string::npos;
        if (is_403 && !login_proxy && current) {
            const std::string key = current->host + ":" + std::to_string(current->port);
            nxrth::proxy::quarantine_proxy(key);
            log("[Bot] server_data via game proxy " + key +
                " returned 403 - quarantined fleet-wide; pulling a replacement from the pool.");
            if (auto repl = nxrth::proxy::next_game_proxy(&*current)) {
                current = repl;
                log("[Bot] switched to replacement game proxy " + repl->host + ":" +
                    std::to_string(repl->port) + " - retrying now.");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            log("[Bot] no clean replacement proxy left in the pool - refusing direct fetch to "
                "protect the real IP; stopping bot.");
            stop.store(true);
            return std::nullopt;
        }

        log("[Bot] fetch: server_data failed: " + res.error + " — retrying in 5s");
        alternate = !alternate;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

}  // namespace

// ===========================================================================
// §2.1 free helpers (nxrth::bot) — login/connection flavour
// ===========================================================================

std::string default_klv(std::string_view rid, std::string_view hash) {
    std::int32_t hash_val;
    try {
        hash_val = static_cast<std::int32_t>(std::stol(std::string(hash)));
    } catch (...) {
        hash_val = static_cast<std::int32_t>(std::stol(std::string(consts::DEFAULT_HASH)));
    }
    return nxrth::protocol::compute_klv(consts::GAME_VER, std::to_string(consts::PROTOCOL), rid,
                                         hash_val);
}

std::string value_or_default(std::string value, std::string_view def) {
    if (trim_copy(value).empty()) return std::string(def);
    return value;
}

// Protocol number for the provider-ltoken gateway + redirect packets. The current
// working reference client sends protocol|225 (with game_version|5.51); default to
// that, overridable via NXRTH_PLTOKEN_PROTOCOL for empirical iteration.
std::string provider_login_protocol() {
    if (const char* p = std::getenv("NXRTH_PLTOKEN_PROTOCOL")) {
        std::string v = trim_copy(p);
        if (!v.empty()) return v;
    }
    return "225";
}

std::string build_checktoken_client_data(const LoginIdentity& id, const std::string& meta,
                                         const std::string& protocol, bool oauth) {
    // PLAINTEXT clientData (GT ignores unknown keys). check_token form-encodes this;
    // GT decodes it to plaintext (NOT base64). Load-bearing fields are the device
    // (rid/mac/vid) + the server_data meta.
    if (oauth) {
        // The "more advanced" reference (bombo) OAuth clientData: tankIDName carries
        // the account name, GDPR|1, no fz/zf/lmode, and the vid line is omitted when
        // empty. cbits/country/hash2 come from the (OAuth-tuned) identity.
        const std::string vid_line = id.vid.empty() ? std::string() : ("vid|" + id.vid + "\n");
        return "tankIDName|" + id.tank_id_name + "\ntankIDPass|\nrequestedName|\nf|1\nprotocol|" +
               protocol + "\ngame_version|" + id.game_version + "\ncbits|" + id.cbits +
               "\nplayer_age|" + id.player_age + "\nGDPR|" + id.gdpr + "\nFCMToken|\ncategory|" +
               id.category + "\ntotalPlaytime|0\nklv|" + id.klv + "\nhash2|" + id.hash2 + "\n" +
               vid_line + "aid|\nmeta|" + meta + "\nfhash|" + std::to_string(consts::FHASH) +
               "\nrid|" + id.rid + "\nplatformID|" + id.platform_id + "\ndeviceVersion|0\ncountry|" +
               id.country + "\nhash|" + id.hash + "\nmac|" + id.mac + "\nwk|" + id.wk + "\n";
    }
    // Non-OAuth (Google refresh-token) superset format.
    return "tankIDName|\ntankIDPass|\nrequestedName|\nf|1\nprotocol|" + protocol +
           "\ngame_version|" + id.game_version + "\nfz|" + id.fz + "\ncbits|" + id.cbits +
           "\nplayer_age|" + id.player_age + "\nGDPR|" + id.gdpr + "\nFCMToken|\ncategory|" +
           id.category + "\ntotalPlaytime|0\nklv|" + id.klv + "\nhash2|" + id.hash2 + "\nvid|" +
           id.vid + "\naid|\nmeta|" + meta + "\nfhash|" + std::to_string(consts::FHASH) + "\nrid|" +
           id.rid + "\nplatformID|" + id.platform_id + "\ndeviceVersion|0\ncountry|" + id.country +
           "\nhash|" + id.hash + "\nmac|" + id.mac + "\nwk|" + id.wk + "\nzf|" + id.zf +
           "\nlmode|1\n";
}

LoginIdentity resolve_login_identity(const std::optional<LoginIdentity>& in) {
    LoginIdentity id = in ? *in : LoginIdentity{};
    // rid + hash first, then the fallback klv is derived from the RESOLVED pair.
    id.rid = value_or_default(id.rid, consts::DEFAULT_RID);
    id.hash = value_or_default(id.hash, consts::DEFAULT_HASH);
    std::string fallback_klv = default_klv(id.rid, id.hash);
    id.game_version = value_or_default(id.game_version, consts::GAME_VER);
    id.cbits = value_or_default(id.cbits, "1536");
    id.player_age = value_or_default(id.player_age, "23");
    id.gdpr = value_or_default(id.gdpr, "2");
    id.category = value_or_default(id.category, "_16");
    id.total_playtime = value_or_default(id.total_playtime, "0");
    id.country = value_or_default(id.country, "us");
    id.zf = value_or_default(id.zf, consts::DEFAULT_ZF);
    id.mac = value_or_default(id.mac, consts::DEFAULT_MAC);
    id.wk = value_or_default(id.wk, consts::DEFAULT_WK);
    id.hash2 = value_or_default(id.hash2, consts::DEFAULT_HASH2);
    id.fz = value_or_default(id.fz, consts::DEFAULT_FZ);
    id.platform_id = value_or_default(id.platform_id, consts::DEFAULT_PLATFORM_ID);
    id.steam_token = value_or_default(id.steam_token, consts::DEFAULT_STEAM_TOKEN);
    id.klv = value_or_default(id.klv, fallback_klv);
    return id;
}

std::vector<std::uint16_t> sorted_blacklist_vec(const std::unordered_set<std::uint16_t>& set) {
    std::vector<std::uint16_t> v(set.begin(), set.end());
    std::sort(v.begin(), v.end());
    return v;
}

std::uint64_t now_millis() {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    if (ms < 0) return 0;
    return static_cast<std::uint64_t>(ms);
}

bool is_http_403_text(const std::string& message) {
    std::string m = to_lower_copy(message);
    return m.find("403") != std::string::npos || m.find("forbidden") != std::string::npos;
}

const char* login_token_field(const std::string& token) {
    int dots = 0;
    for (char c : token)
        if (c == '.') ++dots;
    return dots >= 2 ? "UbiTicket" : "token";  // >= 3 dot-separated segments -> UbiTicket
}

std::optional<LtokenRecord> parse_ltoken_string(const std::string& s) {
    std::string t = trim_copy(s);
    if (t.empty()) return std::nullopt;
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        std::size_t bar = t.find('|', start);
        if (bar == std::string::npos) {
            parts.push_back(trim_copy(t.substr(start)));
            break;
        }
        parts.push_back(trim_copy(t.substr(start, bar - start)));
        start = bar + 1;
    }

    // "@rja_<id>|<mac>:<rid>:<wk>:<ltoken>" delivery (the LTOKEN script export). The
    // 2nd pipe field packs mac(6 octets):rid:wk:ltoken and the mac keeps its own
    // colons, so peel the LAST three colons off the right. Rewrite to the positional
    // token|rid|mac|wk form the logic below already understands.
    if (parts.size() == 2) {
        const std::string& sisa = parts[1];
        const std::size_t c1 = sisa.rfind(':');
        const std::size_t c2 = (c1 == std::string::npos || c1 == 0) ? std::string::npos
                                                                     : sisa.rfind(':', c1 - 1);
        const std::size_t c3 = (c2 == std::string::npos || c2 == 0) ? std::string::npos
                                                                     : sisa.rfind(':', c2 - 1);
        if (c3 != std::string::npos) {
            const std::string mac = sisa.substr(0, c3);
            const std::string rid = sisa.substr(c3 + 1, c2 - c3 - 1);
            const std::string wk = sisa.substr(c2 + 1, c1 - c2 - 1);
            const std::string ltoken = sisa.substr(c1 + 1);
            auto is_hex32 = [](const std::string& v) {
                return v.size() == 32 && std::all_of(v.begin(), v.end(), [](unsigned char c) {
                           return std::isxdigit(c) != 0;
                       });
            };
            auto is_mac = [](const std::string& m) {
                int octets = 0;
                std::size_t i = 0;
                while (i < m.size()) {
                    if (i + 1 >= m.size() || !std::isxdigit(static_cast<unsigned char>(m[i])) ||
                        !std::isxdigit(static_cast<unsigned char>(m[i + 1])))
                        return false;
                    ++octets;
                    i += 2;
                    if (i < m.size()) {
                        if (m[i] != ':') return false;
                        ++i;
                    }
                }
                return octets == 6;
            };
            if (!ltoken.empty() && is_hex32(rid) && is_hex32(wk) && is_mac(mac))
                parts = {ltoken, rid, mac, wk};
        }
    }

    auto normalized_key = [](std::string key) {
        key = to_lower_copy(trim_copy(key));
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) {
                      return c == '_' || c == '-';
                  }),
                  key.end());
        return key;
    };
    auto valid_decimal = [](const std::string& value, std::size_t max_len) {
        return !value.empty() && value.size() <= max_len &&
               std::all_of(value.begin(), value.end(),
                           [](unsigned char c) { return std::isdigit(c) != 0; });
    };
    auto valid_platform = [](const std::string& value) {
        return !value.empty() && value.size() <= 16 && value.front() != ',' &&
               value.back() != ',' &&
               std::all_of(value.begin(), value.end(), [](unsigned char c) {
                   return std::isdigit(c) != 0 || c == ',';
               });
    };
    auto safe_text = [](const std::string& value, std::size_t max_len) {
        return !value.empty() && value.size() <= max_len &&
               value.find_first_of("\r\n") == std::string::npos;
    };

    bool keyed = false;
    for (const auto& part : parts) {
        const auto colon = part.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = normalized_key(part.substr(0, colon));
        if (key == "token" || key == "refreshtoken" || key == "rid" || key == "mac" ||
            key == "wk" || key == "platform" || key == "platformid" || key == "name" ||
            key == "username" || key == "cbits" || key == "playerage" || key == "vid") {
            keyed = true;
            break;
        }
    }

    LtokenRecord record;
    if (!keyed) {
        if (parts.size() != 4) return std::nullopt;
        record.token = parts[0];
        record.rid = parts[1];
        record.mac = parts[2];
        record.wk = parts[3];
    } else {
        std::unordered_set<std::string> seen;
        for (const auto& part : parts) {
            const auto colon = part.find(':');
            if (colon == std::string::npos) continue;  // tolerate provider extensions
            std::string key = normalized_key(part.substr(0, colon));
            std::string value = trim_copy(part.substr(colon + 1));
            const bool refresh_token_key = key == "refreshtoken";
            if (refresh_token_key) key = "token";
            if (key == "platformid") key = "platform";
            if (key == "username") key = "name";
            const bool known = key == "token" || key == "rid" || key == "mac" || key == "wk" ||
                               key == "platform" || key == "name" || key == "cbits" ||
                               key == "playerage" || key == "vid";
            if (!known) continue;
            if (!seen.insert(key).second) return std::nullopt;

            if (key == "token") {
                record.token = std::move(value);
                record.kind = refresh_token_key ? LtokenRecord::Kind::GoogleRefreshToken
                                                : LtokenRecord::Kind::ProviderToken;
            }
            else if (key == "rid") record.rid = std::move(value);
            else if (key == "mac") record.mac = std::move(value);
            else if (key == "wk") record.wk = std::move(value);
            else if (key == "platform" && valid_platform(value))
                record.platform_id = std::move(value);
            else if (key == "name" && safe_text(value, 128))
                record.name = std::move(value);
            else if (key == "cbits" && valid_decimal(value, 10))
                record.cbits = std::move(value);
            else if (key == "playerage" && valid_decimal(value, 3))
                record.player_age = std::move(value);
            else if (key == "vid" && safe_text(value, 128))
                record.vid = std::move(value);
        }
    }

    const bool valid_wk = record.wk.size() == 32 || to_lower_copy(record.wk) == "none0";
    if (record.token.empty() || record.rid.size() != 32 || record.mac.empty() || !valid_wk)
        return std::nullopt;
    return record;
}

std::optional<SockEndpoint> resolve_endpoint(const std::string& host, std::uint16_t port) {
    if (host.empty()) return std::nullopt;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    std::string port_s = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || res == nullptr) {
        if (res) ::freeaddrinfo(res);
        return std::nullopt;
    }
    SockEndpoint e;
    std::memcpy(&e.ss, res->ai_addr, res->ai_addrlen);
    e.len = static_cast<socklen_t>(res->ai_addrlen);
    ::freeaddrinfo(res);
    return e;
}

// ===========================================================================
// §2.4 constructors — factories return nullptr on failed spawn, never throw
// ===========================================================================

std::unique_ptr<Bot> Bot::create(
    const std::string& username, const std::string& password, std::optional<Socks5Config> proxy,
    std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy,
    std::shared_ptr<std::atomic<bool>> stop, std::shared_ptr<SharedBotState> state,
    CmdReceiver cmd_rx, std::shared_ptr<const nxrth::world::ItemsDat> items_dat,
    std::uint32_t bot_id, EventSinkPtr sink, FleetHandle fleet) {
    LogFn log = make_login_log_fn(state, bot_id, sink);
    stagger_http_login(log);
    auto creds = nxrth::login::fetch_credentials(username, password, proxy, login_proxy, *stop, log);
    if (!creds) return nullptr;
    LoginMethod lm;
    lm.kind = LoginMethodKind::Legacy;
    lm.password = password;
    return std::unique_ptr<Bot>(new Bot(username, std::move(lm), std::move(*creds), std::move(proxy),
                                        std::move(login_proxy), std::move(stop), std::move(state),
                                        std::move(cmd_rx), std::move(items_dat), bot_id,
                                        std::move(sink), std::move(fleet)));
}

std::unique_ptr<Bot> Bot::create_ltoken(
    const std::string& ltoken_str, std::optional<Socks5Config> proxy,
    std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy,
    std::shared_ptr<std::atomic<bool>> stop, std::shared_ptr<SharedBotState> state,
    CmdReceiver cmd_rx, std::shared_ptr<const nxrth::world::ItemsDat> items_dat,
    std::uint32_t bot_id, EventSinkPtr sink, FleetHandle fleet) {
    LogFn log = make_login_log_fn(state, bot_id, sink);

    auto parsed = parse_ltoken_string(ltoken_str);
    if (!parsed) {
        log("[Bot] Invalid ltoken string - expected refreshToken|rid|mac|wk or a keyed "
            "mac:/wk:/rid:/token: record; not spawning bot");
        return nullptr;
    }

    const bool google_refresh_token =
        parsed->kind == LtokenRecord::Kind::GoogleRefreshToken;

    LoginIdentity id;
    id.rid = parsed->rid;
    id.mac = parsed->mac;
    id.wk = parsed->wk;  // provider records ship wk:NONE0; kept LITERALLY
    id.hash = std::to_string(nxrth::protocol::hash_string(id.mac + "RT"));
    id.game_version = std::string(consts::GAME_VER);
    id.category = "_-5100";
    id.total_playtime = "0";
    id.fz = "22243512";
    id.zf = "31631978";
    id.steam_token.clear();
    if (google_refresh_token) {
        // core.rs checktoken clientData identity (the exchanged-token path).
        id.hash2 = std::to_string(
            nxrth::protocol::hash_string(nxrth::protocol::random_hex(16) + "RT"));
        id.cbits = "1024";
        id.player_age = "20";
        id.gdpr = "2";
        id.country = "jp";
        id.platform_id = "0,1,1";
        id.klv = default_klv(id.rid, id.hash);
    } else {
        // Provider (Apple/Google) refresh token. This identity feeds the checktoken
        // clientData AND the subserver/redirect packet, matching the working reference
        // client: platformID|1, hash2 == hash, + vid/aid. (The first GATEWAY packet is
        // the bare core.rs form protocol|226/ltoken/platformID|2 — see build_login_packet.)
        id.vid = parsed->vid.value_or(std::string());
        // Identity tuned to the "more advanced" bombo OAuth reference (matches the
        // working Lucifer Google/CID client session): tankIDName = the account name,
        // cbits forced to 1024, GDPR|1, country|us, platformID|1, and a FRESH random
        // hash2 (not hash2 == hash). player_age comes from the record (default 25).
        id.tank_id_name = parsed->name.value_or(std::string());
        id.cbits = "1024";
        id.player_age = parsed->player_age.value_or("25");
        id.gdpr = "1";
        id.country = "us";
        id.platform_id = parsed->platform_id.value_or("1");
        id.hash2 =
            std::to_string(nxrth::protocol::hash_string(nxrth::protocol::random_hex(16) + "RT"));
        int hv = 0;
        try {
            hv = std::stoi(id.hash);
        } catch (...) {
            hv = 0;
        }
        id.klv = nxrth::protocol::compute_klv(consts::GAME_VER, provider_login_protocol(),
                                               id.rid, hv);
    }

    stagger_http_login(log);
    auto sd = fetch_ltoken_server_data(proxy, login_proxy, google_refresh_token, *stop, log);
    if (!sd) return nullptr;
    if (!resolve_endpoint(sd->data.server, sd->data.port)) {
        log("[Bot] invalid server address '" + sd->data.server + ":" +
            std::to_string(sd->data.port) + "' — not spawning bot");
        return nullptr;
    }

    // BOTH the provider (Apple/Google) and refreshToken records validate through
    // /player/growid/checktoken: POST refreshToken=<record token> + a PLAINTEXT
    // clientData carrying THIS device + the server_data `meta`. checktoken returns the
    // fresh session ltoken that the ENet gateway accepts. (Verified empirically:
    // the raw record token is a refresh token — the gateway rejects it directly with
    // "Fail to login. Please try again in 30 seconds."; klv/hash are not validated,
    // but the device rid/mac/vid and a real meta are required, and clientData must be
    // plaintext form-encoded, NOT base64.)
    Credentials creds;
    const bool provider = !google_refresh_token;
    const std::string client_data = build_checktoken_client_data(
        id, sd->data.meta,
        provider ? provider_login_protocol() : std::to_string(consts::PROTOCOL),
        /*oauth=*/provider);

    log("[Bot] validating " + std::string(provider ? "provider" : "Google OAuth") +
        " ltoken through checktoken (" + nxrth::login::token_fingerprint(parsed->token) + ")");
    auto checked = nxrth::login::check_token(parsed->token, client_data, sd->http_proxy_url);
    if (!checked.ok()) {
        const std::string reason = checked.error ? checked.error->display() : "unknown error";
        log("[Bot] ltoken validation failed: " + reason + " - stopping bot");
        stop->store(true);
        return nullptr;
    }
    creds.ltoken = std::move(*checked.token);
    log("[Bot] ltoken validated via checktoken; session ltoken " +
        nxrth::login::token_fingerprint(creds.ltoken));
    creds.meta = sd->data.meta;
    creds.server = sd->data.server;
    creds.port = sd->data.port;
    creds.identity = id;
    if (!google_refresh_token) creds.bypass_enet = std::move(sd->provider_enet);

    LoginMethod lm;
    lm.kind = google_refresh_token ? LoginMethodKind::Ltoken
                                   : LoginMethodKind::ProviderLtoken;
    lm.source_token = parsed->token;  // original refresh token, re-exchanged on reconnect
    if (google_refresh_token) login_proxy.reset();
    return std::unique_ptr<Bot>(new Bot(parsed->name.value_or(std::string()), std::move(lm),
                                        std::move(creds),
                                        std::move(proxy), std::move(login_proxy),
                                        std::move(stop),
                                        std::move(state), std::move(cmd_rx), std::move(items_dat),
                                        bot_id, std::move(sink), std::move(fleet)));
}

// --- private full constructor (§2.4 new_with_credentials, §2.5 defaults) -----
Bot::Bot(std::string username, LoginMethod login_method, Credentials creds,
         std::optional<Socks5Config> proxy,
         std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy,
         std::shared_ptr<std::atomic<bool>> stop, std::shared_ptr<SharedBotState> state,
         CmdReceiver cmd_rx, std::shared_ptr<const nxrth::world::ItemsDat> items_dat,
         std::uint32_t bot_id, EventSinkPtr sink, FleetHandle fleet)
    : proxy_(std::move(proxy)),
      login_proxy_(std::move(login_proxy)),
      stop_(std::move(stop)),
      username_(std::move(username)),
      login_method_(std::move(login_method)),
      start_time_(Clock::now()),
      state_(std::move(state)),
      cmd_rx_(std::move(cmd_rx)),
      collect_timer_(Clock::now()),
      items_dat_(std::move(items_dat)),
      fleet_(std::move(fleet)),
      bot_id_(bot_id),
      sink_(std::move(sink)) {
    ltoken_ = creds.ltoken;
    meta_ = creds.meta;
    bypass_enet_ = creds.bypass_enet;

    // Resolve + assign the 17 identity strings (fills blanks with defaults).
    apply_login_identity(creds.identity ? *creds.identity : LoginIdentity{});

    server_addr_ = resolve_endpoint(creds.server, creds.port);

    // Gateway logon must leave from the pinned bypass IP (the one that minted the
    // ltoken); world/subserver traffic later switches to the game proxy.
    const Socks5Config* logon_proxy =
        bypass_enet_ ? &*bypass_enet_ : (proxy_ ? &*proxy_ : nullptr);
    host_ = create_host(logon_proxy);

    if (state_) {
        state_->write([&](BotState& s) {
            s.username = username_;
            s.mac = mac_;
            s.collect_radius_tiles = collect_radius_tiles_;
            s.collect_blacklist = sorted_blacklist_vec(collect_blacklist_);
        });
    }

    if (server_addr_) {
        host_.connect(server_addr_->addr(), server_addr_->len, kHostChannelLimit, 0);
    }
}

Bot::~Bot() = default;

// ===========================================================================
// §2.3 create_host — anti-IP-leak host builder (delegates to BotHost::create,
// which logs the proxy URL, retries the SOCKS5 UDP bind 4× / 300 ms, then falls
// back to a DEAD loopback socket — never a leaking direct connection).
// ===========================================================================
nxrth::net::BotHost Bot::create_host(const Socks5Config* proxy) {
    return nxrth::net::BotHost::create(proxy);
}

// ===========================================================================
// §2.7 run() — the driver loop (one bot thread body)
// ===========================================================================
void Bot::run(std::shared_ptr<std::atomic<bool>> stop_flag) {
    for (;;) {
        if (stop_flag && stop_flag->load()) {
            log_console("[Bot] Stop flag set, exiting.");
            break;
        }
        if (stop_requested_) {
            log_console("[Bot] Stop requested internally, exiting.");
            break;
        }

        // (3) delayed reconnect cooldown elapsed.
        if (reconnect_after_ && Clock::now() >= *reconnect_after_) {
            reconnect_after_.reset();
            if (auto_reconnect_) {
                bool refresh = refresh_token_on_reconnect_;
                refresh_token_on_reconnect_ = false;
                log_console("[Bot] Reconnect cooldown elapsed — reconnecting with current session");
                reconnect_main(refresh);
            }
        }

        // (4) drain UI/manager commands.
        drain_commands();

        const auto loop_now = Clock::now();

        // (5) Publish ping at telemetry cadence. ENet is still serviced every
        // 10 ms below; RTT does not need thousands of shared-state writes/sec.
        if (peer_id_ && loop_now >= ping_timer_) {
            ping_timer_ = loop_now + std::chrono::seconds(1);
            std::uint32_t rtt = host_.peer_rtt(*peer_id_);
            if (rtt != last_ping_) {
                last_ping_ = rtt;
                if (state_) state_->write([&](BotState& s) { s.ping_ms = rtt; });
                notify_dirty();
            }
        }

        // (6) process all pending ENet events.
        service_once();

        // (6b) release any place that the server never confirmed (lag / no build
        // access) so its reserved item becomes placeable again — without consuming.
        expire_pending_places();

        // (7) login watchdog: 30 s connected with no world = flaky game proxy.
        if (connected_since_ && !world_.has_value() &&
            (Clock::now() - *connected_since_) >= std::chrono::seconds(30)) {
            log_console(
                "[Bot] login stalled — 30s connected with no world (flaky game proxy?); dropping to "
                "retry via gateway");
            if (auto fresh = nxrth::proxy::next_game_proxy(proxy_ ? &*proxy_ : nullptr)) {
                log_console("[Bot] rotating game proxy after stall → " + fresh->host + ":" +
                            std::to_string(fresh->port));
                proxy_ = fresh;
            }
            connected_since_.reset();
            redirect_.reset();
            redirect_attempts_ = 0;
            redirect_connect_fails_ = 0;
            saw_server_hello_ = false;  // route the resulting Disconnect to a clean gateway reconnect
            if (peer_id_) host_.peer_disconnect(*peer_id_, 0);
        }

        // (8) Fleet telemetry and automation use separate cadences from ENet.
        // Actions themselves retain their original delays and keep ENet serviced.
        if (fleet_) {
            if (loop_now >= fleet_publish_timer_) {
                fleet_publish_timer_ = loop_now + std::chrono::milliseconds(250);
                publish_fleet_view();
            }
            if (loop_now >= automation_timer_) {
                automation_timer_ = loop_now + std::chrono::milliseconds(50);
                run_automation(*fleet_);
            }
        }

        // (9) auto-collect tick.
        if (auto_collect_ && (Clock::now() - collect_timer_) >= std::chrono::milliseconds(500)) {
            collect_timer_ = Clock::now();
            collect();
        }

        // (10) breathe.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    shutdown();
}

void Bot::drain_commands() {
    while (auto cmd = cmd_rx_->try_recv()) {
        handle_command(*cmd);
    }
}

// service-while-sleeping: keeps ENet alive during a blocking action's wait.
void Bot::sleep_ms(std::uint64_t ms) {
    auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) {
        service_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Public wrapper so automation modules can pause without dead-sleeping ENet.
void Bot::idle(std::uint64_t ms) { sleep_ms(ms); }

// Public log passthrough so automation modules can report status to the console.
void Bot::log(const std::string& msg) { log_console(msg); }

void Bot::shutdown() {
    script_stop_->store(true);
    if (cmd_rx_) cmd_rx_->close_consumer();
    if (peer_id_) {
        host_.peer_disconnect(*peer_id_, 0);
        for (int i = 0; i < 5; ++i) {  // flush the disconnect
            service_once();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (fleet_) fleet_->release_all(bot_id_);
}

// ===========================================================================
// §2.8 Connect / Disconnect state machine
// ===========================================================================
void Bot::on_connect(nxrth::net::PeerId id) {
    peer_id_ = id;
    saw_server_hello_ = false;
    connected_since_ = Clock::now();
    // limit=0 keeps ENet default (32); minimum 12 s so a brief UDP relay gap
    // doesn't drop an in-world bot; maximum 30 s so a sustained outage times out.
    host_.peer_set_timeout(id, kPeerTimeoutLimit, kPeerTimeoutMinMs, kPeerTimeoutMaxMs);
    log_console("[Bot] Connected: peer " + std::to_string(id.index));
}

void Bot::on_disconnect(nxrth::net::PeerId id, std::uint32_t data) {
    peer_id_.reset();
    connected_since_.reset();
    pathfind_target_.reset();
    pathfind_recalc_ = false;

    log_console("[Bot] Disconnected: peer " + std::to_string(id.index) + " (reason code " +
                std::to_string(data) + ")");

    const bool disconnected_before_server_hello = !redirect_.has_value() && !saw_server_hello_;

    if (state_) {
        state_->write([&](BotState& s) {
            s.world_name.clear();
            s.players.clear();
            s.ping_ms = 0;
        });
    }
    emit_status(BotStatus::Connecting);

    // (A) an unconsumed redirect is in flight.
    if (redirect_) {
        if (redirect_attempts_ > 0) {
            log_console("[Bot] Redirect token was already used " +
                        std::to_string(redirect_attempts_) +
                        " time(s) — waiting for a fresh redirect");
            redirect_.reset();
            redirect_attempts_ = 0;
            reconnect_main(false);  // warp redirect: always complete, regardless of auto-reconnect
            return;
        }
        RedirectData r = *redirect_;
        auto ep = resolve_endpoint(r.server, r.port);
        if (ep) {
            redirect_connect_fails_ += 1;
            if (redirect_connect_fails_ >= REDIRECT_MAX_GAME_PROXY_TRIES) {
                log_console("[Bot] redirect: " + std::to_string(redirect_connect_fails_) +
                            " game proxies failed to reach subserver " + r.server + ":" +
                            std::to_string(r.port) + " — abandoning redirect, full re-login");
                redirect_.reset();
                redirect_attempts_ = 0;
                redirect_connect_fails_ = 0;
                reconnect_main(true);  // warp redirect exhausted proxies: full re-login to finish the warp
                return;
            }
            if (redirect_connect_fails_ >= 2) {
                if (auto fresh = nxrth::proxy::next_game_proxy(proxy_ ? &*proxy_ : nullptr)) {
                    log_console(
                        "[Bot] redirect: subserver unreachable via current game proxy — rotating "
                        "to " +
                        fresh->host + ":" + std::to_string(fresh->port));
                    proxy_ = fresh;
                }
            }
            // Subserver egress: prefer a dedicated GAME proxy, but if none is set
            // fall back to the BYPASS proxy (the same working SOCKS5-UDP exit the
            // login used) instead of nullptr. nullptr -> create_host binds a DEAD
            // loopback (anti-leak), which makes the subserver connect ALWAYS fail ->
            // 6 fails -> a needless full re-login on every warp for bypass-only bots.
            const bool provider_ready = login_method_.kind == LoginMethodKind::ProviderLtoken;
            const bool using_game_proxy = !provider_ready && proxy_.has_value();
            const Socks5Config* world_proxy =
                provider_ready && bypass_enet_ ? &*bypass_enet_
                                               : (proxy_ ? &*proxy_
                                                         : (bypass_enet_ ? &*bypass_enet_ : nullptr));
            log_console("[Bot] Redirecting to " + r.server + ":" + std::to_string(r.port) +
                        (world_proxy ? "" : " (no proxy - dead loopback, will fail)"));
            host_ = create_host(world_proxy);
            saw_server_hello_ = false;
            // This leg is the world/subserver connect. When a dedicated GAME proxy
            // carries it, a drop before ServerHello means THAT proxy can't reach the
            // world -> the world-join handler in on_disconnect will rotate it.
            on_subserver_connect_ = using_game_proxy;
            host_.connect(ep->addr(), ep->len, kHostChannelLimit, 0);
        } else {
            log_console("[Bot] Invalid redirect address '" + r.server + ":" +
                        std::to_string(r.port) + "' — dropping redirect, reconnecting to gateway");
            redirect_.reset();
            redirect_attempts_ = 0;
            redirect_connect_fails_ = 0;
            reconnect_main(false);  // warp redirect address bad: fall back to gateway re-login
        }
        return;
    }

    // (B) a delayed reconnect is already scheduled (e.g. a 2FA cooldown).
    if (reconnect_after_) return;

    // (C) auto-reconnect paths.
    if (auto_reconnect_) {
        bool refresh_token = refresh_token_on_reconnect_;
        refresh_token_on_reconnect_ = false;

        if (disconnected_before_server_hello && on_subserver_connect_ && proxy_) {
            // The dropped leg was a WORLD/subserver connect, carried by the GAME
            // proxy (not the bypass logon IP). Repeated drops here = this game proxy
            // can't reach the world ("error connecting"). Rotate the GAME proxy
            // instead of retrying the same unreachable exit forever. (Fix: rotating-
            // login bots looped on world-join, never switching the erroring proxy;
            // quarantine_logon_ip only swapped the bypass IP, never the game proxy.)
            on_subserver_connect_ = false;
            pre_hello_disconnects_ = 0;
            subserver_connect_fails_ += 1;
            log_console("[Bot] world-join: subserver connect dropped before ServerHello (" +
                        std::to_string(subserver_connect_fails_) + "/2)");
            if (subserver_connect_fails_ >= 2) {
                const std::string key = proxy_->host + ":" + std::to_string(proxy_->port);
                nxrth::proxy::quarantine_proxy(key);
                if (auto fresh = nxrth::proxy::next_game_proxy(&*proxy_)) {
                    proxy_ = fresh;
                    log_console("[Bot] world-join keeps failing via game proxy " + key +
                                " (error connecting) - rotating game proxy to " + fresh->host +
                                ":" + std::to_string(fresh->port));
                } else {
                    log_console("[Bot] world-join failing via game proxy " + key +
                                " - quarantined, but NO replacement game proxy available");
                }
                subserver_connect_fails_ = 0;
            }
            // Reuse the current token (still valid; the logon IP didn't change) — just
            // reconnect to the gateway and take the fresh world redirect via the new proxy.
            log_console("[Bot] Server disconnected — reconnecting with current session");
            schedule_reconnect("world-join connect dropped before ServerHello", false, 1500);
        } else if (disconnected_before_server_hello) {
            pre_hello_disconnects_ += 1;
            log_console("[Bot] disconnected before ServerHello (" +
                        std::to_string(pre_hello_disconnects_) + "/3)");
            if (pre_hello_disconnects_ >= 3) {
                // Couldn't even reach ServerHello after 3 tries = the classic
                // "error connecting": this exit IP is rate-limited by GT's game
                // servers (it can bypass-logon but can't play). Quarantine it +
                // switch to a fresh exit IP instead of retrying it forever.
                log_console(
                    "[Bot] no ServerHello after 3 tries - logon exit IP rate-limited (error "
                    "connecting); switching IP");
                quarantine_logon_ip("no ServerHello / error connecting");
                refresh_token = true;
                pre_hello_disconnects_ = 0;
            }
            log_console("[Bot] Server disconnected — reconnecting with current session");
            schedule_reconnect("Server disconnected before ServerHello", refresh_token, 1500);
        } else if (was_in_world_) {
            // An authenticated in-world session dropped — almost always a flaky
            // game/world proxy, NOT a logon rejection. Restart from scratch on a
            // fresh exit IP + token (force refresh); do NOT touch login_reject_streak.
            was_in_world_ = false;
            pre_hello_disconnects_ = 0;
            log_console(
                "[Bot] in-world session dropped (game proxy?) — restarting login from scratch on a "
                "fresh exit IP + token");
            schedule_reconnect("In-world session dropped", true, 1500);
        } else {
            // Reached ServerHello + sent 225 but the gateway dropped with no
            // redirect = silent rejection (exit IP rate-limited/flagged).
            pre_hello_disconnects_ = 0;
            login_reject_streak_ += 1;
            if (login_reject_streak_ >= 3) {
                // 3 silent rejections on this exit IP = it's rate-limited. Stop
                // hammering the same dead IP with ever-longer back-offs: quarantine
                // it fleet-wide + force a fresh exit IP (like the 403 handling, but
                // for the game connection).
                log_console("[Bot] logon rejected 3x (ServerHello, no redirect) - exit IP "
                            "rate-limited; quarantining + switching IP");
                quarantine_logon_ip("gateway rejected after ServerHello");
                login_reject_streak_ = 0;
                schedule_reconnect("logon IP rate-limited - switching exit IP", true, 1500);
            } else if (refresh_token || login_reject_streak_ == 1) {
                log_console("[Bot] Server disconnected — reconnecting with current session");
                schedule_reconnect("Server disconnected after ServerHello", refresh_token, 1500);
            } else {
                std::uint64_t secs =
                    std::clamp<std::uint64_t>(15 * std::min<std::uint32_t>(login_reject_streak_, 8),
                                              15, 120);
                reconnect_after_ = Clock::now() + std::chrono::seconds(secs);
                log_console("[Bot] logon rejected by gateway (" +
                            std::to_string(login_reject_streak_) +
                            "x, ServerHello but no redirect) — exit IP likely rate-limited; backing "
                            "off " +
                            std::to_string(secs) + "s before retry");
            }
        }
        return;
    }

    // (D) auto-reconnect disabled.
    log_console("[Bot] Server disconnected — auto-reconnect is disabled");
}

// ===========================================================================
// §2.9 wait_for_global_gate — connected-phase gate wait that KEEPS ENET SERVICED
// ===========================================================================
void Bot::wait_for_global_gate(Gate& gate, std::uint64_t spacing_ms, const char* label) {
    auto slot = reserve_gate_slot(gate, spacing_ms, GATE_CONNECTED_MAX_AHEAD_MS);
    auto now = Clock::now();
    if (slot <= now) return;

    auto waited_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(slot - now).count();
    if (waited_ms >= 50) {
        log_console("[Bot] login pacing: waiting ~" + std::to_string(waited_ms) + "ms before " +
                    label + " (keeping ENet serviced)");
    }

    // Re-entrant (a serviced wait's service_once re-entered another gate): plain sleep.
    if (in_gate_wait_) {
        std::this_thread::sleep_for(slot - now);
        return;
    }

    in_gate_wait_ = true;
    while (Clock::now() < slot) {
        if (stop_ && stop_->load()) break;
        service_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    in_gate_wait_ = false;
}

// ===========================================================================
// quarantine_logon_ip — a post-logon exit IP that can't bring the bot online
// (connect refused before ServerHello, or the gateway silently drops us after it)
// is rate-limited by GT's game servers. Set it aside fleet-wide + rotate. Mirrors
// the HTTP-403 quarantine, but for the game connection.
// ===========================================================================
void Bot::quarantine_logon_ip(const std::string& reason) {
    if (bypass_enet_) {
        const std::string key =
            bypass_enet_->host + ":" + std::to_string(bypass_enet_->port);
        nxrth::proxy::quarantine_proxy(key);
        log_console("[Bot] logon exit IP " + key + " rate-limited (" + reason +
                    ") - quarantined fleet-wide; re-login will pin a fresh bypass IP");
    } else if (proxy_) {
        const std::string key = proxy_->host + ":" + std::to_string(proxy_->port);
        nxrth::proxy::quarantine_proxy(key);
        if (auto fresh = nxrth::proxy::next_game_proxy(&*proxy_)) {
            proxy_ = fresh;
            log_console("[Bot] logon exit IP " + key + " rate-limited (" + reason +
                        ") - quarantined + rotated game proxy to " + fresh->host + ":" +
                        std::to_string(fresh->port));
        } else {
            log_console("[Bot] logon exit IP " + key + " rate-limited (" + reason +
                        ") - quarantined, but NO replacement game proxy available");
        }
    }
}

// ===========================================================================
// §2.13 reconnect_main — always reconnects to the gateway from the pinned IP
// ===========================================================================
void Bot::reconnect_main(bool refresh_token) {
    const auto logon_proxy = [this]() -> const Socks5Config* {
        return bypass_enet_ ? &*bypass_enet_ : (proxy_ ? &*proxy_ : nullptr);
    };

    on_subserver_connect_ = false;  // reconnect_main always connects to the logon gateway
    host_ = create_host(logon_proxy());
    peer_id_.reset();
    saw_server_hello_ = false;

    if (refresh_token) {
        server_addr_.reset();
        this->refresh_token();  // §2.14 — may repopulate server_addr_ + re-pin bypass_enet_
        if (server_addr_) {
            host_ = create_host(logon_proxy());
            saw_server_hello_ = false;
            log_console("[Bot] reconnect: connecting to refreshed " + ep_str(*server_addr_));
            host_.connect(server_addr_->addr(), server_addr_->len, kHostChannelLimit, 0);
            return;
        }
    } else {
        log_console("[Bot] reconnect: reusing current login token");
        if (server_addr_) {
            saw_server_hello_ = false;
            log_console("[Bot] reconnect: connecting to current " + ep_str(*server_addr_));
            host_.connect(server_addr_->addr(), server_addr_->len, kHostChannelLimit, 0);
            return;
        }
    }

    // Terminal-stop guard: refresh_token() sets stop_requested_ for terminal cases.
    if (stop_requested_ || (stop_ && stop_->load())) {
        log_console("[Bot] reconnect aborted — bot is stopping (no server address)");
        return;
    }

    // Full server_data re-fetch with the rich proxy-candidate list.
    struct Candidate {
        std::string label;
        std::optional<std::string> url;
    };
    std::vector<Candidate> candidates;
    if (login_proxy_) {
        std::string url = login_proxy_->fresh_url();
        std::optional<std::string> alt;
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
            alt = alternate_scheme_url(url);
        candidates.push_back({"rotating login proxy", url});
        if (alt) candidates.push_back({"rotating login proxy (alt scheme)", *alt});
        log_console(
            "[Bot] reconnect: rotating login proxy enabled; assigned game proxy fallback disabled");
    } else if (proxy_) {
        candidates.push_back({"assigned game proxy", socks5_to_url(*proxy_)});
    }
    if (candidates.empty()) candidates.push_back({"direct", std::nullopt});

    nxrth::net::LoginInfo login_info;
    login_info.protocol = consts::PROTOCOL;
    login_info.game_version = std::string(consts::GAME_VER);

    pace_http_login();

    for (;;) {
        if (stop_ && stop_->load()) {
            log_console("[Bot] reconnect aborted — bot stopped");
            return;
        }
        std::optional<ServerData> found;
        for (const auto& cand : candidates) {
            bool got = false;
            for (bool alternate : {false, true}) {
                log_console("[Bot] reconnect: fetching server_data (alternate=" +
                            std::string(alternate ? "true" : "false") + ", http_proxy=" +
                            cand.label + ")...");
                auto res = nxrth::net::get_server_data_proxied(alternate, login_info, cand.url);
                if (res.ok()) {
                    found = res.data;
                    got = true;
                    break;
                }
                log_console("[Bot] reconnect: server_data failed via " + cand.label + ": " +
                            res.error);
                if (is_http_403_text(res.error)) {
                    log_console("[Bot] reconnect: proxy returned 403; skipping growtopia2 "
                                "alternate for " +
                                cand.label);
                    break;  // skip the alternate for this candidate
                }
            }
            if (got) break;
        }

        if (found) {
            meta_ = found->meta;
            auto ep = resolve_endpoint(found->server, found->port);
            if (ep) {
                server_addr_ = ep;
                saw_server_hello_ = false;
                log_console("[Bot] reconnect: connecting to fetched " + ep_str(*server_addr_));
                host_.connect(server_addr_->addr(), server_addr_->len, kHostChannelLimit, 0);
            } else {
                log_console("[Bot] reconnect: invalid server address '" + found->server + ":" +
                            std::to_string(found->port) + "' — retrying in 10s");
                reconnect_after_ = Clock::now() + std::chrono::seconds(10);
            }
            return;
        }

        log_console("[Bot] reconnect: all server_data proxy candidates failed - retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

// ===========================================================================
// §2.15 schedule_reconnect — escalating backoff with jitter
// ===========================================================================
void Bot::schedule_reconnect(const std::string& reason, bool refresh_token,
                             std::uint64_t base_ms) {
    std::uint32_t streak = std::min<std::uint32_t>(
        std::max<std::uint32_t>(login_reject_streak_, pre_hello_disconnects_), 8);
    std::uint64_t backoff = base_ms * std::max<std::uint32_t>(streak, 1);
    std::uint64_t jitter =
        ((static_cast<std::uint64_t>(bot_id_) * 137) + (static_cast<std::uint64_t>(streak) * 251)) %
        1000;
    std::uint64_t delay_ms = std::min<std::uint64_t>(backoff + jitter, 30000);
    refresh_token_on_reconnect_ = refresh_token_on_reconnect_ || refresh_token;
    reconnect_after_ = Clock::now() + std::chrono::milliseconds(delay_ms);
    log_console("[Bot] " + reason + " - reconnecting in " + std::to_string(delay_ms) + "ms");
}

// ===========================================================================
// §2.14 refresh_token / apply_credentials / apply_login_identity
// ===========================================================================
void Bot::refresh_token() {
    LogFn log = [this](const std::string& m) { log_console(m); };
    switch (login_method_.kind) {
        case LoginMethodKind::Ltoken: {
            pace_http_login();
            std::optional<std::string> proxy_url;
            if (proxy_) proxy_url = socks5_to_url(*proxy_);
            auto checked = nxrth::login::check_token(ltoken_, build_login_data(), proxy_url);
            if (checked.ok()) {
                ltoken_ = std::move(*checked.token);
                log_console("[Bot] Google OAuth ltoken refreshed via checktoken");
            } else {
                const std::string reason =
                    checked.error ? checked.error->display() : "unknown error";
                log_console("[Bot] Google OAuth ltoken refresh failed: " + reason +
                            " - stopping bot");
                stop_requested_ = true;
            }
            break;
        }
        case LoginMethodKind::ProviderLtoken: {
            pace_http_login();
            auto refreshed = fetch_ltoken_server_data(proxy_, login_proxy_, false, *stop_, log);
            if (!refreshed) {
                stop_requested_ = true;
                break;
            }
            auto endpoint = resolve_endpoint(refreshed->data.server, refreshed->data.port);
            if (!endpoint) {
                log_console("[Bot] provider token retry: invalid server address - stopping bot");
                stop_requested_ = true;
                break;
            }
            meta_ = refreshed->data.meta;
            // Re-exchange the ORIGINAL refresh token for a fresh session ltoken bound to
            // the new server_data session (the prior session ltoken is single-session).
            const std::string client_data = build_checktoken_client_data(
                login_identity_view(), meta_, provider_login_protocol(), /*oauth=*/true);
            auto checked = nxrth::login::check_token(login_method_.source_token, client_data,
                                                      refreshed->http_proxy_url);
            if (!checked.ok()) {
                const std::string reason =
                    checked.error ? checked.error->display() : "unknown error";
                log_console("[Bot] provider ltoken re-validation failed: " + reason +
                            " - stopping bot");
                stop_requested_ = true;
                break;
            }
            ltoken_ = std::move(*checked.token);
            server_addr_ = std::move(endpoint);
            bypass_enet_ = std::move(refreshed->provider_enet);  // empty in game-proxy mode
            log_console("[Bot] provider ltoken re-validated via checktoken on a fresh session");
            break;
        }
        case LoginMethodKind::Legacy: {
            log_console("[Bot] falling back to full re-login");
            pace_http_login();
            auto creds = nxrth::login::fetch_credentials(username_, login_method_.password, proxy_,
                                                          login_proxy_, *stop_, log);
            if (creds)
                apply_credentials(*creds);
            else
                stop_requested_ = true;
            break;
        }
    }
}

void Bot::apply_credentials(const Credentials& creds) {
    ltoken_ = creds.ltoken;
    meta_ = creds.meta;
    bypass_enet_ = creds.bypass_enet;  // re-pin the logon IP to the new token's IP
    server_addr_ = resolve_endpoint(creds.server, creds.port);
    redirect_.reset();
    redirect_attempts_ = 0;
    last_redirect_token_.reset();
    last_redirect_uuid_.reset();
    if (creds.identity) apply_login_identity(*creds.identity);
}

void Bot::apply_login_identity(const LoginIdentity& identity) {
    LoginIdentity r = resolve_login_identity(std::optional<LoginIdentity>(identity));
    game_version_ = r.game_version;
    cbits_ = r.cbits;
    player_age_ = r.player_age;
    gdpr_ = r.gdpr;
    category_ = r.category;
    total_playtime_ = r.total_playtime;
    country_ = r.country;
    zf_ = r.zf;
    rid_ = r.rid;
    mac_ = r.mac;
    wk_ = r.wk;
    hash_ = r.hash;
    hash2_ = r.hash2;
    fz_ = r.fz;
    platform_id_ = r.platform_id;
    steam_token_ = r.steam_token;
    klv_ = r.klv;
    vid_ = r.vid;
    if (state_) state_->write([&](BotState& s) { s.mac = mac_; });
}

// ===========================================================================
// §2.6 fetch_server_data_loop (member) — used by reconnect paths that want the
// simple ltoken/har-style loop.
// ===========================================================================
std::optional<ServerData> Bot::fetch_server_data_loop() {
    LogFn log = [this](const std::string& m) { log_console(m); };
    return fetch_server_data_free(proxy_, login_proxy_, *stop_, log);
}

// ===========================================================================
// Native fleet-aware automation seam (replaces Nxrth's Lua script channel).
// ===========================================================================
void Bot::add_automation_module(std::unique_ptr<AutomationModule> mod) {
    if (mod) automation_modules_.push_back(std::move(mod));
}

void Bot::run_automation(FleetState& fleet) {
    if (automation_modules_.empty()) return;
    const auto cfg = fleet.config_handle();
    for (auto& mod : automation_modules_) {
        const std::string module_name = mod->name();
        const bool enabled = cfg->is_on_for(module_name, bot_id_);
        const bool was_enabled = automation_module_enabled_[module_name];
        if (enabled && !was_enabled) mod->on_enabled(*this, fleet);
        if (!enabled && was_enabled) mod->on_disabled(*this, fleet);
        automation_module_enabled_[module_name] = enabled;
        if (enabled) mod->tick(*this, fleet, *cfg);
    }
}

void Bot::publish_fleet_view() {
    if (!fleet_) return;
    BotView v;
    v.id = bot_id_;
    if (state_) {
        state_->read([&](const BotState& s) {
            v.username = s.username;
            v.status = s.status;
            v.world_name = s.world_name;
            v.pos_x = s.pos_x;
            v.pos_y = s.pos_y;
            v.gems = s.gems;
            v.ping_ms = s.ping_ms;
            return 0;
        });
    }
    if (proxy_) v.proxy_key = proxy_->host + ":" + std::to_string(proxy_->port);
    fleet_->upsert(v);
}

}  // namespace nxrth::bot
