// Adonai — reusable libcurl HTTP client (the single HTTP primitive).
//
// This wraps one persistent libcurl easy handle and is the transport used by:
//   * server_data   — POST server_data.php (TLS-verify OFF, 20s timeout)
//   * login         — GET the GrowID login page, POST growid/login/validate,
//                     POST checktoken (cookies flow across the session)
//   * dashboard     — POST /player/login/dashboard with FOLLOWLOCATION off, then
//                     read the Location response header + follow it manually
//   * proxy_test    — a plain GET/POST through a candidate proxy
//
// Feature set (per port specs 03-net-socks5 §2.9 and 05-login §4.1/§4.2):
//   * GET / POST                        (HttpMethod)
//   * optional proxy URL                (socks5h:// / socks5:// / http://)
//   * per-session cookie jar            (in-memory, survives across requests)
//   * custom headers + User-Agent       (Content-Type, Origin, Referer, ...)
//   * form body                         (application/x-www-form-urlencoded, etc.)
//   * configurable global timeout       (whole seconds)
//   * TLS-verify toggle                 (server_data disables verification)
//   * redirect-follow toggle            (dashboard needs max_redirects(0))
//
// Returns { status, body, error } plus the final-response headers (needed for the
// dashboard redirect loop's Location lookup).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adonai::net {

enum class HttpMethod { Get, Post };

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string url;
    std::string body;  // form body (POST only); ignored for GET

    // Sets CURLOPT_USERAGENT when non-empty. GT rejects requests without the
    // exact UA it expects, so callers pass the verbatim Growtopia/browser UA.
    std::string user_agent;

    // Extra request headers (e.g. {"Content-Type","application/x-www-form-urlencoded"},
    // {"Origin","https://..."}, {"Referer","..."}). Sent verbatim, in order.
    std::vector<HttpHeader> headers;

    // Proxy URL; the scheme selects the type. Use "socks5h://user:pass@host:port"
    // for SOCKS proxies (remote DNS — matches the ltoken IP-binding requirement),
    // "http://host:port" for HTTP proxies. Empty = direct connection.
    std::string proxy;

    long timeout_secs = 20;         // CURLOPT_TIMEOUT (connect+transfer, whole seconds)
    long connect_timeout_secs = 0;  // 0 = curl default; else CURLOPT_CONNECTTIMEOUT

    bool verify_tls = true;         // false => SSL_VERIFYPEER/HOST off (server_data)
    bool follow_redirects = true;   // false => FOLLOWLOCATION 0 (dashboard follows manually)
    long max_redirects = -1;        // when following: cap; -1 = unlimited

    // When non-empty, overrides the HTTP verb via CURLOPT_CUSTOMREQUEST (e.g.
    // "PATCH" to edit a Discord webhook message). The body is still sent.
    std::string custom_method;
};

struct HttpResponse {
    long status = 0;      // HTTP status code; 0 if the transport failed before a response
    std::string body;     // response body
    std::string error;    // empty on success; curl error message otherwise

    // Final-response headers (after any followed redirects). Used by the dashboard
    // loop to read Location. Intermediate redirect hops' headers are discarded.
    std::vector<HttpHeader> headers;

    bool ok() const noexcept { return error.empty(); }

    // Case-insensitive header lookup; returns the last matching value (nullopt if absent).
    std::optional<std::string> header(std::string_view name) const;
};

// One HTTP session backed by a persistent libcurl easy handle. Cookies set by one
// request are replayed on later requests on the same client (dashboard -> growid
// pages depend on this), mirroring the Rust per-bot ureq::Agent.
//
// NOT thread-safe: a libcurl easy handle must not be driven concurrently. Use one
// HttpClient per worker thread / per bot. Move-only.
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) noexcept;
    HttpClient& operator=(HttpClient&&) noexcept;

    // Core entry point. Never throws for HTTP/transport failures — those surface
    // via HttpResponse::error (and status, which may be 0).
    HttpResponse Request(const HttpRequest& req);

    // Convenience wrappers. `opts` carries headers/UA/proxy/timeout/TLS/redirect
    // options; its method/url/body fields are overwritten by the call.
    HttpResponse Get(const std::string& url, HttpRequest opts = {});
    HttpResponse Post(const std::string& url, std::string body, HttpRequest opts = {});

private:
    void* handle_;  // CURL* (opaque here to keep <curl/curl.h> out of the header)
};

}  // namespace adonai::net
