// Nxrth — GrowID dashboard POST + redirect-follow parsing.
// Ported from Mori/dashboard.rs (see docs/port-specs/05-login.md §4.2).
//
// A dashboard POST hits https://{login_url}/player/login/dashboard, then the
// login page bounces through up to 4 manual redirects (the agent is built with
// FOLLOWLOCATION off) before the final HTML carries the GrowID/Apple/Google
// links. Only the Growtopia href is consumed by the login flow.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "net/server_data.h"  // nxrth::net::LoginInfo

namespace nxrth::login {

// The three account-provider hrefs scraped from the dashboard HTML. Only
// `growtopia` is consumed by login (the href to the GrowID login page).
struct DashboardLinks {
    std::optional<std::string> apple;
    std::optional<std::string> google;
    std::optional<std::string> growtopia;
};

// Result of a dashboard POST + parse. `error` is empty on success.
struct DashboardResult {
    DashboardLinks links;
    std::string error;
    bool ok() const noexcept { return error.empty(); }
};

// LEGACY dashboard POST (classic GrowID). 24 fields, platformID=0,1,1, includes
// fz+hash2. Kept for the legacy login mode.
DashboardResult get_dashboard_proxied(const std::string& login_url,
                                      const nxrth::net::LoginInfo& login_info,
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

}  // namespace nxrth::login
