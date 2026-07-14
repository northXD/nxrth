// Adonai — GrowID dashboard POST + redirect-follow parsing.
// Ported from Mori/dashboard.rs (see docs/port-specs/05-login.md §4.2).
//
// A dashboard POST hits https://{login_url}/player/login/dashboard, then the
// login page bounces through up to 4 manual redirects (the agent is built with
// FOLLOWLOCATION off) before the final HTML carries the GrowID/Apple/Google
// links. Only the Growtopia href is consumed by the login flow.
//
// The NEWLY path (get_newly_dashboard_proxied) is the current 5.51 flow and
// sends EXACTLY 22 fields with platformID=15,1,0. It MUST NOT send fz/hash2/zf/
// steamToken — the 5.51 dashboard returns HTTP 500 if they are present.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "net/server_data.h"  // adonai::net::LoginInfo

namespace adonai::login {

// The three account-provider hrefs scraped from the dashboard HTML. Only
// `growtopia` is consumed by login (the href to the GrowID login page).
struct DashboardLinks {
    std::optional<std::string> apple;
    std::optional<std::string> google;
    std::optional<std::string> growtopia;
};

// Optional per-request identity overriding the newly-dashboard defaults. In the
// live flow this is std::nullopt (the canonical 22-field POST uses the fixed
// defaults; the per-account identity is reserved for the ENet packet). fz/hash2/
// zf/steam_token are carried for struct fidelity but are NEVER sent (§4.2).
struct NewlyDashboardIdentity {
    std::string cbits;
    std::string player_age;
    std::string gdpr;
    std::string category;
    std::string total_playtime;
    std::string country;
    std::string fz;            // present, NOT sent
    std::string rid;
    std::string mac;
    std::string wk;
    std::string hash;
    std::string hash2;         // present, NOT sent
    std::string zf;            // present, NOT sent
    std::string platform_id;
    std::string steam_token;   // present, NOT sent
    std::string klv;
};

// Result of a dashboard POST + parse. `error` is empty on success.
struct DashboardResult {
    DashboardLinks links;
    std::string error;
    bool ok() const noexcept { return error.empty(); }
};

// NEWLY (current 5.51) dashboard POST. EXACTLY 22 fields, platformID=15,1,0,
// Origin header set. `identity` std::nullopt => all canonical defaults.
DashboardResult get_newly_dashboard_proxied(const std::string& login_url,
                                            const adonai::net::LoginInfo& login_info,
                                            const std::string& meta,
                                            const std::optional<std::string>& proxy_url,
                                            const std::optional<NewlyDashboardIdentity>& identity);

// LEGACY dashboard POST (classic GrowID). 24 fields, platformID=0,1,1, includes
// fz+hash2. Kept for the legacy login mode.
DashboardResult get_dashboard_proxied(const std::string& login_url,
                                      const adonai::net::LoginInfo& login_info,
                                      const std::string& meta,
                                      const std::optional<std::string>& proxy_url);

// --- helpers exposed for reuse / testing -------------------------------------

// "{k}|{v}\n" concatenated in order; values are NOT URL-encoded (raw pipe body).
std::string build_pipe_body(const std::vector<std::pair<std::string, std::string>>& fields);

// Absolute href passthrough; otherwise resolve against login_url's host
// (LOGIN_HOST fallback), prefixing "https://{host}".
std::string normalize_login_href(const std::string& href, const std::string& login_url);

// Parse the final dashboard HTML into DashboardLinks; JSON body => error.
DashboardResult parse_dashboard_response(const std::string& html, const std::string& login_url);

}  // namespace adonai::login
