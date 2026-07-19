#include "core/accounts.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace nxrth::core {
namespace {

using nlohmann::json;

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool likely_username(const std::string& u) {
    if (u.size() < 2 || u.size() > 40) return false;
    for (char c : u)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
            return false;
    return true;
}

bool likely_password(const std::string& p) { return p.size() >= 3 && p.size() <= 64; }

// The many key spellings Mori accepted for each field.
const std::vector<std::string>& user_keys() {
    static const std::vector<std::string> k = {
        "username", "user",      "growid",       "grow_id",   "grow_id_name",
        "growidname", "tankidname", "tank_id_name", "account",   "account_name",
        "name",     "login"};
    return k;
}
const std::vector<std::string>& pass_keys() {
    static const std::vector<std::string> k = {"password",     "pass",         "pwd",
                                               "growpass",     "grow_pass",    "grow_id_pass",
                                               "growidpass",   "tankidpass",   "tank_id_pass"};
    return k;
}
const std::vector<std::string>& token_keys() {
    static const std::vector<std::string> k = {"login_token", "loginToken", "ltoken"};
    return k;
}

// Case-insensitive lookup of the first present string field from `keys`.
std::string field(const json& obj, const std::vector<std::string>& keys) {
    for (const auto& [key, val] : obj.items()) {
        if (!val.is_string()) continue;
        const std::string lk = lower(key);
        for (const auto& want : keys)
            if (lk == lower(want)) return trim(val.get<std::string>());
    }
    return {};
}

// "user:pass" (no pipe, no scheme). Mirrors parseCredentialString.
bool parse_cred_string(const std::string& value, Account& out) {
    const std::string raw = trim(value);
    if (raw.empty() || raw.find('\n') != std::string::npos || raw.find('|') != std::string::npos)
        return false;
    if (lower(raw).rfind("http", 0) == 0) return false;
    const auto colon = raw.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    Account a;
    a.username = trim(raw.substr(0, colon));
    a.password = trim(raw.substr(colon + 1));
    if (!likely_username(a.username) || !likely_password(a.password)) return false;
    out = a;
    return true;
}

std::string normalized_ltoken_key(std::string key) {
    key = lower(trim(key));
    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) {
                  return c == '_' || c == '-';
              }),
              key.end());
    return key;
}

// Keep the full secret opaque here; the bot parser performs authoritative validation.
bool parse_keyed_ltoken_string(const std::string& value, Account& out) {
    const std::string raw = trim(value);
    if (raw.empty() || raw.find('|') == std::string::npos || raw.find('\n') != std::string::npos)
        return false;

    bool has_token = false, has_rid = false, has_mac = false, has_wk = false;
    std::string name, rid;
    std::size_t start = 0;
    while (start <= raw.size()) {
        const auto bar = raw.find('|', start);
        const std::string part = raw.substr(
            start, bar == std::string::npos ? std::string::npos : bar - start);
        const auto colon = part.find(':');
        if (colon != std::string::npos) {
            const std::string key = normalized_ltoken_key(part.substr(0, colon));
            const std::string field_value = trim(part.substr(colon + 1));
            if (key == "token" || key == "refreshtoken") has_token = !field_value.empty();
            else if (key == "rid") {
                has_rid = !field_value.empty();
                rid = field_value;
            } else if (key == "mac") has_mac = !field_value.empty();
            else if (key == "wk") has_wk = !field_value.empty();
            else if (key == "name" || key == "username") name = field_value;
        }
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    if (!has_token || !has_rid || !has_mac || !has_wk) return false;

    Account a;
    a.username = likely_username(name) ? name : "ltoken_" + rid.substr(0, 8);
    a.login_token = raw;
    out = std::move(a);
    return true;
}

void collect(const json& v, std::vector<Account>& out) {
    if (v.is_array()) {
        for (const auto& e : v) collect(e, out);
        return;
    }
    if (v.is_string()) {
        Account a;
        const std::string value = v.get<std::string>();
        if (parse_keyed_ltoken_string(value, a) || parse_cred_string(value, a))
            out.push_back(a);
        return;
    }
    if (!v.is_object()) return;

    const std::string u = field(v, user_keys());
    const std::string p = field(v, pass_keys());
    if (likely_username(u) && likely_password(p)) {
        Account a;
        a.username = u;
        a.password = p;
        a.login_token = field(v, token_keys());
        out.push_back(a);
    }
    for (const auto& [key, child] : v.items()) collect(child, out);
}

std::vector<Account> parse_lines(const std::string& input) {
    std::vector<Account> out;
    std::size_t start = 0;
    while (start <= input.size()) {
        const auto nl = input.find('\n', start);
        std::string line = input.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        Account a;
        if (parse_keyed_ltoken_string(line, a) || parse_cred_string(line, a)) out.push_back(a);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

std::vector<Account> dedup(std::vector<Account> in) {
    std::vector<Account> out;
    std::unordered_set<std::string> seen;
    for (auto& a : in) {
        const std::string key = a.login_token.empty() ? "user:" + lower(a.username)
                                                       : "ltoken:" + a.login_token;
        if (seen.insert(key).second) out.push_back(std::move(a));
    }
    return out;
}

}  // namespace

std::vector<Account> parse_account_stats(const std::string& input) {
    const std::string text = trim(input);
    if (text.empty()) return {};
    // Try JSON first.
    json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_discarded()) {
        std::vector<Account> found;
        collect(parsed, found);
        found = dedup(std::move(found));
        if (!found.empty()) return found;
    }
    // Fallback: plain "user:pass" or provider-keyed ltoken lines.
    return dedup(parse_lines(text));
}

}  // namespace nxrth::core
