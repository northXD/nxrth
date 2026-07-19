#include "recovery/fleet_store.h"

#include "bot/bot_manager.h"
#include "proxy/proxy_pool.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace nxrth::recovery {
namespace {

using json = nlohmann::json;
using nxrth::bot::AutomationConfig;
using nxrth::bot::LaunchCredentialKind;
using nxrth::bot::LaunchRecord;
using nxrth::bot::ProxyPolicy;
using nxrth::net::Socks5Config;

constexpr std::string_view kMagic = "NXRTH-FLEET-DPAPI-V1\n";
constexpr std::string_view kDocumentType = "nxrth.fleet";
constexpr std::uint32_t kDocumentVersion = 1;
constexpr std::uintmax_t kMaxBackupBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxBots = 10000;
constexpr std::size_t kMaxEncodedField = 16u * 1024u * 1024u;

struct FleetSnapshot {
    std::uint64_t launch_generation = 0;
    std::vector<LaunchRecord> records;
    AutomationConfig automation;
    bool has_automation = true;
};

std::string hex_encode(std::string_view bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = static_cast<unsigned char>(bytes[i]);
        out[i * 2] = digits[value >> 4];
        out[i * 2 + 1] = digits[value & 0x0f];
    }
    return out;
}

unsigned char hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
    throw std::runtime_error("invalid hex field");
}

std::string hex_decode(const json& value) {
    if (!value.is_string()) throw std::runtime_error("invalid encoded field");
    const std::string encoded = value.get<std::string>();
    if (encoded.size() > kMaxEncodedField || (encoded.size() & 1u) != 0)
        throw std::runtime_error("invalid encoded field");
    std::string out(encoded.size() / 2, '\0');
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>((hex_nibble(encoded[i * 2]) << 4) |
                                   hex_nibble(encoded[i * 2 + 1]));
    }
    return out;
}

json optional_bytes(const std::optional<std::string>& value) {
    return value ? json(hex_encode(*value)) : json(nullptr);
}

std::optional<std::string> parse_optional_bytes(const json& value) {
    if (value.is_null()) return std::nullopt;
    return hex_decode(value);
}

const char* credential_kind_name(LaunchCredentialKind kind) {
    return kind == LaunchCredentialKind::Ltoken ? "ltoken" : "growid";
}

LaunchCredentialKind parse_credential_kind(const json& value) {
    if (!value.is_string()) throw std::runtime_error("invalid credential kind");
    const auto text = value.get<std::string>();
    if (text == "growid") return LaunchCredentialKind::GrowId;
    if (text == "ltoken") return LaunchCredentialKind::Ltoken;
    throw std::runtime_error("invalid credential kind");
}

const char* proxy_policy_name(ProxyPolicy policy) {
    switch (policy) {
        case ProxyPolicy::Direct: return "direct";
        case ProxyPolicy::Pool: return "pool";
        case ProxyPolicy::Custom: return "custom";
    }
    return "direct";
}

ProxyPolicy parse_proxy_policy(const json& value) {
    if (!value.is_string()) throw std::runtime_error("invalid proxy policy");
    const auto text = value.get<std::string>();
    if (text == "direct") return ProxyPolicy::Direct;
    if (text == "pool") return ProxyPolicy::Pool;
    if (text == "custom") return ProxyPolicy::Custom;
    throw std::runtime_error("invalid proxy policy");
}

json proxy_to_json(const Socks5Config& proxy) {
    return json{{"host_hex", hex_encode(proxy.host)},
                {"port", proxy.port},
                {"username_hex", optional_bytes(proxy.username)},
                {"password_hex", optional_bytes(proxy.password)}};
}

Socks5Config proxy_from_json(const json& value) {
    if (!value.is_object()) throw std::runtime_error("invalid custom proxy");
    Socks5Config proxy;
    proxy.host = hex_decode(value.at("host_hex"));
    const auto port = value.at("port").get<std::uint32_t>();
    if (proxy.host.empty() || port == 0 || port > std::numeric_limits<std::uint16_t>::max())
        throw std::runtime_error("invalid custom proxy");
    proxy.port = static_cast<std::uint16_t>(port);
    proxy.username = parse_optional_bytes(value.at("username_hex"));
    proxy.password = parse_optional_bytes(value.at("password_hex"));
    return proxy;
}

json record_to_json(const LaunchRecord& record) {
    json custom = nullptr;
    if (record.custom_proxy) custom = proxy_to_json(*record.custom_proxy);
    return json{{"old_id", record.id},
                {"credential_kind", credential_kind_name(record.credential_kind)},
                {"credential_hex", hex_encode(record.credential)},
                {"password_hex", hex_encode(record.password)},
                {"proxy_policy", proxy_policy_name(record.proxy_policy)},
                {"custom_proxy", std::move(custom)},
                {"rotating_login_requested", record.rotating_login_requested}};
}

LaunchRecord record_from_json(const json& value) {
    if (!value.is_object()) throw std::runtime_error("invalid bot record");
    LaunchRecord record;
    record.id = value.at("old_id").get<std::uint32_t>();
    record.credential_kind = parse_credential_kind(value.at("credential_kind"));
    record.credential = hex_decode(value.at("credential_hex"));
    record.password = hex_decode(value.at("password_hex"));
    record.proxy_policy = parse_proxy_policy(value.at("proxy_policy"));
    record.rotating_login_requested = value.at("rotating_login_requested").get<bool>();

    const auto& custom = value.at("custom_proxy");
    if (!custom.is_null()) record.custom_proxy = proxy_from_json(custom);
    if (record.proxy_policy == ProxyPolicy::Custom && !record.custom_proxy)
        throw std::runtime_error("custom proxy missing");
    if (record.credential.empty()) throw std::runtime_error("credential missing");
    return record;
}

template <typename Map, typename ValueWriter>
json map_entries_to_json(const Map& values, ValueWriter write_value) {
    std::vector<typename Map::key_type> keys;
    keys.reserve(values.size());
    for (const auto& [key, value] : values) {
        (void)value;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());

    json result = json::array();
    for (const auto& key : keys)
        result.push_back(json::array({hex_encode(key), write_value(values.at(key))}));
    return result;
}

json automation_to_json(const AutomationConfig& config) {
    return json{
        {"enabled", map_entries_to_json(config.enabled, [](bool value) { return json(value); })},
        {"params", map_entries_to_json(config.params, [](const std::string& value) {
             return json(hex_encode(value));
         })},
        {"groups", map_entries_to_json(config.groups, [](const auto& value) {
             return json(value);
         })},
        {"module_bot_ids", map_entries_to_json(config.module_bot_ids, [](const auto& value) {
             return json(value);
         })},
        {"module_groups", map_entries_to_json(config.module_groups, [](const std::string& value) {
             return json(hex_encode(value));
         })}};
}

template <typename Apply>
void parse_map_entries(const json& entries, Apply apply) {
    if (!entries.is_array()) throw std::runtime_error("invalid automation map");
    for (const auto& entry : entries) {
        if (!entry.is_array() || entry.size() != 2)
            throw std::runtime_error("invalid automation map entry");
        apply(hex_decode(entry[0]), entry[1]);
    }
}

AutomationConfig automation_from_json(const json& value) {
    if (!value.is_object()) throw std::runtime_error("invalid automation config");
    AutomationConfig config;
    parse_map_entries(value.at("enabled"), [&](std::string key, const json& item) {
        config.enabled.insert_or_assign(std::move(key), item.get<bool>());
    });
    parse_map_entries(value.at("params"), [&](std::string key, const json& item) {
        config.params.insert_or_assign(std::move(key), hex_decode(item));
    });
    parse_map_entries(value.at("groups"), [&](std::string key, const json& item) {
        config.groups.insert_or_assign(std::move(key), item.get<std::vector<std::uint32_t>>());
    });
    parse_map_entries(value.at("module_bot_ids"), [&](std::string key, const json& item) {
        config.module_bot_ids.insert_or_assign(std::move(key),
                                               item.get<std::vector<std::uint32_t>>());
    });
    parse_map_entries(value.at("module_groups"), [&](std::string key, const json& item) {
        config.module_groups.insert_or_assign(std::move(key), hex_decode(item));
    });
    return config;
}

json snapshot_to_json(const FleetSnapshot& snapshot) {
    json bots = json::array();
    for (const auto& record : snapshot.records) bots.push_back(record_to_json(record));
    return json{{"format", kDocumentType},
                {"version", kDocumentVersion},
                {"launch_generation", snapshot.launch_generation},
                {"bots", std::move(bots)},
                {"automation", automation_to_json(snapshot.automation)}};
}

FleetSnapshot snapshot_from_json(const json& value) {
    if (!value.is_object() || value.at("format").get<std::string>() != kDocumentType ||
        value.at("version").get<std::uint32_t>() != kDocumentVersion)
        throw std::runtime_error("unsupported fleet backup");

    const auto& bots = value.at("bots");
    if (!bots.is_array() || bots.size() > kMaxBots)
        throw std::runtime_error("invalid bot record list");

    FleetSnapshot snapshot;
    snapshot.launch_generation = value.at("launch_generation").get<std::uint64_t>();
    snapshot.automation = automation_from_json(value.at("automation"));
    std::set<std::uint32_t> ids;
    snapshot.records.reserve(bots.size());
    for (const auto& item : bots) {
        LaunchRecord record = record_from_json(item);
        if (!ids.insert(record.id).second) throw std::runtime_error("duplicate bot id");
        snapshot.records.push_back(std::move(record));
    }
    return snapshot;
}

AutomationConfig remap_automation(
    const AutomationConfig& source,
    const std::unordered_map<std::uint32_t, std::uint32_t>& id_remap) {
    AutomationConfig result;
    result.enabled = source.enabled;
    result.params = source.params;
    result.module_groups = source.module_groups;

    auto remap_ids = [&](const std::vector<std::uint32_t>& old_ids) {
        std::vector<std::uint32_t> mapped;
        std::set<std::uint32_t> seen;
        mapped.reserve(old_ids.size());
        for (const auto old_id : old_ids) {
            const auto found = id_remap.find(old_id);
            if (found != id_remap.end() && seen.insert(found->second).second)
                mapped.push_back(found->second);
        }
        return mapped;
    };

    for (const auto& [name, ids] : source.groups)
        result.groups.emplace(name, remap_ids(ids));
    for (const auto& [name, ids] : source.module_bot_ids)
        result.module_bot_ids.emplace(name, remap_ids(ids));
    return result;
}

#ifdef _WIN32
DATA_BLOB entropy_blob() {
    // Optional entropy is not a secret; it binds the DPAPI payload to this file
    // type in addition to Windows' CurrentUser key.
    static std::array<BYTE, 20> entropy = {
        0x41, 0x64, 0x6f, 0x6e, 0x61, 0x69, 0x2e, 0x46, 0x6c, 0x65,
        0x65, 0x74, 0x2e, 0x44, 0x50, 0x41, 0x50, 0x49, 0x2e, 0x31};
    return DATA_BLOB{static_cast<DWORD>(entropy.size()), entropy.data()};
}
#endif

bool protect_bytes(std::string_view plaintext, std::vector<std::uint8_t>& encrypted,
                   std::string& error) {
#ifdef _WIN32
    if (plaintext.size() > std::numeric_limits<DWORD>::max()) {
        error = "fleet backup is too large";
        return false;
    }
    DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()))};
    DATA_BLOB output{};
    DATA_BLOB entropy = entropy_blob();
    if (!::CryptProtectData(&input, L"Nxrth fleet backup", &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = "DPAPI encryption failed (code " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    encrypted.assign(output.pbData, output.pbData + output.cbData);
    ::LocalFree(output.pbData);
    return true;
#else
    (void)plaintext;
    (void)encrypted;
    error = "DPAPI fleet backups require Windows";
    return false;
#endif
}

bool unprotect_bytes(std::span<const std::uint8_t> encrypted, std::string& plaintext,
                     std::string& error) {
#ifdef _WIN32
    if (encrypted.empty() || encrypted.size() > std::numeric_limits<DWORD>::max()) {
        error = "invalid encrypted fleet backup";
        return false;
    }
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
                    const_cast<BYTE*>(reinterpret_cast<const BYTE*>(encrypted.data()))};
    DATA_BLOB output{};
    DATA_BLOB entropy = entropy_blob();
    if (!::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = "DPAPI decryption failed (code " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    plaintext.assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    ::LocalFree(output.pbData);
    return true;
#else
    (void)encrypted;
    (void)plaintext;
    error = "DPAPI fleet backups require Windows";
    return false;
#endif
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
               std::string& error) {
    try {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            error = "backup does not exist or cannot be opened";
            return false;
        }
        const auto end = in.tellg();
        if (end < 0 || static_cast<std::uintmax_t>(end) > kMaxBackupBytes) {
            error = "backup file is too large";
            return false;
        }
        bytes.resize(static_cast<std::size_t>(end));
        in.seekg(0);
        if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()),
                                       static_cast<std::streamsize>(bytes.size()))) {
            error = "backup could not be read";
            return false;
        }
        return true;
    } catch (...) {
        error = "backup could not be read";
        return false;
    }
}

bool atomic_write(const std::filesystem::path& target, std::span<const std::uint8_t> bytes,
                  std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        error = "fleet backup directory could not be created";
        return false;
    }

    std::filesystem::path temporary = target;
#ifdef _WIN32
    temporary += L".tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                 std::to_wstring(::GetTickCount64());
#else
    temporary += ".tmp";
#endif
    try {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "temporary backup file could not be opened";
            return false;
        }
        if (!bytes.empty())
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temporary, ec);
            error = "temporary backup file could not be written";
            return false;
        }
        out.close();

#ifdef _WIN32
        if (!::MoveFileExW(temporary.c_str(), target.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary, ec);
            error = "fleet backup could not be committed (code " +
                    std::to_string(::GetLastError()) + ")";
            return false;
        }
#else
        std::filesystem::rename(temporary, target, ec);
        if (ec) {
            std::filesystem::remove(temporary, ec);
            error = "fleet backup could not be committed";
            return false;
        }
#endif
        return true;
    } catch (...) {
        std::filesystem::remove(temporary, ec);
        error = "fleet backup could not be written";
        return false;
    }
}

std::string trim_ascii(std::string value) {
    auto blank = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && blank(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && blank(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::vector<std::string> split_pipe(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const auto next = line.find('|', start);
        if (next == std::string::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, next - start));
        start = next + 1;
    }
}

std::optional<Socks5Config> legacy_proxy(const std::string& text) {
    if (trim_ascii(text).empty()) return std::nullopt;
    const auto entries = nxrth::proxy::parse_proxy_lines(text);
    if (entries.size() != 1) throw std::runtime_error("invalid legacy proxy");
    Socks5Config proxy;
    proxy.host = entries[0].host;
    proxy.port = entries[0].port;
    proxy.username = entries[0].username;
    proxy.password = entries[0].password;
    if (proxy.host.empty() || proxy.port == 0) throw std::runtime_error("invalid legacy proxy");
    return proxy;
}

bool contains_legacy_delimiter(std::string_view value, bool colon_too = false) {
    return value.find_first_of(colon_too ? "|:\r\n" : "|\r\n") != std::string_view::npos;
}

std::optional<std::string> legacy_proxy_text(const Socks5Config& proxy) {
    if (proxy.host.empty() || proxy.port == 0 || contains_legacy_delimiter(proxy.host, true))
        return std::nullopt;
    if (proxy.username && contains_legacy_delimiter(*proxy.username, true)) return std::nullopt;
    if (proxy.password && contains_legacy_delimiter(*proxy.password, true)) return std::nullopt;
    if (proxy.username.has_value() != proxy.password.has_value()) return std::nullopt;

    std::string text = proxy.host + ":" + std::to_string(proxy.port);
    if (proxy.username) text += ":" + *proxy.username + ":" + *proxy.password;
    return text;
}

std::optional<std::string> legacy_record_line(const LaunchRecord& record, bool& lossy) {
    lossy = record.rotating_login_requested || record.proxy_policy != ProxyPolicy::Direct;
    if (record.credential.find_first_of("\r\n") != std::string::npos) return std::nullopt;
    if (record.credential_kind == LaunchCredentialKind::Ltoken) {
        // The current Bots-tab format stores a full provider record verbatim and
        // has nowhere to append proxy/policy metadata.
        return record.credential;
    }

    if (contains_legacy_delimiter(record.credential) || contains_legacy_delimiter(record.password))
        return std::nullopt;
    std::string proxy_text;
    if (record.proxy_policy == ProxyPolicy::Custom) {
        if (!record.custom_proxy) return std::nullopt;
        const auto encoded = legacy_proxy_text(*record.custom_proxy);
        if (!encoded) return std::nullopt;
        proxy_text = *encoded;
    }
    // cred|pass|mac|rid|hash|platform|otp|proxy (the exact existing UI shape).
    return record.credential + "|" + record.password + "||||0||" + proxy_text;
}

std::string display_name_for_path(const std::filesystem::path& path) {
    try {
        const auto stem = path.stem().string();
        return stem.empty() ? std::string("fleet") : stem;
    } catch (...) {
        return "fleet";
    }
}

FleetLoadResult restore_snapshot(const FleetSnapshot& snapshot, std::string name,
                                 FleetBackupFormat format, nxrth::bot::BotManager& manager,
                                 nxrth::proxy::ProxyPool& proxy_pool, FleetLoadOptions options,
                                 std::vector<FleetLoadIssue> initial_issues = {},
                                 std::size_t source_record_count = 0) {
    FleetLoadResult result;
    result.name = std::move(name);
    result.format = format;
    result.record_count = source_record_count == 0 ? snapshot.records.size() : source_record_count;
    result.issues = std::move(initial_issues);

    struct PreparedRecord {
        std::size_t index = 0;
        const LaunchRecord* record = nullptr;
        std::optional<Socks5Config> proxy;
        std::optional<nxrth::proxy::RotatingLoginProxy> rotating;
    };
    std::vector<PreparedRecord> prepared;
    prepared.reserve(snapshot.records.size());

    // Resolve every external dependency before a replacement can stop a live
    // bot. For replacement, capacity is calculated as if the current fleet had
    // already been released; merge mode includes current occupancy.
    nxrth::proxy::ActiveCounts planned_counts =
        options.replace_existing ? nxrth::proxy::ActiveCounts{}
                                 : manager.proxy_key_counts();
    for (std::size_t index = 0; index < snapshot.records.size(); ++index) {
        const auto& record = snapshot.records[index];
        PreparedRecord item;
        item.index = index;
        item.record = &record;
        std::optional<Socks5Config> proxy;
        try {
            switch (record.proxy_policy) {
                case ProxyPolicy::Direct: break;
                case ProxyPolicy::Pool:
                    proxy = proxy_pool.choose(planned_counts);
                    if (!proxy) throw std::runtime_error("pool unavailable");
                    break;
                case ProxyPolicy::Custom:
                    if (!record.custom_proxy) throw std::runtime_error("custom proxy missing");
                    proxy = record.custom_proxy;
                    break;
            }
        } catch (...) {
            result.issues.push_back(
                {index, record.id, record.proxy_policy == ProxyPolicy::Pool
                                       ? "game proxy pool is disabled or unavailable"
                                       : "custom proxy is invalid"});
            continue;
        }

        if (const auto key = nxrth::bot::proxy_key(proxy)) ++planned_counts[*key];
        item.proxy = std::move(proxy);

        if (record.rotating_login_requested) {
            try {
                item.rotating = proxy_pool.rotating_login_proxy();
            } catch (...) {
                item.rotating.reset();
            }
            if (!item.rotating) {
                result.issues.push_back(
                    {index, record.id, "rotating login proxy is disabled or unavailable"});
                continue;
            }
        }
        prepared.push_back(std::move(item));
    }

    // Replacement is all-or-nothing at the dependency/preflight boundary. A
    // malformed record or unavailable proxy must never wipe the current fleet.
    if (options.replace_existing && !result.issues.empty()) {
        result.error = "replacement cancelled because one or more records could not be prepared";
        result.launch_generation = manager.launch_generation();
        return result;
    }

    if (options.replace_existing) {
        const auto current = manager.list();
        for (const auto& bot : current) {
            if (manager.stop(bot.id)) ++result.stopped_count;
        }
    }

    for (auto& item : prepared) {
        const auto& record = *item.record;
        try {
            std::uint32_t new_id = 0;
            if (record.credential_kind == LaunchCredentialKind::Ltoken) {
                new_id = manager.spawn_ltoken(record.credential, std::move(item.proxy),
                                               std::move(item.rotating), record.proxy_policy);
            } else {
                new_id = manager.spawn(record.credential, record.password, std::move(item.proxy),
                                       std::move(item.rotating), record.proxy_policy);
            }
            result.new_bot_ids.push_back(new_id);
            result.id_remap.insert_or_assign(record.id, new_id);
            ++result.spawned_count;
        } catch (...) {
            result.issues.push_back({item.index, record.id, "bot spawn failed"});
        }
    }

    if (snapshot.has_automation && options.restore_automation) {
        manager.fleet()->set_config(remap_automation(snapshot.automation, result.id_remap));
        result.automation_restored = true;
    }

    result.launch_generation = manager.launch_generation();
    result.ok = result.issues.empty();
    if (!result.ok && result.spawned_count == 0) result.error = "no fleet records were restored";
    return result;
}

FleetLoadResult failed_load(std::string_view name, FleetBackupFormat format, std::string error) {
    FleetLoadResult result;
    result.name = std::string(name);
    result.format = format;
    result.error = std::move(error);
    return result;
}

}  // namespace

std::filesystem::path FleetStore::directory() {
    try {
        return (std::filesystem::current_path() / "data" / "fleets").lexically_normal();
    } catch (...) {
        return (std::filesystem::path(".") / "data" / "fleets").lexically_normal();
    }
}

bool FleetStore::valid_backup_name(std::string_view name) {
    if (name.empty() || name.size() > 80) return false;
    for (const unsigned char c : name) {
        if (!(std::isalnum(c) != 0 || c == '-' || c == '_')) return false;
    }
    return true;
}

std::optional<std::filesystem::path> FleetStore::backup_path(std::string_view name,
                                                             FleetBackupFormat format) {
    if (!valid_backup_name(name)) return std::nullopt;
    std::filesystem::path filename{std::string(name)};
    filename += format == FleetBackupFormat::Protected ? ".fleet" : ".txt";
    return directory() / filename;
}

std::vector<FleetBackupInfo> FleetStore::list() {
    std::vector<FleetBackupInfo> result;
    std::error_code ec;
    const auto root = directory();
    if (!std::filesystem::exists(root, ec)) return result;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto extension = it->path().extension().string();
        FleetBackupFormat format;
        if (extension == ".fleet")
            format = FleetBackupFormat::Protected;
        else if (extension == ".txt")
            format = FleetBackupFormat::LegacyText;
        else
            continue;
        const auto name = it->path().stem().string();
        if (!valid_backup_name(name)) continue;

        FleetBackupInfo info;
        info.name = name;
        info.format = format;
        info.file_size = it->file_size(ec);
        if (ec) {
            ec.clear();
            info.file_size = 0;
        }
        const auto file_time = it->last_write_time(ec);
        if (!ec) {
            const auto system_time = std::chrono::time_point_cast<std::chrono::milliseconds>(
                file_time - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            info.modified_unix_ms = system_time.time_since_epoch().count();
        } else {
            ec.clear();
        }
        result.push_back(std::move(info));
    }
    std::sort(result.begin(), result.end(), [](const FleetBackupInfo& a, const FleetBackupInfo& b) {
        if (a.name != b.name) return a.name < b.name;
        return static_cast<int>(a.format) < static_cast<int>(b.format);
    });
    return result;
}

FleetSaveResult FleetStore::save(std::string_view name,
                                 const nxrth::bot::BotManager& manager) {
    const auto path = backup_path(name);
    if (!path) {
        FleetSaveResult result;
        result.name = std::string(name);
        result.error = "invalid backup name";
        return result;
    }
    auto result = save_to_path(*path, FleetBackupFormat::Protected, manager);
    result.name = std::string(name);
    return result;
}

FleetSaveResult FleetStore::save_legacy_text(std::string_view name,
                                             const nxrth::bot::BotManager& manager) {
    const auto path = backup_path(name, FleetBackupFormat::LegacyText);
    if (!path) {
        FleetSaveResult result;
        result.name = std::string(name);
        result.error = "invalid backup name";
        return result;
    }
    auto result = save_to_path(*path, FleetBackupFormat::LegacyText, manager);
    result.name = std::string(name);
    return result;
}

FleetSaveResult FleetStore::save_to_path(const std::filesystem::path& path,
                                         FleetBackupFormat format,
                                         const nxrth::bot::BotManager& manager) {
    FleetSaveResult result;
    result.name = display_name_for_path(path);
    if (path.empty()) {
        result.error = "backup path is empty";
        return result;
    }

    try {
        FleetSnapshot snapshot;
        snapshot.launch_generation = manager.launch_generation();
        snapshot.records = manager.launch_records();
        snapshot.automation = manager.fleet()->config_snapshot();
        result.bot_count = snapshot.records.size();
        result.launch_generation = snapshot.launch_generation;

        std::vector<std::uint8_t> file;
        if (format == FleetBackupFormat::Protected) {
            const std::string plaintext = snapshot_to_json(snapshot).dump();
            std::vector<std::uint8_t> encrypted;
            if (!protect_bytes(plaintext, encrypted, result.error)) return result;
            file.reserve(kMagic.size() + encrypted.size());
            file.insert(file.end(), kMagic.begin(), kMagic.end());
            file.insert(file.end(), encrypted.begin(), encrypted.end());
        } else {
            std::string plaintext;
            for (const auto& record : snapshot.records) {
                bool lossy = false;
                const auto line = legacy_record_line(record, lossy);
                if (!line) {
                    result.error =
                        "a bot record cannot be represented by legacy TXT; use protected format";
                    return result;
                }
                if (lossy) ++result.lossy_record_count;
                plaintext += *line;
                plaintext.push_back('\n');
            }
            file.assign(plaintext.begin(), plaintext.end());
        }
        if (!atomic_write(path, file, result.error)) return result;

        result.encrypted_bytes = file.size();
        result.ok = true;
        return result;
    } catch (...) {
        result.error = "fleet backup could not be serialized";
        return result;
    }
}

FleetLoadResult FleetStore::load(std::string_view name, nxrth::bot::BotManager& manager,
                                 nxrth::proxy::ProxyPool& proxy_pool,
                                 FleetLoadOptions options) {
    const auto path = backup_path(name);
    if (!path) return failed_load(name, FleetBackupFormat::Protected, "invalid backup name");

    std::error_code exists_error;
    if (!std::filesystem::exists(*path, exists_error)) {
        const auto legacy = backup_path(name, FleetBackupFormat::LegacyText);
        if (legacy && std::filesystem::exists(*legacy, exists_error))
            return load_legacy_text(name, manager, proxy_pool, options);
    }
    auto result = load_from_path(*path, FleetBackupFormat::Protected, manager, proxy_pool, options);
    result.name = std::string(name);
    return result;
}

FleetLoadResult FleetStore::load_legacy_text(std::string_view name,
                                             nxrth::bot::BotManager& manager,
                                             nxrth::proxy::ProxyPool& proxy_pool,
                                             FleetLoadOptions options) {
    const auto path = backup_path(name, FleetBackupFormat::LegacyText);
    if (!path) return failed_load(name, FleetBackupFormat::LegacyText, "invalid backup name");

    auto result = load_from_path(*path, FleetBackupFormat::LegacyText, manager, proxy_pool, options);
    result.name = std::string(name);
    return result;
}

FleetLoadResult FleetStore::load_from_path(const std::filesystem::path& path,
                                           FleetBackupFormat format,
                                           nxrth::bot::BotManager& manager,
                                           nxrth::proxy::ProxyPool& proxy_pool,
                                           FleetLoadOptions options) {
    const std::string display_name = display_name_for_path(path);
    if (path.empty()) return failed_load(display_name, format, "backup path is empty");

    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!read_file(path, bytes, error)) return failed_load(display_name, format, std::move(error));

    if (format == FleetBackupFormat::Protected) {
        if (bytes.size() <= kMagic.size() ||
            !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
            return failed_load(display_name, format, "invalid fleet backup header");

        std::string plaintext;
        if (!unprotect_bytes(std::span(bytes).subspan(kMagic.size()), plaintext, error))
            return failed_load(display_name, format, std::move(error));
        try {
            const FleetSnapshot snapshot = snapshot_from_json(json::parse(plaintext));
            return restore_snapshot(snapshot, display_name, format, manager, proxy_pool, options);
        } catch (...) {
            return failed_load(display_name, format,
                               "fleet backup data is invalid or unsupported");
        }
    }

    FleetSnapshot snapshot;
    snapshot.has_automation = false;
    std::vector<FleetLoadIssue> issues;
    std::size_t source_records = 0;
    std::istringstream input(
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    std::string raw;
    std::size_t line_index = 0;
    while (std::getline(input, raw)) {
        ++line_index;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const std::string line = trim_ascii(std::move(raw));
        if (line.empty()) continue;
        ++source_records;

        LaunchRecord record;
        record.id = static_cast<std::uint32_t>(line_index);
        record.proxy_policy = proxy_pool.config().enabled ? ProxyPolicy::Pool : ProxyPolicy::Direct;
        record.rotating_login_requested = proxy_pool.config().rotating_login_enabled;
        try {
            if (nxrth::bot::parse_ltoken_string(line)) {
                record.credential_kind = LaunchCredentialKind::Ltoken;
                record.credential = line;
            } else {
                const auto fields = split_pipe(line);
                const auto get = [&](std::size_t index) -> std::string {
                    return index < fields.size() ? fields[index] : std::string{};
                };
                const std::string credential = get(0);
                const std::string password = get(1);
                if (credential.empty()) throw std::runtime_error("missing credential");
                if (!password.empty()) {
                    record.credential_kind = LaunchCredentialKind::GrowId;
                    record.credential = credential;
                    record.password = password;
                } else {
                    const std::string mac = get(2);
                    const std::string rid = get(3);
                    if (mac.empty() || rid.size() != 32)
                        throw std::runtime_error("invalid provider fields");
                    record.credential_kind = LaunchCredentialKind::Ltoken;
                    record.credential = "token:" + credential + "|rid:" + rid + "|mac:" + mac +
                                        "|wk:NONE0";
                    if (!nxrth::bot::parse_ltoken_string(record.credential))
                        throw std::runtime_error("invalid provider record");
                }

                const auto proxy = legacy_proxy(get(7));
                if (proxy) {
                    record.proxy_policy = ProxyPolicy::Custom;
                    record.custom_proxy = proxy;
                }
            }
            snapshot.records.push_back(std::move(record));
        } catch (...) {
            issues.push_back({line_index - 1, static_cast<std::uint32_t>(line_index),
                              "legacy bot record is invalid"});
        }
    }

    return restore_snapshot(snapshot, display_name, FleetBackupFormat::LegacyText, manager,
                            proxy_pool, options, std::move(issues), source_records);
}

bool FleetStore::self_test(std::string* error) {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    try {
        FleetSnapshot source;
        source.launch_generation = 42;
        LaunchRecord record;
        record.id = 7;
        record.credential_kind = LaunchCredentialKind::Ltoken;
        record.credential = std::string("fake\0provider\xff|token", 20);
        record.password = std::string("fake\0password", 13);
        record.proxy_policy = ProxyPolicy::Custom;
        record.rotating_login_requested = true;
        Socks5Config proxy;
        proxy.host = std::string("127.0.0.1\0exact", 16);
        proxy.port = 1080;
        proxy.username = std::string("fake\0user", 9);
        proxy.password = std::string("fake\0proxy-secret", 17);
        record.custom_proxy = proxy;
        source.records.push_back(record);
        source.automation.enabled["geiger"] = true;
        source.automation.params[std::string("binary\0key", 10)] =
            std::string("binary\0value\xff", 13);
        source.automation.groups["workers"] = {7, 999, 8, 7};
        source.automation.module_bot_ids["geiger"] = {8, 7, 999};
        source.automation.module_groups["geiger"] = "workers";

        const std::string plaintext = snapshot_to_json(source).dump();
        std::vector<std::uint8_t> encrypted;
        std::string detail;
        if (!protect_bytes(plaintext, encrypted, detail)) return fail("DPAPI protect failed");
        std::string decrypted;
        if (!unprotect_bytes(encrypted, decrypted, detail)) return fail("DPAPI unprotect failed");
        if (decrypted != plaintext) return fail("DPAPI exact-byte roundtrip failed");

        const auto restored = snapshot_from_json(json::parse(decrypted));
        if (restored.records.size() != 1) return fail("record roundtrip count failed");
        const auto& actual = restored.records.front();
        if (actual.id != record.id || actual.credential_kind != record.credential_kind ||
            actual.credential != record.credential || actual.password != record.password ||
            actual.proxy_policy != record.proxy_policy ||
            actual.rotating_login_requested != record.rotating_login_requested ||
            !actual.custom_proxy || actual.custom_proxy->host != proxy.host ||
            actual.custom_proxy->port != proxy.port ||
            actual.custom_proxy->username != proxy.username ||
            actual.custom_proxy->password != proxy.password)
            return fail("launch record exact-byte roundtrip failed");
        if (restored.automation.params != source.automation.params)
            return fail("automation exact-byte roundtrip failed");

        const std::unordered_map<std::uint32_t, std::uint32_t> mapping{{7, 70}, {8, 80}};
        const auto remapped = remap_automation(restored.automation, mapping);
        const std::vector<std::uint32_t> expected_group{70, 80};
        const std::vector<std::uint32_t> expected_module{80, 70};
        if (remapped.groups.at("workers") != expected_group ||
            remapped.module_bot_ids.at("geiger") != expected_module)
            return fail("automation id remap failed");
        return true;
    } catch (...) {
        return fail("fleet store self-test threw");
    }
}

}  // namespace nxrth::recovery
