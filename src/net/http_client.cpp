#include "net/http_client.h"

#include <curl/curl.h>

#include <cctype>
#include <mutex>
#include <utility>

namespace nxrth::net {

namespace {

// curl_global_init is required once per process before any easy handle is used.
// Thread-safe one-shot; global cleanup is intentionally skipped (process-lifetime
// resources reclaimed at exit — matches how a long-lived bot fleet runs).
void ensure_global_init() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t write_body(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t n = size * nmemb;
    static_cast<std::string*>(userdata)->append(ptr, n);
    return n;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back())) s.remove_suffix(1);
    return s;
}

// Collects response headers. libcurl calls this once per header line, including
// the "HTTP/x yyy" status line (fires again for each redirect hop) and the final
// blank line. On a new status line we clear, so we keep only the last response's
// headers after redirects are followed.
std::size_t write_header(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
    const std::size_t len = size * nitems;
    auto* headers = static_cast<std::vector<HttpHeader>*>(userdata);
    std::string_view line(buffer, len);

    if (line.rfind("HTTP/", 0) == 0) {
        headers->clear();
        return len;
    }
    const auto colon = line.find(':');
    if (colon != std::string_view::npos) {
        std::string_view name = trim(line.substr(0, colon));
        std::string_view value = trim(line.substr(colon + 1));
        if (!name.empty()) {
            headers->push_back({std::string(name), std::string(value)});
        }
    }
    return len;
}

}  // namespace

std::optional<std::string> HttpResponse::header(std::string_view name) const {
    std::optional<std::string> found;
    for (const auto& h : headers) {
        if (iequals(h.name, name)) found = h.value;  // last wins
    }
    return found;
}

HttpClient::HttpClient() {
    ensure_global_init();
    handle_ = curl_easy_init();
}

HttpClient::~HttpClient() {
    if (handle_) curl_easy_cleanup(static_cast<CURL*>(handle_));
}

HttpClient::HttpClient(HttpClient&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

HttpClient& HttpClient::operator=(HttpClient&& other) noexcept {
    if (this != &other) {
        if (handle_) curl_easy_cleanup(static_cast<CURL*>(handle_));
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

HttpResponse HttpClient::Request(const HttpRequest& req) {
    HttpResponse resp;
    CURL* curl = static_cast<CURL*>(handle_);
    if (!curl) {
        resp.error = "curl handle not initialized";
        return resp;
    }

    // Reset per-request options. curl_easy_reset PRESERVES the in-memory cookie
    // store (and DNS/connection caches), so cookies persist across requests on
    // this client while every request starts from a clean option slate.
    curl_easy_reset(curl);

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // safe timeouts on worker threads
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, req.timeout_secs);
    if (req.connect_timeout_secs > 0) {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, req.connect_timeout_secs);
    }

    // Enable the cookie engine with no file to load ("" = start empty). The store
    // already held in this handle survives the reset above, so Set-Cookie values
    // from earlier requests are replayed automatically.
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");

    // TLS verification toggle (server_data.php fetch disables it — proxies MITM TLS).
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, req.verify_tls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, req.verify_tls ? 2L : 0L);

    // Redirects. Dashboard uses follow_redirects=false so the caller can read the
    // Location header and drive the redirect chain by hand.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.follow_redirects ? 1L : 0L);
    if (req.follow_redirects && req.max_redirects >= 0) {
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, req.max_redirects);
    }

    // Proxy: the scheme in the URL selects socks5h/socks5/http. socks5h resolves
    // DNS proxy-side (required so the ltoken binds to the proxy's egress IP).
    if (!req.proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, req.proxy.c_str());
    }

    if (!req.user_agent.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, req.user_agent.c_str());
    }

    // Method + body.
    if (!req.custom_method.empty()) {
        // Custom verb (PATCH/PUT/DELETE) carrying a body, e.g. a Discord webhook
        // message edit. Send the body like a POST, then override the verb string.
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(req.body.size()));
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, req.body.data());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.custom_method.c_str());
    } else if (req.method == HttpMethod::Post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        // Set size first, then COPYPOSTFIELDS so curl duplicates the exact bytes
        // (handles empty/binary-safe bodies without relying on strlen).
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(req.body.size()));
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, req.body.data());
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    // Custom headers. Suppress libcurl's auto "Expect: 100-continue" (which it
    // adds for POST bodies > 1KB, e.g. Requestly HAR replays) unless the caller
    // set Expect explicitly — GT's PHP endpoints and the Rust ureq path never use
    // it, and some servers stall waiting on the 100 response.
    bool has_expect = false;
    for (const auto& h : req.headers) {
        if (iequals(h.name, "Expect")) has_expect = true;
    }
    struct curl_slist* header_list = nullptr;
    for (const auto& h : req.headers) {
        const std::string line = h.name + ": " + h.value;
        header_list = curl_slist_append(header_list, line.c_str());
    }
    if (!has_expect) {
        header_list = curl_slist_append(header_list, "Expect:");
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // Sinks.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);

    const CURLcode code = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    resp.status = status;
    if (code != CURLE_OK) {
        resp.error = (errbuf[0] != '\0') ? errbuf : curl_easy_strerror(code);
    }

    if (header_list) curl_slist_free_all(header_list);
    return resp;
}

HttpResponse HttpClient::Get(const std::string& url, HttpRequest opts) {
    opts.method = HttpMethod::Get;
    opts.url = url;
    return Request(opts);
}

HttpResponse HttpClient::Post(const std::string& url, std::string body, HttpRequest opts) {
    opts.method = HttpMethod::Post;
    opts.url = url;
    opts.body = std::move(body);
    return Request(opts);
}

}  // namespace nxrth::net
