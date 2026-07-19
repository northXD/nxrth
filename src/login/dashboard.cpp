// Nxrth — GrowID dashboard POST implementation (see dashboard.h / §4.2).
#include "login/dashboard.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/constants.h"
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

// UA sent to the legacy dashboard + GrowID pages (wire-fixed, do not rename).
constexpr const char* kMacUA =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)";
constexpr const char* kValKey = "40db4045f2d8c572efe8c4a060605726";
constexpr const char* kLoginHost = "login.growtopiagame.com";

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

// Default klv = compute_klv(GAME_VER, "226", DEFAULT_RID, DEFAULT_HASH_i32).
std::string default_klv(const LoginInfo& login_info) {
    int hash_i32 = 0;
    try {
        hash_i32 = std::stoi(std::string(consts::DEFAULT_HASH));
    } catch (...) {
        hash_i32 = 0;
    }
    return nxrth::protocol::compute_klv(login_info.game_version,
                                         std::to_string(login_info.protocol),
                                         std::string(consts::DEFAULT_RID), hash_i32);
}

// Read attribute `attr` from a single tag substring (e.g. "<a href=... onclick=...>").
// Handles both single- and double-quoted values; returns "" if absent.
std::string tag_attr(const std::string& tag, const std::string& attr) {
    std::string low = to_lower(tag);
    std::string key = to_lower(attr);
    std::size_t search = 0;
    while (true) {
        std::size_t p = low.find(key, search);
        if (p == std::string::npos) return "";
        // Ensure it's an attribute name (preceded by space/'<') and followed by '='.
        if (p > 0 && !std::isspace(static_cast<unsigned char>(low[p - 1])) && low[p - 1] != '<') {
            search = p + key.size();
            continue;
        }
        std::size_t q = p + key.size();
        while (q < low.size() && std::isspace(static_cast<unsigned char>(low[q]))) ++q;
        if (q >= low.size() || low[q] != '=') {
            search = p + key.size();
            continue;
        }
        ++q;
        while (q < low.size() && std::isspace(static_cast<unsigned char>(low[q]))) ++q;
        if (q >= tag.size()) return "";
        char quote = tag[q];
        if (quote == '"' || quote == '\'') {
            std::size_t end = tag.find(quote, q + 1);
            if (end == std::string::npos) return "";
            return tag.substr(q + 1, end - (q + 1));
        }
        // Unquoted: read until whitespace or '>'.
        std::size_t end = q;
        while (end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[end])) &&
               tag[end] != '>')
            ++end;
        return tag.substr(q, end - q);
    }
}

// Find a <meta http-equiv="refresh" content="...url=..."> target, else the first
// <a href> pointing at /steam/redirect or /player/login/dashboard.
std::optional<std::string> extract_html_redirect(const std::string& html,
                                                 const std::string& login_url) {
    std::string low = to_lower(html);
    // 1) meta refresh
    std::size_t pos = 0;
    while (true) {
        std::size_t m = low.find("<meta", pos);
        if (m == std::string::npos) break;
        std::size_t end = html.find('>', m);
        if (end == std::string::npos) break;
        std::string tag = html.substr(m, end - m + 1);
        if (ci_contains(tag_attr(tag, "http-equiv"), "refresh")) {
            std::string content = tag_attr(tag, "content");
            std::string clow = to_lower(content);
            std::size_t u = clow.find("url=");
            if (u != std::string::npos) {
                std::string rest = trim(content.substr(u + 4));
                if (!rest.empty() && (rest.front() == '\'' || rest.front() == '"'))
                    rest.erase(rest.begin());
                if (!rest.empty() && (rest.back() == '\'' || rest.back() == '"')) rest.pop_back();
                rest = trim(rest);
                if (!rest.empty()) return normalize_login_href(rest, login_url);
            }
        }
        pos = end + 1;
    }
    // 2) first anchor to /steam/redirect or /player/login/dashboard
    pos = 0;
    while (true) {
        std::size_t a = low.find("<a", pos);
        if (a == std::string::npos) break;
        std::size_t end = html.find('>', a);
        if (end == std::string::npos) break;
        std::string tag = html.substr(a, end - a + 1);
        std::string href = tag_attr(tag, "href");
        if (href.find("/steam/redirect") != std::string::npos ||
            href.find("/player/login/dashboard") != std::string::npos) {
            return normalize_login_href(href, login_url);
        }
        pos = end + 1;
    }
    return std::nullopt;
}

// Build the FOLLOWLOCATION-off, 30s dashboard agent request template.
HttpRequest dashboard_opts(const std::optional<std::string>& proxy_url) {
    HttpRequest opts;
    opts.user_agent = kMacUA;
    opts.timeout_secs = 30;
    opts.follow_redirects = false;
    if (proxy_url) opts.proxy = *proxy_url;
    return opts;
}

// One step of the manual redirect loop: derive the next URL from a response.
std::optional<std::string> derive_next(const HttpResponse& resp, const std::string& login_url) {
    if (auto loc = resp.header("Location")) {
        if (!loc->empty()) return normalize_login_href(*loc, login_url);
    }
    return extract_html_redirect(resp.body, login_url);
}

// Run the POST + up-to-4 manual redirect hops, then parse.
DashboardResult post_and_follow(const std::string& url, const std::string& body,
                                const std::string& login_url,
                                std::vector<HttpHeader> headers,
                                const std::optional<std::string>& proxy_url) {
    HttpClient client;
    HttpRequest opts = dashboard_opts(proxy_url);
    opts.headers = std::move(headers);

    HttpResponse resp = client.Post(url, body, opts);
    if (!resp.ok()) return {{}, resp.error};
    std::string html = resp.body;
    std::optional<std::string> next = derive_next(resp, login_url);

    for (int hop = 0; hop < 4 && next; ++hop) {
        HttpRequest gopts = dashboard_opts(proxy_url);
        resp = client.Get(*next, gopts);
        if (!resp.ok()) break;
        html = resp.body;
        next = derive_next(resp, login_url);
    }
    return parse_dashboard_response(html, login_url);
}

}  // namespace

std::string build_pipe_body(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& [k, v] : fields) {
        out += k;
        out += '|';
        out += v;
        out += '\n';
    }
    return out;
}

std::string normalize_login_href(const std::string& href, const std::string& login_url) {
    if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) return href;
    std::string host = login_url;
    if (host.rfind("https://", 0) == 0)
        host = host.substr(8);
    else if (host.rfind("http://", 0) == 0)
        host = host.substr(7);
    while (!host.empty() && host.back() == '/') host.pop_back();
    if (host.empty()) host = kLoginHost;
    if (!href.empty() && href.front() == '/') return "https://" + host + href;
    return "https://" + host + "/" + href;
}

DashboardResult parse_dashboard_response(const std::string& html, const std::string& login_url) {
    std::string trimmed = html;
    {
        std::size_t a = 0;
        while (a < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[a]))) ++a;
        trimmed = trimmed.substr(a);
    }
    if (!trimmed.empty() && trimmed.front() == '{') {
        std::string msg = trimmed;
        try {
            auto j = nlohmann::json::parse(trimmed);
            if (j.contains("message") && j["message"].is_string())
                msg = j["message"].get<std::string>();
        } catch (...) {
        }
        return {{}, "Dashboard returned error: " + msg};
    }

    DashboardLinks links;
    std::string low = to_lower(html);
    std::size_t pos = 0;
    while (true) {
        std::size_t a = low.find("<a", pos);
        if (a == std::string::npos) break;
        std::size_t end = html.find('>', a);
        if (end == std::string::npos) break;
        std::string tag = html.substr(a, end - a + 1);
        std::string onclick = tag_attr(tag, "onclick");
        if (!onclick.empty()) {
            std::string href = normalize_login_href(tag_attr(tag, "href"), login_url);
            if (onclick.find("optionChose('Apple')") != std::string::npos)
                links.apple = href;
            else if (onclick.find("optionChose('Google')") != std::string::npos)
                links.google = href;
            else if (onclick.find("optionChose('Grow')") != std::string::npos)
                links.growtopia = href;
        }
        pos = end + 1;
    }
    return {links, ""};
}

DashboardResult get_dashboard_proxied(const std::string& login_url, const LoginInfo& login_info,
                                      const std::string& meta,
                                      const std::optional<std::string>& proxy_url) {
    std::string klv = default_klv(login_info);
    // Legacy: 24 fields, platformID=0,1,1, includes fz+hash2 (column-major order).
    std::vector<std::pair<std::string, std::string>> fields = {
        {"tankIDName", ""},
        {"tankIDPass", ""},
        {"player_age", "25"},
        {"totalPlaytime", "0"},
        {"fhash", std::to_string(consts::FHASH)},
        {"country", "us"},
        {"requestedName", ""},
        {"f", "1"},
        {"GDPR", "1"},
        {"klv", klv},
        {"rid", std::string(consts::DEFAULT_RID)},
        {"hash", std::string(consts::DEFAULT_HASH)},
        {"protocol", std::to_string(login_info.protocol)},
        {"game_version", login_info.game_version},
        {"FCMToken", ""},
        {"hash2", std::string(consts::DEFAULT_HASH2)},
        {"platformID", "0,1,1"},
        {"mac", std::string(consts::DEFAULT_MAC)},
        {"fz", "31631978"},
        {"cbits", "0"},
        {"category", "_-5100"},
        {"meta", meta},
        {"deviceVersion", "0"},
        {"wk", std::string(consts::DEFAULT_WK)},
    };

    std::string url = "https://" + login_url + "/player/login/dashboard?valKey=" + kValKey;
    std::vector<HttpHeader> headers = {
        {"Content-Type", "application/x-www-form-urlencoded"},
    };
    return post_and_follow(url, build_pipe_body(fields), login_url, std::move(headers), proxy_url);
}

}  // namespace nxrth::login
