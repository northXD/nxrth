#include "ai/ai_controller.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace nxrth::ai {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxResponseBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxRequestBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxToolResultBytes = 512u * 1024u;
constexpr std::size_t kMaxTranscriptEntries = 500;
constexpr std::uint32_t kMaxRounds = 12;
constexpr std::uint32_t kMaxToolCalls = 32;
constexpr long kHttpTimeoutMs = 120'000;

struct ToolCall {
    std::string id;
    std::string name;
    json arguments = json::object();
};

struct AssistantTurn {
    std::string text;
    std::vector<ToolCall> calls;
};

struct ParsedResponse {
    bool ok = false;
    AssistantTurn turn;
    std::string error;
};

struct HttpOutcome {
    bool ok = false;
    bool cancelled = false;
    AssistantTurn turn;
    std::string error;
};

enum class InternalRole { User, Assistant, Tool };

struct InternalMessage {
    InternalRole role = InternalRole::User;
    std::string text;
    std::vector<ToolCall> calls;
    std::string tool_call_id;
    std::string tool_name;
    bool tool_error = false;
};

std::string lower_ascii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

bool is_sensitive_key(std::string_view key) {
    const std::string k = lower_ascii(key);
    static constexpr std::array<std::string_view, 34> exact = {
        "token",       "ltoken",       "refresh_token", "refreshtoken",
        "access_token", "password",     "pass",          "api_key",
        "apikey",      "authorization", "x-api-key",     "proxy",
        "proxy_url",   "proxy_password", "secret",        "otp",
        "wk",          "login_record", "tankidpass",    "tankidname",
        "ubiticket",   "uuidtoken",    "mac",           "rid",
        "vid",         "aid",           "webhook",      "webhook_url",
        "tokens",      "accounts",      "credentials",  "login_records",
        "account_data", "accounts_data",
    };
    if (std::find(exact.begin(), exact.end(), k) != exact.end()) return true;
    return k.find("token") != std::string::npos ||
           k.find("password") != std::string::npos ||
           k.find("credential") != std::string::npos ||
           k.find("ticket") != std::string::npos ||
           k.find("proxy") != std::string::npos ||
           k.find("webhook") != std::string::npos;
}

bool secret_run_char(unsigned char c) {
    return std::isalnum(c) || c == '+' || c == '/' || c == '=' || c == '_' || c == '-';
}

void replace_long_secret_runs(std::string& text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (!secret_run_char(static_cast<unsigned char>(text[pos]))) {
            ++pos;
            continue;
        }
        const std::size_t begin = pos;
        bool has_digit = false;
        bool has_symbol = false;
        while (pos < text.size() && secret_run_char(static_cast<unsigned char>(text[pos]))) {
            const unsigned char c = static_cast<unsigned char>(text[pos]);
            has_digit = has_digit || std::isdigit(c);
            has_symbol = has_symbol || c == '+' || c == '/' || c == '=' || c == '_' || c == '-';
            ++pos;
        }
        // Long provider tokens are usually base64/base64url. Also catch long
        // opaque alphanumeric blobs, while leaving ordinary prose untouched.
        if (pos - begin >= 80 || (pos - begin >= 32 && has_digit && has_symbol)) {
            text.replace(begin, pos - begin, "<redacted>");
            pos = begin + std::string_view("<redacted>").size();
        }
    }
}

void redact_url_credentials(std::string& text) {
    std::size_t scan = 0;
    while ((scan = text.find("://", scan)) != std::string::npos) {
        const std::size_t authority = scan + 3;
        const std::size_t end = text.find_first_of("/\\ \t\r\n", authority);
        const std::size_t at = text.find('@', authority);
        if (at != std::string::npos && (end == std::string::npos || at < end)) {
            text.replace(authority, at - authority, "<redacted>");
            scan = authority + std::string_view("<redacted>@").size();
        } else {
            scan = authority;
        }
    }
}

void redact_provider_key_prefixes(std::string& text) {
    const std::string lower = lower_ascii(text);
    std::string out;
    out.reserve(text.size());
    std::size_t copied = 0;
    std::size_t scan = 0;
    while ((scan = lower.find("sk-", scan)) != std::string::npos) {
        std::size_t end = scan + 3;
        while (end < text.size() && secret_run_char(static_cast<unsigned char>(text[end]))) ++end;
        if (end - scan >= 16) {
            out.append(text, copied, scan - copied);
            out += "<redacted>";
            copied = end;
        }
        scan = end;
    }
    if (copied != 0) {
        out.append(text, copied, std::string::npos);
        text = std::move(out);
    }
}

bool label_boundary(char c) {
    return !std::isalnum(static_cast<unsigned char>(c)) && c != '_';
}

void redact_labeled_values(std::string& text) {
    static constexpr std::array<std::string_view, 40> labels = {
        "refresh_token", "refreshtoken", "access_token", "proxy_password",
        "authorization", "password",     "x-api-key",    "api_key",
        "api key",       "apikey",       "ltoken",       "token",        "proxy",
        "secret",        "otp",          "wk",           "login_record",
        "logintoken",    "accesstoken",  "steamtoken",   "uuidtoken",
        "ubiticket",     "tankidpass",   "tankidname",   "proxyurl",
        "proxypassword", "mac",          "rid",          "vid",
        "aid",           "webhook",      "webhookurl",   "webhook_url",
        "geiger_webhook_url", "tokens",  "accounts",     "credentials",
        "login_records", "account_data", "accounts_data",
    };

    std::string lower = lower_ascii(text);
    std::size_t scan = 0;
    while (scan < text.size()) {
        std::size_t best = std::string::npos;
        std::string_view label;
        for (const auto candidate : labels) {
            std::size_t found = lower.find(candidate, scan);
            while (found != std::string::npos &&
                   ((found > 0 && !label_boundary(lower[found - 1])) ||
                    (found + candidate.size() < lower.size() &&
                     !label_boundary(lower[found + candidate.size()])))) {
                found = lower.find(candidate, found + 1);
            }
            if (found < best) {
                best = found;
                label = candidate;
            }
        }
        if (best == std::string::npos) break;

        std::size_t cursor = best + label.size();
        while (cursor < text.size() &&
               (text[cursor] == ' ' || text[cursor] == '\t' || text[cursor] == '"' ||
                text[cursor] == '\'')) {
            ++cursor;
        }
        if (cursor >= text.size() ||
            (text[cursor] != ':' && text[cursor] != '=' && text[cursor] != '|')) {
            scan = best + label.size();
            continue;
        }
        ++cursor;
        while (cursor < text.size() &&
               (text[cursor] == ' ' || text[cursor] == '\t' || text[cursor] == '"' ||
                text[cursor] == '\'')) {
            ++cursor;
        }
        const std::size_t value_begin = cursor;
        while (cursor < text.size() && text[cursor] != '\r' && text[cursor] != '\n' &&
               text[cursor] != '|' && text[cursor] != ',' && text[cursor] != '"' &&
               text[cursor] != '\'') {
            ++cursor;
        }
        while (cursor > value_begin &&
               std::isspace(static_cast<unsigned char>(text[cursor - 1]))) {
            --cursor;
        }
        if (cursor > value_begin) {
            text.replace(value_begin, cursor - value_begin, "<redacted>");
            lower.replace(value_begin, cursor - value_begin, "<redacted>");
            scan = value_begin + std::string_view("<redacted>").size();
        } else {
            scan = best + label.size();
        }
    }
}

std::string redact_text(std::string text) {
    redact_labeled_values(text);
    redact_url_credentials(text);
    redact_provider_key_prefixes(text);
    replace_long_secret_runs(text);
    return text;
}

json redact_json(const json& value, std::string_view parent_key = {}) {
    if (is_sensitive_key(parent_key)) return "<redacted>";
    if (value.is_object()) {
        json out = json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            out[it.key()] = redact_json(it.value(), it.key());
        }
        return out;
    }
    if (value.is_array()) {
        json out = json::array();
        for (const auto& item : value) out.push_back(redact_json(item));
        return out;
    }
    if (value.is_string()) return redact_text(value.get<std::string>());
    return value;
}

std::string bounded_text(std::string text, std::size_t limit) {
    if (text.size() <= limit) return text;
    text.resize(limit);
    text += "\n<truncated>";
    return text;
}

std::string dump_json_lossy(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool valid_tool_name(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}

json normalize_tools(const json& supplied) {
    const json* tools = &supplied;
    if (supplied.is_object() && supplied.contains("tools")) tools = &supplied["tools"];
    if (!tools->is_array()) throw std::invalid_argument("tool definitions must be an array");
    if (tools->size() > 128) throw std::invalid_argument("too many tool definitions");

    json out = json::array();
    for (const auto& item : *tools) {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string())
            throw std::invalid_argument("each tool requires a string name");
        const std::string name = item["name"].get<std::string>();
        if (!valid_tool_name(name)) throw std::invalid_argument("invalid tool name: " + name);
        const std::string description = item.value("description", std::string{});
        if (description.size() > 8192)
            throw std::invalid_argument("tool description is too long: " + name);
        json schema = item.value("inputSchema", json::object());
        if (!schema.is_object())
            throw std::invalid_argument("tool inputSchema must be an object: " + name);
        if (schema.empty()) schema = {{"type", "object"}, {"properties", json::object()}};
        out.push_back({{"name", name},
                       {"description", redact_text(description)},
                       {"inputSchema", std::move(schema)}});
    }
    return out;
}

json openai_tools(const json& canonical) {
    json out = json::array();
    for (const auto& item : canonical) {
        out.push_back({{"type", "function"},
                       {"function",
                        {{"name", item.at("name")},
                         {"description", item.value("description", std::string{})},
                         {"parameters", item.at("inputSchema")}}}});
    }
    return out;
}

json anthropic_tools(const json& canonical) {
    json out = json::array();
    for (const auto& item : canonical) {
        out.push_back({{"name", item.at("name")},
                       {"description", item.value("description", std::string{})},
                       {"input_schema", item.at("inputSchema")}});
    }
    return out;
}

std::string content_text(const json& content) {
    if (content.is_string()) return content.get<std::string>();
    if (!content.is_array()) return {};
    std::string out;
    for (const auto& block : content) {
        if (!block.is_object() || block.value("type", std::string{}) != "text" ||
            !block.contains("text") || !block["text"].is_string()) {
            continue;
        }
        if (!out.empty()) out += '\n';
        out += block["text"].get<std::string>();
    }
    return out;
}

ParsedResponse parse_openai_response(const json& root) {
    ParsedResponse result;
    if (!root.is_object() || !root.contains("choices") || !root["choices"].is_array() ||
        root["choices"].empty() || !root["choices"][0].is_object() ||
        !root["choices"][0].contains("message") ||
        !root["choices"][0]["message"].is_object()) {
        result.error = "OpenAI-compatible response is missing choices[0].message";
        return result;
    }
    const auto& message = root["choices"][0]["message"];
    if (message.contains("content")) result.turn.text = content_text(message["content"]);

    if (message.contains("tool_calls")) {
        if (!message["tool_calls"].is_array()) {
            result.error = "OpenAI-compatible tool_calls is not an array";
            return result;
        }
        for (const auto& call : message["tool_calls"]) {
            if (result.turn.calls.size() >= 64) {
                result.error = "OpenAI-compatible response contains too many tool calls";
                return result;
            }
            if (!call.is_object() || !call.contains("id") || !call["id"].is_string() ||
                !call.contains("function") || !call["function"].is_object()) {
                result.error = "OpenAI-compatible tool call is malformed";
                return result;
            }
            const auto& fn = call["function"];
            if (!fn.contains("name") || !fn["name"].is_string() ||
                !fn.contains("arguments") || !fn["arguments"].is_string()) {
                result.error = "OpenAI-compatible function call is malformed";
                return result;
            }
            const std::string name = fn["name"].get<std::string>();
            const std::string id = call["id"].get<std::string>();
            if (id.empty() || id.size() > 256) {
                result.error = "OpenAI-compatible response contains an invalid tool call id";
                return result;
            }
            if (!valid_tool_name(name)) {
                result.error = "OpenAI-compatible response contains an invalid tool name";
                return result;
            }
            json args = json::parse(fn["arguments"].get<std::string>(), nullptr, false);
            if (args.is_discarded() || !args.is_object()) {
                result.error = "OpenAI-compatible tool arguments are not a JSON object";
                return result;
            }
            result.turn.calls.push_back({id, std::move(name), std::move(args)});
        }
    }
    result.ok = true;
    return result;
}

ParsedResponse parse_anthropic_response(const json& root) {
    ParsedResponse result;
    if (!root.is_object() || !root.contains("content") || !root["content"].is_array()) {
        result.error = "Anthropic response is missing content";
        return result;
    }
    for (const auto& block : root["content"]) {
        if (!block.is_object()) continue;
        const std::string type = block.value("type", std::string{});
        if (type == "text" && block.contains("text") && block["text"].is_string()) {
            if (!result.turn.text.empty()) result.turn.text += '\n';
            result.turn.text += block["text"].get<std::string>();
        } else if (type == "tool_use") {
            if (result.turn.calls.size() >= 64) {
                result.error = "Anthropic response contains too many tool calls";
                return result;
            }
            if (!block.contains("id") || !block["id"].is_string() ||
                !block.contains("name") || !block["name"].is_string() ||
                !block.contains("input") || !block["input"].is_object()) {
                result.error = "Anthropic tool_use block is malformed";
                return result;
            }
            const std::string name = block["name"].get<std::string>();
            const std::string id = block["id"].get<std::string>();
            if (id.empty() || id.size() > 256) {
                result.error = "Anthropic response contains an invalid tool call id";
                return result;
            }
            if (!valid_tool_name(name)) {
                result.error = "Anthropic response contains an invalid tool name";
                return result;
            }
            result.turn.calls.push_back({id, name, block["input"]});
        }
    }
    result.ok = true;
    return result;
}

ParsedResponse parse_provider_response(Provider provider, const json& root) {
    return provider == Provider::Anthropic ? parse_anthropic_response(root)
                                           : parse_openai_response(root);
}

json openai_messages(std::string_view system_prompt,
                     const std::vector<InternalMessage>& history) {
    json messages = json::array({{{"role", "system"}, {"content", system_prompt}}});
    for (const auto& item : history) {
        if (item.role == InternalRole::User) {
            messages.push_back({{"role", "user"}, {"content", item.text}});
        } else if (item.role == InternalRole::Tool) {
            messages.push_back({{"role", "tool"},
                                {"tool_call_id", item.tool_call_id},
                                {"content", item.text}});
        } else {
            json message = {{"role", "assistant"}};
            message["content"] = item.text.empty() ? json(nullptr) : json(item.text);
            if (!item.calls.empty()) {
                message["tool_calls"] = json::array();
                for (const auto& call : item.calls) {
                    message["tool_calls"].push_back(
                        {{"id", call.id},
                         {"type", "function"},
                         {"function",
                          {{"name", call.name}, {"arguments", call.arguments.dump()}}}});
                }
            }
            messages.push_back(std::move(message));
        }
    }
    return messages;
}

void append_anthropic_message(json& messages, const char* role, json content) {
    if (!messages.empty() && messages.back().value("role", std::string{}) == role &&
        messages.back().contains("content") && messages.back()["content"].is_array()) {
        for (auto& block : content) messages.back()["content"].push_back(std::move(block));
        return;
    }
    messages.push_back({{"role", role}, {"content", std::move(content)}});
}

json anthropic_messages(const std::vector<InternalMessage>& history) {
    json messages = json::array();
    for (const auto& item : history) {
        if (item.role == InternalRole::User) {
            append_anthropic_message(
                messages, "user", json::array({{{"type", "text"}, {"text", item.text}}}));
        } else if (item.role == InternalRole::Tool) {
            json block = {{"type", "tool_result"},
                          {"tool_use_id", item.tool_call_id},
                          {"content", item.text}};
            if (item.tool_error) block["is_error"] = true;
            append_anthropic_message(messages, "user", json::array({std::move(block)}));
        } else {
            json content = json::array();
            if (!item.text.empty()) content.push_back({{"type", "text"}, {"text", item.text}});
            for (const auto& call : item.calls) {
                content.push_back({{"type", "tool_use"},
                                   {"id", call.id},
                                   {"name", call.name},
                                   {"input", call.arguments}});
            }
            if (content.empty()) content.push_back({{"type", "text"}, {"text", ""}});
            append_anthropic_message(messages, "assistant", std::move(content));
        }
    }
    return messages;
}

json build_request(const AiSettings& settings, std::string_view system_prompt,
                   const json& tools, const std::vector<InternalMessage>& history) {
    if (settings.provider == Provider::Anthropic) {
        json request = {{"model", settings.model},
                        {"max_tokens", settings.max_tokens},
                        {"system", system_prompt},
                        {"messages", anthropic_messages(history)}};
        if (!tools.empty()) request["tools"] = anthropic_tools(tools);
        return request;
    }
    json request = {{"model", settings.model},
                    {"max_tokens", settings.max_tokens},
                    {"messages", openai_messages(system_prompt, history)}};
    if (!tools.empty()) request["tools"] = openai_tools(tools);
    return request;
}

std::string endpoint_host(std::string_view endpoint, std::string_view scheme) {
    std::string_view rest = endpoint.substr(scheme.size());
    const std::size_t slash = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, slash);
    if (authority.find('@') != std::string_view::npos) return {};
    if (authority.starts_with('[')) {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return {};
        return lower_ascii(authority.substr(0, close + 1));
    }
    const std::size_t colon = authority.find(':');
    return lower_ascii(authority.substr(0, colon));
}

bool valid_endpoint(std::string_view endpoint) {
    if (endpoint.empty() || endpoint.size() > 2048 ||
        endpoint.find_first_of("\r\n") != std::string_view::npos) {
        return false;
    }
    const std::string lower = lower_ascii(endpoint);
    if (lower.starts_with("https://")) return !endpoint_host(endpoint, "https://").empty();
    if (!lower.starts_with("http://")) return false;
    const std::string host = endpoint_host(endpoint, "http://");
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

bool validate_settings(AiSettings& settings, std::string& error) {
    if (settings.endpoint.empty()) {
        settings.endpoint = settings.provider == Provider::Anthropic
                                ? "https://api.anthropic.com/v1/messages"
                                : "https://api.openai.com/v1/chat/completions";
    }
    if (!valid_endpoint(settings.endpoint)) {
        error = "Endpoint must use HTTPS (plain HTTP is allowed only for localhost).";
        return false;
    }
    if (settings.model.empty() || settings.model.size() > 256 ||
        settings.model.find_first_of("\r\n") != std::string::npos) {
        error = "Model is required and must be at most 256 characters.";
        return false;
    }
    if (settings.api_key.empty() || settings.api_key.size() > 8192 ||
        settings.api_key.find_first_of("\r\n") != std::string::npos) {
        error = "API key is required and contains invalid characters.";
        return false;
    }
    if (settings.max_tokens < 64 || settings.max_tokens > 32'768) {
        error = "max_tokens must be between 64 and 32768.";
        return false;
    }
    return true;
}

void secure_clear(std::string& value) {
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

void ensure_curl_initialized() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct ResponseSink {
    std::string body;
    bool too_large = false;
};

std::size_t write_response(char* data, std::size_t size, std::size_t count, void* userdata) {
    auto& sink = *static_cast<ResponseSink*>(userdata);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const std::size_t bytes = size * count;
    if (bytes > kMaxResponseBytes - std::min(kMaxResponseBytes, sink.body.size())) {
        sink.too_large = true;
        return 0;
    }
    sink.body.append(data, bytes);
    return bytes;
}

int transfer_progress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<std::atomic<bool>*>(userdata)->load(std::memory_order_relaxed) ? 1 : 0;
}

std::string provider_error_message(Provider provider, const json& body, long status) {
    std::string message = "AI provider returned HTTP " + std::to_string(status) + '.';
    if (!body.is_object()) return message;
    if (provider == Provider::Anthropic && body.contains("error") && body["error"].is_object() &&
        body["error"].contains("message") && body["error"]["message"].is_string()) {
        message += " " + body["error"]["message"].get<std::string>();
    } else if (body.contains("error")) {
        if (body["error"].is_object() && body["error"].contains("message") &&
            body["error"]["message"].is_string()) {
            message += " " + body["error"]["message"].get<std::string>();
        } else if (body["error"].is_string()) {
            message += " " + body["error"].get<std::string>();
        }
    }
    return bounded_text(redact_text(std::move(message)), 1024);
}

HttpOutcome perform_http(AiSettings settings, json request,
                         std::atomic<bool>* cancel_requested) {
    struct KeyGuard {
        std::string& key;
        ~KeyGuard() { secure_clear(key); }
    } key_guard{settings.api_key};

    HttpOutcome outcome;
    std::string body = dump_json_lossy(request);
    if (body.size() > kMaxRequestBytes) {
        outcome.error = "AI request exceeds the 4 MiB safety limit.";
        return outcome;
    }
    ensure_curl_initialized();
    CURL* curl = curl_easy_init();
    if (!curl) {
        outcome.error = "Could not initialize the AI HTTP client.";
        return outcome;
    }

    struct CurlGuard {
        CURL* value;
        ~CurlGuard() { curl_easy_cleanup(value); }
    } curl_guard{curl};
    struct curl_slist* headers = nullptr;
    struct HeaderGuard {
        curl_slist*& value;
        ~HeaderGuard() {
            if (value) curl_slist_free_all(value);
        }
    } header_guard{headers};

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    if (settings.provider == Provider::Anthropic) {
        const std::string key_header = "x-api-key: " + settings.api_key;
        headers = curl_slist_append(headers, key_header.c_str());
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    } else {
        const std::string auth_header = "Authorization: Bearer " + settings.api_key;
        headers = curl_slist_append(headers, auth_header.c_str());
    }

    ResponseSink sink;
    char error_buffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_URL, settings.endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kHttpTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15'000L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);  // Never forward an auth header.
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Nxrth-AI/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel_requested);

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    secure_clear(body);

    if (cancel_requested->load(std::memory_order_relaxed) || code == CURLE_ABORTED_BY_CALLBACK) {
        outcome.cancelled = true;
        outcome.error = "AI request cancelled.";
        return outcome;
    }
    if (sink.too_large) {
        outcome.error = "AI provider response exceeds the 4 MiB safety limit.";
        return outcome;
    }
    if (code != CURLE_OK) {
        std::string detail = error_buffer[0] ? error_buffer : curl_easy_strerror(code);
        outcome.error = bounded_text(redact_text("AI network request failed: " + detail), 1024);
        return outcome;
    }

    json parsed = json::parse(sink.body, nullptr, false);
    secure_clear(sink.body);
    if (parsed.is_discarded()) {
        outcome.error = "AI provider returned an invalid JSON response.";
        return outcome;
    }
    if (status < 200 || status >= 300) {
        outcome.error = provider_error_message(settings.provider, parsed, status);
        return outcome;
    }
    ParsedResponse response = parse_provider_response(settings.provider, parsed);
    if (!response.ok) {
        outcome.error = redact_text(std::move(response.error));
        return outcome;
    }
    outcome.ok = true;
    outcome.turn = std::move(response.turn);
    return outcome;
}

std::string tool_list_label(const std::vector<ToolCall>& calls) {
    std::string out;
    for (const auto& call : calls) {
        if (!out.empty()) out += ", ";
        out += call.name;
    }
    return redact_text(std::move(out));
}

}  // namespace

struct AiController::Impl {
    struct PendingInternal {
        std::uint64_t id = 0;
        std::vector<ToolCall> calls;
        std::optional<bool> decision;
    };

    mutable std::mutex mutex;
    json tools = json::array();
    ToolExecutor executor;
    std::string system_prompt;
    std::vector<InternalMessage> history;
    std::vector<TranscriptEntry> transcript;
    AiSettings settings;
    std::string error;

    bool active = false;
    bool http_running = false;
    std::uint64_t generation = 0;
    std::uint64_t next_approval_id = 1;
    std::uint32_t rounds = 0;
    std::uint32_t tool_calls = 0;
    std::atomic<bool> cancel_requested{false};
    std::thread worker;
    std::optional<HttpOutcome> completed;
    std::optional<PendingInternal> pending;

    Impl(json canonical_tools, ToolExecutor callback, std::string prompt)
        : tools(normalize_tools(canonical_tools)),
          executor(std::move(callback)),
          system_prompt(std::move(prompt)) {
        if (system_prompt.empty() || system_prompt.size() > 64u * 1024u)
            throw std::invalid_argument("system prompt length is invalid");
    }

    ~Impl() {
        cancel_requested.store(true, std::memory_order_relaxed);
        if (worker.joinable()) worker.join();
        secure_clear(settings.api_key);
    }

    void append_transcript(TranscriptRole role, std::string text,
                           std::string tool_name = {}, bool is_error = false) {
        transcript.push_back({role, bounded_text(redact_text(std::move(text)), 64u * 1024u),
                              redact_text(std::move(tool_name)), is_error});
        if (transcript.size() > kMaxTranscriptEntries) {
            transcript.erase(transcript.begin(),
                             transcript.begin() + (transcript.size() - kMaxTranscriptEntries));
        }
    }

    void finish_locked() {
        active = false;
        pending.reset();
        secure_clear(settings.api_key);
    }

    void fail_locked(std::string message) {
        error = bounded_text(redact_text(std::move(message)), 2048);
        append_transcript(TranscriptRole::Error, error, {}, true);
        finish_locked();
    }

    bool launch_locked() {
        if (rounds >= kMaxRounds) {
            fail_locked("AI agent stopped after reaching the 12-round safety limit.");
            return false;
        }
        if (worker.joinable()) {
            fail_locked("Internal AI worker lifecycle error.");
            return false;
        }

        json request;
        try {
            request = build_request(settings, system_prompt, tools, history);
        } catch (const std::exception& e) {
            fail_locked(std::string("Could not build AI request: ") + e.what());
            return false;
        }
        AiSettings snapshot = settings;
        cancel_requested.store(false, std::memory_order_relaxed);
        http_running = true;
        completed.reset();
        ++rounds;
        const std::uint64_t run_generation = generation;
        try {
            worker = std::thread([this, run_generation, snapshot = std::move(snapshot),
                                  request = std::move(request)]() mutable {
                HttpOutcome outcome;
                try {
                    outcome =
                        perform_http(std::move(snapshot), std::move(request), &cancel_requested);
                } catch (const std::exception& e) {
                    outcome.error = bounded_text(
                        redact_text(std::string("AI worker failed: ") + e.what()), 1024);
                } catch (...) {
                    outcome.error = "AI worker failed with an unknown error.";
                }
                std::lock_guard lock(mutex);
                if (run_generation == generation) completed = std::move(outcome);
                http_running = false;
            });
        } catch (const std::exception& e) {
            http_running = false;
            fail_locked(std::string("Could not start AI worker: ") + e.what());
            return false;
        }
        return true;
    }

    void append_tool_result_locked(const ToolCall& call, json result, bool is_error) {
        std::string content;
        try {
            content = dump_json_lossy(redact_json(result));
            if (content.size() > kMaxToolResultBytes) {
                content = dump_json_lossy(
                    json{{"truncated", true},
                         {"preview", bounded_text(content, kMaxToolResultBytes / 2)}});
            }
        } catch (const std::exception&) {
            content = R"({"ok":false,"error":"Tool result could not be serialized."})";
            is_error = true;
        } catch (...) {
            content = R"({"ok":false,"error":"Tool result could not be serialized."})";
            is_error = true;
        }
        history.push_back({InternalRole::Tool, content, {}, call.id, call.name, is_error});
        append_transcript(TranscriptRole::Tool, content, call.name, is_error);
    }

    std::thread take_finished_worker_locked() {
        if (worker.joinable() && !http_running) return std::move(worker);
        return {};
    }

    bool has_tool(std::string_view name) const {
        return std::any_of(tools.begin(), tools.end(), [&](const json& item) {
            return item.value("name", std::string{}) == name;
        });
    }
};

AiController::AiController(json canonical_tools, ToolExecutor executor,
                           std::string system_prompt)
    : impl_(std::make_unique<Impl>(std::move(canonical_tools), std::move(executor),
                                  std::move(system_prompt))) {}

AiController::~AiController() = default;

bool AiController::submit(std::string user_message, AiSettings settings, std::string* error) {
    if (user_message.empty() || user_message.size() > 256u * 1024u) {
        if (error) *error = "Message must be between 1 byte and 256 KiB.";
        return false;
    }
    std::string validation_error;
    if (!validate_settings(settings, validation_error)) {
        if (error) *error = validation_error;
        secure_clear(settings.api_key);
        return false;
    }

    std::thread finished_worker;
    {
        std::lock_guard lock(impl_->mutex);
        finished_worker = impl_->take_finished_worker_locked();
    }
    if (finished_worker.joinable()) finished_worker.join();

    std::lock_guard lock(impl_->mutex);
    if (impl_->active || impl_->http_running || impl_->worker.joinable()) {
        if (error) *error = "AI controller is busy.";
        secure_clear(settings.api_key);
        return false;
    }
    impl_->settings = std::move(settings);
    impl_->error.clear();
    impl_->rounds = 0;
    impl_->tool_calls = 0;
    impl_->pending.reset();
    impl_->completed.reset();
    ++impl_->generation;
    impl_->active = true;
    impl_->history.push_back({InternalRole::User, user_message});
    impl_->append_transcript(TranscriptRole::User, std::move(user_message));
    if (!impl_->launch_locked()) {
        if (error) *error = impl_->error;
        return false;
    }
    return true;
}

void AiController::pump() {
    // Reap a completed HTTP worker before any possible next round. No tool
    // callback exists anywhere in the worker path below perform_http().
    std::thread completed_worker;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->worker.joinable() && !impl_->http_running) {
            completed_worker = std::move(impl_->worker);
        }
    }
    if (completed_worker.joinable()) completed_worker.join();

    std::vector<ToolCall> calls_to_execute;
    ToolExecutor executor;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->active) return;

        if (impl_->completed) {
            HttpOutcome outcome = std::move(*impl_->completed);
            impl_->completed.reset();
            if (outcome.cancelled || impl_->cancel_requested.load(std::memory_order_relaxed)) {
                impl_->append_transcript(TranscriptRole::Status, "AI request cancelled.");
                impl_->finish_locked();
                return;
            }
            if (!outcome.ok) {
                impl_->fail_locked(std::move(outcome.error));
                return;
            }

            outcome.turn.text = bounded_text(redact_text(std::move(outcome.turn.text)),
                                             1024u * 1024u);
            impl_->history.push_back(
                {InternalRole::Assistant, outcome.turn.text, outcome.turn.calls});
            if (!outcome.turn.text.empty()) {
                impl_->append_transcript(TranscriptRole::Assistant,
                                         std::move(outcome.turn.text));
            } else if (!outcome.turn.calls.empty()) {
                impl_->append_transcript(
                    TranscriptRole::Assistant,
                    "Requested tools: " + tool_list_label(outcome.turn.calls));
            }
            if (outcome.turn.calls.empty()) {
                impl_->finish_locked();
                return;
            }
            for (std::size_t i = 0; i < outcome.turn.calls.size(); ++i) {
                const auto& call = outcome.turn.calls[i];
                if (!impl_->has_tool(call.name)) {
                    impl_->history.pop_back();
                    impl_->fail_locked("AI requested unavailable tool '" +
                                       redact_text(call.name) + "'; no calls were run.");
                    return;
                }
                for (std::size_t j = 0; j < i; ++j) {
                    if (outcome.turn.calls[j].id == call.id) {
                        impl_->history.pop_back();
                        impl_->fail_locked(
                            "AI returned duplicate tool call identifiers; no calls were run.");
                        return;
                    }
                }
            }
            if (impl_->rounds >= kMaxRounds) {
                impl_->history.pop_back();
                impl_->fail_locked(
                    "AI agent requested tools at the 12-round safety limit; calls were not run.");
                return;
            }
            if (outcome.turn.calls.size() > kMaxToolCalls - impl_->tool_calls) {
                impl_->history.pop_back();
                impl_->fail_locked(
                    "AI agent exceeded the 32-tool-call safety limit; calls were not run.");
                return;
            }
            impl_->tool_calls += static_cast<std::uint32_t>(outcome.turn.calls.size());
            impl_->pending = Impl::PendingInternal{impl_->next_approval_id++,
                                                   std::move(outcome.turn.calls), std::nullopt};
            if (impl_->settings.autonomy == Autonomy::Ask) return;
            impl_->pending->decision = true;
        }

        if (!impl_->pending || !impl_->pending->decision.has_value()) return;
        const bool allowed = *impl_->pending->decision;
        const auto calls = std::move(impl_->pending->calls);
        impl_->pending.reset();
        if (!allowed) {
            for (const auto& call : calls) {
                impl_->append_tool_result_locked(
                    call, {{"ok", false}, {"error", "Denied by user."}}, true);
            }
            impl_->launch_locked();
            return;
        }
        if (!impl_->executor) {
            for (const auto& call : calls) {
                impl_->append_tool_result_locked(
                    call, {{"ok", false}, {"error", "Tool executor is not configured."}}, true);
            }
            impl_->fail_locked("AI requested a tool, but no tool executor is configured.");
            return;
        }
        if (impl_->cancel_requested.load(std::memory_order_relaxed)) {
            for (const auto& call : calls) {
                impl_->append_tool_result_locked(
                    call, {{"ok", false}, {"error", "Cancelled by user."}}, true);
            }
            impl_->append_transcript(TranscriptRole::Status, "AI request cancelled.");
            impl_->finish_locked();
            return;
        }
        calls_to_execute = calls;
        executor = impl_->executor;
    }

    struct Executed {
        ToolCall call;
        json result;
        bool error = false;
    };
    std::vector<Executed> executed;
    executed.reserve(calls_to_execute.size());
    for (const auto& call : calls_to_execute) {
        try {
            json result = executor(call.name, call.arguments);
            const bool is_error = result.is_object() && result.value("isError", false);
            executed.push_back({call, std::move(result), is_error});
        } catch (const std::exception& e) {
            executed.push_back(
                {call, {{"ok", false}, {"error", redact_text(e.what())}}, true});
        } catch (...) {
            executed.push_back(
                {call, {{"ok", false}, {"error", "Tool executor threw an unknown error."}},
                 true});
        }
    }

    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->active) return;  // cancel() may have won while a tool was running.
        for (auto& item : executed) {
            impl_->append_tool_result_locked(item.call, std::move(item.result), item.error);
        }
        if (impl_->cancel_requested.load(std::memory_order_relaxed)) {
            impl_->append_transcript(TranscriptRole::Status, "AI request cancelled.");
            impl_->finish_locked();
            return;
        }
        impl_->launch_locked();
    }
}

bool AiController::busy() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->active;
}

void AiController::cancel() {
    impl_->cancel_requested.store(true, std::memory_order_relaxed);
    std::thread finished_worker;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->active) {
            finished_worker = impl_->take_finished_worker_locked();
        } else if (impl_->http_running) {
            return;
        } else if (impl_->pending) {
            // Preserve a provider-valid history: Anthropic requires every
            // tool_use block to be followed by a matching tool_result.
            for (const auto& call : impl_->pending->calls) {
                impl_->append_tool_result_locked(
                    call, {{"ok", false}, {"error", "Cancelled by user."}}, true);
            }
            impl_->pending.reset();
            impl_->append_transcript(TranscriptRole::Status, "AI request cancelled.");
            impl_->finish_locked();
            finished_worker = impl_->take_finished_worker_locked();
        } else if (impl_->completed) {
            ++impl_->generation;
            impl_->completed.reset();
            impl_->append_transcript(TranscriptRole::Status, "AI request cancelled.");
            impl_->finish_locked();
            finished_worker = impl_->take_finished_worker_locked();
        }
    }
    if (finished_worker.joinable()) finished_worker.join();
    // Otherwise pump() is currently executing a tool callback. That callback
    // cannot be preempted safely; pump() will append its result and observe the
    // cancellation before starting another HTTP round.
}

std::vector<TranscriptEntry> AiController::transcript_snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->transcript;
}

std::optional<ApprovalRequest> AiController::pending_approval() const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->active || !impl_->pending || impl_->pending->decision.has_value())
        return std::nullopt;
    ApprovalRequest out;
    out.id = impl_->pending->id;
    out.calls.reserve(impl_->pending->calls.size());
    for (const auto& call : impl_->pending->calls) {
        out.calls.push_back(
            {redact_text(call.id), redact_text(call.name), redact_json(call.arguments)});
    }
    return out;
}

bool AiController::approve(std::uint64_t approval_id, bool allow, std::string* error) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->active || !impl_->pending || impl_->pending->decision.has_value() ||
        impl_->pending->id != approval_id) {
        if (error) *error = "Approval request is no longer pending.";
        return false;
    }
    impl_->pending->decision = allow;
    return true;
}

bool AiController::clear() {
    std::thread finished_worker;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->active || impl_->http_running) return false;
        finished_worker = impl_->take_finished_worker_locked();
        if (impl_->worker.joinable()) return false;
        impl_->history.clear();
        impl_->transcript.clear();
        impl_->error.clear();
        impl_->pending.reset();
    }
    if (finished_worker.joinable()) finished_worker.join();
    return true;
}

bool AiController::set_tools(json canonical_tools, std::string* error) {
    json normalized;
    try {
        normalized = normalize_tools(canonical_tools);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->active) {
        if (error) *error = "AI controller is busy.";
        return false;
    }
    impl_->tools = std::move(normalized);
    return true;
}

bool AiController::set_tool_executor(ToolExecutor executor) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->active) return false;
    impl_->executor = std::move(executor);
    return true;
}

bool AiController::set_system_prompt(std::string prompt, std::string* error) {
    if (prompt.empty() || prompt.size() > 64u * 1024u) {
        if (error) *error = "System prompt must be between 1 byte and 64 KiB.";
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->active) {
        if (error) *error = "AI controller is busy.";
        return false;
    }
    impl_->system_prompt = std::move(prompt);
    return true;
}

std::string AiController::last_error() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->error;
}

std::string AiController::default_endpoint(Provider provider) {
    return provider == Provider::Anthropic ? "https://api.anthropic.com/v1/messages"
                                           : "https://api.openai.com/v1/chat/completions";
}

bool AiController::self_test(std::string* error) {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    try {
        const json canonical = json::array(
            {{{"name", "session_list"},
              {"description", "List bots"},
              {"inputSchema",
               {{"type", "object"},
                {"properties", {{"detailed", {{"type", "boolean"}}}}}}}}});
        const json normalized = normalize_tools(canonical);
        const json oa = openai_tools(normalized);
        const json an = anthropic_tools(normalized);
        if (oa[0]["type"] != "function" ||
            oa[0]["function"]["parameters"]["type"] != "object" ||
            an[0]["input_schema"]["type"] != "object") {
            return fail("provider tool schema conversion failed");
        }

        const json openai_fixture = {
            {"choices",
             json::array({{{"message",
                            {{"role", "assistant"},
                             {"content", "Checking."},
                             {"tool_calls",
                              json::array(
                                  {{{"id", "call_1"},
                                    {"type", "function"},
                                    {"function",
                                     {{"name", "session_list"},
                                      {"arguments", "{\"detailed\":true}"}}}}})}}}}})}};
        const ParsedResponse parsed_oa =
            parse_provider_response(Provider::OpenAICompatible, openai_fixture);
        if (!parsed_oa.ok || parsed_oa.turn.text != "Checking." ||
            parsed_oa.turn.calls.size() != 1 ||
            parsed_oa.turn.calls[0].arguments.value("detailed", false) != true) {
            return fail("OpenAI-compatible response parsing failed");
        }

        const json anthropic_fixture = {
            {"content",
             json::array({{{"type", "text"}, {"text", "Checking."}},
                          {{"type", "tool_use"},
                           {"id", "toolu_1"},
                           {"name", "session_list"},
                           {"input", {{"detailed", true}}}}})}};
        const ParsedResponse parsed_an =
            parse_provider_response(Provider::Anthropic, anthropic_fixture);
        if (!parsed_an.ok || parsed_an.turn.calls.size() != 1 ||
            parsed_an.turn.calls[0].id != "toolu_1") {
            return fail("Anthropic response parsing failed");
        }

        const std::string secret = std::string(120, 'A') + "+/==";
        const std::string text = redact_text("token:" + secret + " proxy=user:pass@host");
        const json redacted = redact_json(
            {{"token", secret}, {"nested", {{"api_key", "sk-test-value"}}}, {"safe", 7}});
        const json native_redacted = redact_json(
            {{"loginToken", "short-login"},
             {"UUIDToken", "short-uuid"},
             {"UbiTicket", "short-ticket"},
             {"tankIDPass", "short-password"},
             {"safe", 9}});
        if (text.find(secret) != std::string::npos ||
            text.find("<redacted>") == std::string::npos ||
            redacted.dump().find(secret) != std::string::npos ||
            redacted["token"] != "<redacted>" || redacted["nested"]["api_key"] != "<redacted>" ||
            redacted["safe"] != 7 || native_redacted["loginToken"] != "<redacted>" ||
            native_redacted["UUIDToken"] != "<redacted>" ||
            native_redacted["UbiTicket"] != "<redacted>" ||
            native_redacted["tankIDPass"] != "<redacted>" ||
            native_redacted["safe"] != 9) {
            return fail("secret redaction fixture failed");
        }

        const json invalid_utf8 = std::string(1, static_cast<char>(0xff));
        if (dump_json_lossy(invalid_utf8).empty())
            return fail("invalid UTF-8 serialization fixture failed");

        if (!valid_endpoint("https://api.example.com/v1/chat/completions") ||
            !valid_endpoint("http://localhost:11434/v1/chat/completions") ||
            valid_endpoint("http://example.com/v1/chat/completions") ||
            valid_endpoint("https://user:pass@example.com/v1/chat/completions")) {
            return fail("endpoint policy fixture failed");
        }
    } catch (const std::exception& e) {
        return fail(std::string("self-test exception: ") + e.what());
    }
    if (error) error->clear();
    return true;
}

}  // namespace nxrth::ai
