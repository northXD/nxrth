#include "mcp/mcp_server.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "ai/ai_controller.h"
#include "core/accounts.h"
#include "core/logger.h"
#include "lua/lua_engine.h"
#include "mcp/app_bridge.h"
#include "recovery/fleet_store.h"
#include "recovery/recovery_controller.h"
#include "script/script_store.h"
#include "world/items.h"
#include "world/world.h"

namespace nxrth::mcp {
namespace {

using json = nlohmann::json;
using nxrth::bot::BotCommand;
namespace cmd = nxrth::bot::cmd;

constexpr std::size_t kMaxAccountsInputBytes = 4'000'000;
constexpr std::size_t kMaxAccountsSpawn = 5'000;

std::string public_proxy_label(std::string label) {
    const auto credential_suffix = label.rfind(" (");
    if (credential_suffix != std::string::npos && label.ends_with(')'))
        label.resize(credential_suffix);
    return label;
}

json object_schema(json properties, json required = json::array()) {
    if (properties.is_null()) properties = json::object();
    return {{"type", "object"}, {"properties", std::move(properties)},
            {"required", std::move(required)}, {"additionalProperties", false}};
}

json tool(std::string name, std::string description, json schema) {
    return {{"name", std::move(name)}, {"description", std::move(description)},
            {"inputSchema", std::move(schema)}};
}

json bot_id_property() {
    return {{"type", "integer"}, {"minimum", 0},
            {"description", "Bot identifier returned by session_login or session_list."}};
}

std::uint32_t required_u32(const json& args, const char* key,
                           std::uint32_t max = std::numeric_limits<std::uint32_t>::max()) {
    if (!args.contains(key) || !args[key].is_number_integer())
        throw std::invalid_argument(std::string("'" ) + key + "' must be an integer");
    const auto value = args[key].get<std::int64_t>();
    if (value < 0 || static_cast<std::uint64_t>(value) > max)
        throw std::invalid_argument(std::string("'" ) + key + "' is out of range");
    return static_cast<std::uint32_t>(value);
}

std::int32_t required_i32(const json& args, const char* key) {
    if (!args.contains(key) || !args[key].is_number_integer())
        throw std::invalid_argument(std::string("'" ) + key + "' must be an integer");
    const auto value = args[key].get<std::int64_t>();
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max())
        throw std::invalid_argument(std::string("'" ) + key + "' is out of range");
    return static_cast<std::int32_t>(value);
}

std::string required_string(const json& args, const char* key, std::size_t max_length) {
    if (!args.contains(key) || !args[key].is_string())
        throw std::invalid_argument(std::string("'" ) + key + "' must be a string");
    auto value = args[key].get<std::string>();
    if (value.empty() || value.size() > max_length)
        throw std::invalid_argument(std::string("'" ) + key + "' length is invalid");
    return value;
}

json success(json data) {
    return {{"content", json::array({{{"type", "text"}, {"text", data.dump()}}})},
            {"structuredContent", std::move(data)}, {"isError", false}};
}

json structured_tool_result(json data, bool is_error) {
    const std::string text = data.dump();
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})},
            {"structuredContent", std::move(data)}, {"isError", is_error}};
}

json failure(std::string message) {
    return {{"content", json::array({{{"type", "text"}, {"text", message}}})},
            {"isError", true}};
}

json rpc_error(const json& id, int code, std::string message) {
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", {{"code", code}, {"message", std::move(message)}}}};
}

json state_summary(std::uint32_t id, const nxrth::bot::BotState& state) {
    const std::string status = nxrth::bot::to_string(state.status);
    const bool connected = state.status == nxrth::bot::BotStatus::Connected ||
                           state.status == nxrth::bot::BotStatus::InGame;
    return {{"bot_id", id}, {"nickname", state.username}, {"status", status},
            {"connected", connected}, {"world", state.world_name},
            {"position", {{"x", state.pos_x}, {"y", state.pos_y}, {"unit", "tiles"}}},
            {"ping_ms", state.ping_ms}, {"gems", state.gems}};
}

json item_json(const nxrth::bot::InvSlot& slot,
               const std::shared_ptr<const nxrth::world::ItemsDat>& items) {
    json out = {{"item_id", slot.item_id}, {"amount", slot.amount},
                {"active", slot.is_active}, {"action_type", slot.action_type}};
    if (items) {
        if (const auto* info = items->find_by_id(slot.item_id)) out["name"] = info->name;
    }
    return out;
}

bool known_automation_module(const std::string& module) {
    return module == "geiger" || module == "collect" || module == "coordinate";
}

json supported_automation_json() {
    return {
        {"modules", json::array({"geiger", "collect", "coordinate"})},
        {"params",
         {
             {"geiger",
              {
                  {"geiger_hunt_worlds", "Hunt/search worlds, comma/newline separated. Door id can be WORLD|door."},
                  {"geiger_depot_worlds", "Depot worlds used when inventory needs deposit."},
                  {"geiger_pickup_worlds", "Worlds where a Geiger Counter can be picked up when missing."},
                  {"geiger_item", "Charged Geiger Counter item id. Default 2204."},
                  {"geiger_wear", "1/0, auto-wear the counter. Default 1."},
                  {"geiger_dig", "1/0, punch the target tile after prize signal. Default 1."},
                  {"geiger_recharge_min", "Minutes to wait after a prize while the counter recharges. Default 30."},
                  {"geiger_min_y", "Search grid minimum Y. Default 0."},
                  {"geiger_max_y", "Search grid maximum Y. Default 53."},
                  {"geiger_world_width", "Search grid width cap. Default 100."},
                  {"geiger_signal_wait_ms", "Milliseconds to wait for a fresh geiger particle per probe. Default 4200."},
                  {"geiger_settle_ms", "Milliseconds to settle on a probe tile before measuring. Default 700."},
                  {"geiger_max_steps", "Maximum probe steps per hunt. Default 70."},
                  {"geiger_pickup_scan_ms", "Pickup depot rescan interval in milliseconds. Default 3000."},
                  {"geiger_pickup_empty_scans", "Empty scans before rotating to another pickup depot. Default 12."},
                  {"geiger_webhook_url", "Optional Discord webhook URL for geiger fleet logs."},
              }},
             {"collect", {{"collect_radius", "Pickup radius in tiles, clamped 1..5. Default 3."}}},
             {"coordinate", {{"coordinate_worlds", "Fleet spread worlds, comma/newline separated. Door id can be WORLD|door."}}},
         }},
    };
}

json lua_api_reference_json() {
    return {
        {"runtime",
         {{"version", "Lua 5.4"},
          {"state", "fresh state per execution"},
          {"instruction_limit", 20'000'000},
          {"time_limit_ms", 30'000},
          {"memory_limit_bytes", 64 * 1024 * 1024},
          {"output_limit_bytes", 512 * 1024},
          {"libraries", json::array({"base", "coroutine", "table", "string", "math", "utf8"})},
          {"unavailable", json::array({"os", "io", "package", "debug", "dofile", "loadfile"})}}},
        {"globals",
         json::array({
             "TOKEN",
             "calculateBackpackCost",
             "addBot(table) -> ok, bot_id_or_error",
             "getBot(selector)",
             "getBots()",
             "removeBot(selector)",
             "print(...) (secret-redacted)",
             "sleep(milliseconds)",
         })},
        {"addBot_fields",
         json::array({"token", "refreshToken", "rid", "mac", "wk", "platform",
                      "platformID", "name", "cbits", "playerAge", "player_age", "vid",
                      "proxy", "connect", "type", "rotatingLogin", "username", "growid",
                      "password"})},
        {"autogeiger",
         json::array({
             "enable([bool])", "disable()", "status()", "getconfig()",
             "addworld(world)", "setworlds({worlds})", "removeworld(world)",
             "clearworlds()", "getworlds()", "addstorageworld(world)",
             "setstorageworlds({worlds})", "removestorageworld(world)",
             "clearstorageworlds()", "getstorageworlds()",
             "addgeigerstorageworld(world)", "setgeigerstorageworlds({worlds})",
             "removegeigerstorageworld(world)", "cleargeigerstorageworlds()",
             "getgeigerstorageworlds()", "getSignal(bot)", "setoption(key, value)",
         })},
        {"automation",
         json::array({"enable(module, bool)", "setparam(key, value)", "getparam(key)",
                      "setbots(module, {selectors})", "allbots(module)"})},
        {"bot_state",
         json::array({"list", "status", "find", "getLocal", "getPing", "getSignal",
                      "isInWorld", "isInTile", "isWearing", "waitOnline", "getWorld",
                      "getPlayers", "getInventory", "getFloatingItems", "getObjects",
                      "getTiles", "getTile", "getConsole", "chatlog"})},
        {"bot_actions",
         json::array({"connect", "reconnect", "disconnect", "remove", "leaveWorld",
                      "respawn", "warp", "say", "chat", "enter", "acceptAccess", "move",
                      "moveTile", "moveTo", "moveLeft", "moveRight", "moveUp", "moveDown",
                      "walk", "findPath", "setDirection", "place", "punch", "hit", "wrench",
                      "wrenchPlayer", "activateTile", "active", "wear", "use", "unwear",
                      "drop", "trash", "activateItem", "collect", "collectObject",
                      "setAutoCollect", "auto_collect", "autoreconnect"})},
        {"notes",
         json::array({
             "Every bot function accepts a numeric bot id or case-insensitive bot name.",
             "Bot actions enqueue work; inspect state to confirm completion.",
             "proxy='auto' requires a configured game proxy and never falls back to the real IP.",
             "AutoGeiger changes update and persist the fleet-wide shared automation config.",
             "Use docs/LUA.md for detailed signatures and examples when repository files are available.",
         })},
    };
}

bool sensitive_automation_param(std::string_view key) {
    std::string folded(key);
    std::transform(folded.begin(), folded.end(), folded.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return folded.find("webhook") != std::string::npos ||
           folded.find("token") != std::string::npos ||
           folded.find("password") != std::string::npos ||
           folded.find("secret") != std::string::npos ||
           folded.find("proxy") != std::string::npos ||
           folded.find("authorization") != std::string::npos;
}

json automation_config_data_json(const nxrth::bot::AutomationConfig& cfg,
                                 bool redact_sensitive = false) {
    json enabled = json::object();
    for (const auto& [key, value] : cfg.enabled) enabled[key] = value;
    json params = json::object();
    for (const auto& [key, value] : cfg.params) {
        params[key] = redact_sensitive && sensitive_automation_param(key)
                          ? json("<redacted>")
                          : json(value);
    }
    json groups = json::object();
    for (const auto& [name, ids] : cfg.groups) groups[name] = ids;
    json scopes = json::object();
    for (const auto& [module, ids] : cfg.module_bot_ids) {
        if (!known_automation_module(module)) continue;
        scopes[module]["bot_ids"] = ids;
    }
    for (const auto& [module, group] : cfg.module_groups) {
        if (!known_automation_module(module)) continue;
        scopes[module]["group"] = group;
    }
    return {{"enabled", std::move(enabled)}, {"params", std::move(params)},
            {"groups", std::move(groups)}, {"scopes", std::move(scopes)}};
}

json automation_config_json(const nxrth::bot::AutomationConfig& cfg) {
    json out = automation_config_data_json(cfg, true);
    out["supported"] = supported_automation_json();
    out["secrets_returned"] = false;
    return out;
}

std::vector<std::uint32_t> parse_bot_id_array(const json& value, const char* key) {
    if (!value.is_array()) throw std::invalid_argument(std::string("'") + key + "' must be an array");
    std::vector<std::uint32_t> ids;
    for (const auto& raw : value) {
        if (!raw.is_number_integer())
            throw std::invalid_argument(std::string("'") + key + "' values must be integers");
        const auto id = raw.get<std::int64_t>();
        if (id < 0 || id > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument(std::string("'") + key + "' value is out of range");
        ids.push_back(static_cast<std::uint32_t>(id));
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::string required_profile_name(const json& args) {
    std::string name = required_string(args, "name", 64);
    for (char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) throw std::invalid_argument("'name' may contain only letters, digits, '_' and '-'");
    }
    return name;
}

std::filesystem::path automation_profiles_path() {
    return std::filesystem::current_path() / "data" / "automation_profiles.json";
}

json read_profiles_file() {
    const auto path = automation_profiles_path();
    if (!std::filesystem::exists(path)) return json::object();
    std::ifstream in(path);
    if (!in) throw std::runtime_error("automation profiles file cannot be opened");
    json profiles = json::parse(in, nullptr, true, true);
    if (!profiles.is_object()) throw std::runtime_error("automation profiles file must be an object");
    return profiles;
}

void write_profiles_file(const json& profiles) {
    const auto path = automation_profiles_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    if (!out) throw std::runtime_error("automation profiles file cannot be written");
    out << profiles.dump(2);
}

std::string scalar_to_param_string(const json& value);

void automation_config_from_json(const json& j, nxrth::bot::AutomationConfig& cfg) {
    cfg = {};
    if (j.contains("enabled")) {
        if (!j["enabled"].is_object()) throw std::invalid_argument("'enabled' must be an object");
        for (const auto& [module, value] : j["enabled"].items()) {
            if (!known_automation_module(module) || !value.is_boolean()) continue;
            cfg.enabled[module] = value.get<bool>();
        }
    }
    if (j.contains("params")) {
        if (!j["params"].is_object()) throw std::invalid_argument("'params' must be an object");
        for (const auto& [key, value] : j["params"].items()) {
            if (key.empty() || key.size() > 64) continue;
            cfg.params[key] = scalar_to_param_string(value);
        }
    }
    if (j.contains("groups")) {
        if (!j["groups"].is_object()) throw std::invalid_argument("'groups' must be an object");
        for (const auto& [name, ids] : j["groups"].items()) {
            if (name.empty() || name.size() > 64) continue;
            cfg.groups[name] = parse_bot_id_array(ids, "groups");
        }
    }
    if (j.contains("scopes")) {
        if (!j["scopes"].is_object()) throw std::invalid_argument("'scopes' must be an object");
        for (const auto& [module, scope] : j["scopes"].items()) {
            if (!known_automation_module(module) || !scope.is_object()) continue;
            if (scope.contains("bot_ids"))
                cfg.module_bot_ids[module] = parse_bot_id_array(scope["bot_ids"], "bot_ids");
            if (scope.contains("group") && scope["group"].is_string())
                cfg.module_groups[module] = scope["group"].get<std::string>();
        }
    }
}

std::string scalar_to_param_string(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "1" : "0";
    if (value.is_number_integer() || value.is_number_unsigned() || value.is_number_float())
        return value.dump();
    throw std::invalid_argument("automation param values must be strings, numbers, or booleans");
}

void set_string_param_if_present(nxrth::bot::AutomationConfig& cfg, const json& args,
                                 const char* arg_key, const char* param_key,
                                 std::size_t max_length) {
    if (!args.contains(arg_key)) return;
    if (!args[arg_key].is_string())
        throw std::invalid_argument(std::string("'") + arg_key + "' must be a string");
    std::string value = args[arg_key].get<std::string>();
    if (value.size() > max_length)
        throw std::invalid_argument(std::string("'") + arg_key + "' length is invalid");
    cfg.params[param_key] = std::move(value);
}

void set_int_param_if_present(nxrth::bot::AutomationConfig& cfg, const json& args,
                              const char* arg_key, const char* param_key,
                              std::int64_t min_value, std::int64_t max_value) {
    if (!args.contains(arg_key)) return;
    if (!args[arg_key].is_number_integer())
        throw std::invalid_argument(std::string("'") + arg_key + "' must be an integer");
    const auto value = args[arg_key].get<std::int64_t>();
    if (value < min_value || value > max_value)
        throw std::invalid_argument(std::string("'") + arg_key + "' is out of range");
    cfg.params[param_key] = std::to_string(value);
}

void set_bool_param_if_present(nxrth::bot::AutomationConfig& cfg, const json& args,
                               const char* arg_key, const char* param_key) {
    if (!args.contains(arg_key)) return;
    if (!args[arg_key].is_boolean())
        throw std::invalid_argument(std::string("'") + arg_key + "' must be a boolean");
    cfg.params[param_key] = args[arg_key].get<bool>() ? "1" : "0";
}

std::string trim_copy(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string lower_ascii(std::string value) {
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}

std::string redact_log_line(std::string line) {
    struct SecretField {
        std::string_view marker;
        bool pipe_delimited;
    };
    const SecretField fields[] = {
        {"refreshtoken:", true}, {"ltoken:", true},       {"token:", true},
        {"ltoken|", false},      {"token|", false},       {"requestedname|", false},
        {"tankidpass|", false},  {"password|", false},
    };
    for (const auto& field : fields) {
        std::size_t search_from = 0;
        while (search_from < line.size()) {
            const std::string lowered = lower_ascii(line);
            const auto pos = lowered.find(field.marker, search_from);
            if (pos == std::string::npos) break;
            if (field.marker == "token|" && pos > 0 &&
                std::isalnum(static_cast<unsigned char>(lowered[pos - 1]))) {
                search_from = pos + field.marker.size();
                continue;
            }
            const auto value_begin = pos + field.marker.size();
            auto value_end = line.find_first_of(field.pipe_delimited ? "|\r\n" : "\r\n",
                                                value_begin);
            if (value_end == std::string::npos) value_end = line.size();
            line.replace(value_begin, value_end - value_begin, " <redacted>");
            search_from = value_begin + std::string_view(" <redacted>").size();
        }
    }

    // Defense in depth for HTTP/SOCKS URLs emitted by other subsystems.
    std::size_t search_from = 0;
    while (true) {
        const auto scheme = line.find("://", search_from);
        if (scheme == std::string::npos) break;
        const auto authority = scheme + 3;
        const auto end = line.find_first_of(" /\\\"')", authority);
        const auto at = line.find('@', authority);
        if (at != std::string::npos && (end == std::string::npos || at < end)) {
            line.replace(authority, at - authority + 1, "<redacted>@");
            search_from = authority + std::string_view("<redacted>@").size();
        } else {
            search_from = authority;
        }
    }
    return line;
}

bool blank_param(const nxrth::bot::AutomationConfig& cfg, const std::string& key) {
    return trim_copy(cfg.param(key)).empty();
}

int int_param_or(const nxrth::bot::AutomationConfig& cfg, const std::string& key,
                 int fallback) {
    try {
        std::size_t used = 0;
        const std::string raw = trim_copy(cfg.param(key, std::to_string(fallback)));
        int value = std::stoi(raw, &used);
        if (used != raw.size()) return fallback;
        return value;
    } catch (...) {
        return fallback;
    }
}

std::size_t world_list_count(const std::string& raw) {
    std::size_t count = 0;
    std::string cur;
    for (char c : raw) {
        if (c == ',' || c == '\n' || c == '\r' || c == ';') {
            if (!trim_copy(cur).empty()) ++count;
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!trim_copy(cur).empty()) ++count;
    return count;
}

json automation_validation_json(const nxrth::bot::AutomationConfig& cfg) {
    json issues = json::array();
    json warnings = json::array();
    json preview = json::object();

    for (const auto& [module, group] : cfg.module_groups) {
        if (!group.empty() && cfg.groups.find(group) == cfg.groups.end())
            issues.push_back("module '" + module + "' references missing group '" + group + "'");
    }

    if (cfg.is_on("geiger")) {
        if (blank_param(cfg, "geiger_hunt_worlds"))
            issues.push_back("geiger_hunt_worlds is required when geiger is enabled");
        if (blank_param(cfg, "geiger_depot_worlds"))
            warnings.push_back("geiger_depot_worlds is empty; full inventory cannot be deposited");
        if (blank_param(cfg, "geiger_pickup_worlds"))
            warnings.push_back("geiger_pickup_worlds is empty; bots without a counter cannot recover");

        const int min_y = int_param_or(cfg, "geiger_min_y", 0);
        const int max_y = int_param_or(cfg, "geiger_max_y", 53);
        if (max_y <= min_y) issues.push_back("geiger_max_y must be greater than geiger_min_y");
        if (int_param_or(cfg, "geiger_signal_wait_ms", 4200) < 500)
            issues.push_back("geiger_signal_wait_ms must be at least 500");
        if (int_param_or(cfg, "geiger_max_steps", 70) < 1)
            issues.push_back("geiger_max_steps must be at least 1");

        preview["geiger"] = {
            {"hunt_world_count", world_list_count(cfg.param("geiger_hunt_worlds"))},
            {"depot_world_count", world_list_count(cfg.param("geiger_depot_worlds"))},
            {"pickup_world_count", world_list_count(cfg.param("geiger_pickup_worlds"))},
            {"item_id", int_param_or(cfg, "geiger_item", 2204)},
            {"wear", cfg.param("geiger_wear", "1") != "0"},
            {"dig", cfg.param("geiger_dig", "1") != "0"},
            {"recharge_min", int_param_or(cfg, "geiger_recharge_min", 30)},
            {"min_y", min_y},
            {"max_y", max_y},
            {"world_width", int_param_or(cfg, "geiger_world_width", 100)},
            {"signal_wait_ms", int_param_or(cfg, "geiger_signal_wait_ms", 4200)},
            {"max_steps", int_param_or(cfg, "geiger_max_steps", 70)},
        };
    }

    if (cfg.is_on("collect")) {
        const int radius = int_param_or(cfg, "collect_radius", 3);
        if (radius < 1 || radius > 5) warnings.push_back("collect_radius is clamped to 1..5");
    }
    if (cfg.is_on("coordinate") && blank_param(cfg, "coordinate_worlds"))
        warnings.push_back("coordinate_worlds is empty; coordinate module has no targets");

    return {{"valid", issues.empty()}, {"issues", std::move(issues)},
            {"warnings", std::move(warnings)}, {"preview", std::move(preview)}};
}

std::uint32_t inventory_amount(const nxrth::bot::BotState& state, std::uint16_t item_id) {
    std::uint32_t total = 0;
    for (const auto& slot : state.inventory)
        if (slot.item_id == item_id) total += slot.amount;
    return total;
}

json geiger_signal_json(const std::optional<nxrth::bot::GeigerSignal>& sig) {
    if (!sig) return nullptr;
    return {{"x", sig->x}, {"y", sig->y}, {"type", nxrth::bot::as_str(sig->area_type)},
            {"timestamp_ms", sig->timestamp_ms}};
}

std::string geiger_phase_for_bot(const nxrth::bot::AutomationConfig& cfg,
                                 const nxrth::bot::BotState& state,
                                 std::uint32_t bot_id) {
    if (!cfg.is_on_for("geiger", bot_id)) return "disabled";
    if (blank_param(cfg, "geiger_hunt_worlds")) return "blocked:no_hunt_worlds";
    if (state.status != nxrth::bot::BotStatus::InGame) return "waiting_for_world";
    const auto item = static_cast<std::uint16_t>(
        std::clamp(int_param_or(cfg, "geiger_item", 2204), 1, 65535));
    const std::uint32_t charged = inventory_amount(state, item);
    const std::uint32_t dead = inventory_amount(state, 2286);
    const bool full = state.inventory_size > 0 && state.inventory.size() >= state.inventory_size;
    if (full && blank_param(cfg, "geiger_depot_worlds")) return "blocked:no_depot_worlds";
    if (charged == 0 && dead == 0 && blank_param(cfg, "geiger_pickup_worlds"))
        return "blocked:no_counter_pickup_worlds";
    if (charged == 0 && dead > 0) return "recharging";
    if (charged == 0) return "pickup";
    if (full) return "deposit";
    return "hunting";
}

json floating_object_json(std::uint32_t bot_id, const nxrth::bot::BotState& state,
                          const nxrth::bot::WorldObjectInfo& o,
                          const std::shared_ptr<const nxrth::world::ItemsDat>& items) {
    json obj = {{"bot_id", bot_id}, {"uid", o.uid}, {"item_id", o.item_id}, {"count", o.count},
                {"world", state.world_name},
                {"position_pixels", {{"x", o.x}, {"y", o.y}}},
                {"position_tiles", {{"x", o.x / 32.0f}, {"y", o.y / 32.0f}}}};
    const float dx = state.pos_x - (o.x / 32.0f);
    const float dy = state.pos_y - (o.y / 32.0f);
    obj["distance_tiles"] = std::sqrt(dx * dx + dy * dy);
    if (items) {
        if (const auto* info = items->find_by_id(o.item_id)) obj["name"] = info->name;
    }
    return obj;
}

std::optional<std::pair<std::uint32_t, std::string>> parse_bot_uri(const std::string& uri) {
    constexpr std::string_view prefix = "nxrth://bot/";
    if (!std::string_view(uri).starts_with(prefix)) return std::nullopt;
    const std::string tail = uri.substr(prefix.size());
    const auto slash = tail.find('/');
    if (slash == std::string::npos || slash == 0) return std::nullopt;
    try {
        std::size_t used = 0;
        const auto raw = std::stoull(tail.substr(0, slash), &used);
        if (used != slash || raw > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
        return std::make_pair(static_cast<std::uint32_t>(raw), tail.substr(slash + 1));
    } catch (...) {
        return std::nullopt;
    }
}

const char* fleet_format_name(nxrth::recovery::FleetBackupFormat format) {
    return format == nxrth::recovery::FleetBackupFormat::Protected ? "protected"
                                                                    : "legacy_txt";
}

nxrth::recovery::FleetBackupFormat required_fleet_format(const json& args,
                                                           const char* default_value) {
    const std::string format = args.value("format", std::string(default_value));
    if (format == "protected") return nxrth::recovery::FleetBackupFormat::Protected;
    if (format == "legacy_txt") return nxrth::recovery::FleetBackupFormat::LegacyText;
    throw std::invalid_argument("format must be protected or legacy_txt");
}

json fleet_save_result_json(const nxrth::recovery::FleetSaveResult& result,
                            nxrth::recovery::FleetBackupFormat format) {
    return {
        {"saved", result.ok},
        {"name", result.name},
        {"format", fleet_format_name(format)},
        {"bot_count", result.bot_count},
        {"launch_generation", result.launch_generation},
        {"stored_bytes", result.encrypted_bytes},
        {"lossy_record_count", result.lossy_record_count},
        {"secrets_preserved", result.ok},
        {"secrets_returned", false},
        {"error", result.error},
    };
}

json fleet_load_result_json(const nxrth::recovery::FleetLoadResult& result) {
    json issues = json::array();
    for (const auto& issue : result.issues) {
        issues.push_back({
            {"record_index", issue.record_index},
            {"old_bot_id", issue.old_bot_id ? json(*issue.old_bot_id) : json(nullptr)},
            {"error", issue.error},
        });
    }
    json id_remap = json::array();
    for (const auto& [old_id, new_id] : result.id_remap)
        id_remap.push_back({{"old_bot_id", old_id}, {"new_bot_id", new_id}});
    return {
        {"loaded", result.ok},
        {"name", result.name},
        {"format", fleet_format_name(result.format)},
        {"record_count", result.record_count},
        {"stopped_count", result.stopped_count},
        {"spawned_count", result.spawned_count},
        {"automation_restored", result.automation_restored},
        {"launch_generation", result.launch_generation},
        {"new_bot_ids", result.new_bot_ids},
        {"id_remap", std::move(id_remap)},
        {"issues", std::move(issues)},
        {"secrets_preserved", true},
        {"secrets_returned", false},
        {"error", result.error},
    };
}

json script_metadata_json(const nxrth::script::ScriptMetadata& metadata) {
    return {{"name", metadata.name},
            {"size_bytes", metadata.size_bytes},
            {"fingerprint", metadata.fingerprint}};
}

std::optional<nxrth::script::ScriptMetadata> find_script_metadata(
    const nxrth::script::ScriptStore& store, std::string_view name, std::string* error) {
    const std::filesystem::path requested{std::string(name)};
    const std::string canonical = requested.extension().empty()
                                      ? requested.filename().string() + ".lua"
                                      : requested.filename().string();
    auto scripts = store.list(error);
    if (error && !error->empty()) return std::nullopt;
    const std::string folded = lower_ascii(canonical);
    const auto it = std::find_if(scripts.begin(), scripts.end(), [&](const auto& metadata) {
        return lower_ascii(metadata.name) == folded;
    });
    if (it == scripts.end()) {
        if (error) *error = "script metadata is unavailable";
        return std::nullopt;
    }
    return *it;
}

json recovery_status_json(const nxrth::recovery::RecoveryStatus& status) {
    return {
        {"configured", status.configured},
        {"enabled", status.enabled},
        {"mode", nxrth::recovery::to_string(status.mode)},
        {"fleet_name", status.fleet_name},
        {"script_name", status.script_name ? json(*status.script_name) : json(nullptr)},
        {"restart_delay_ms", status.restart_delay_ms},
        {"max_restarts", status.max_restarts},
        {"window_seconds", status.window_seconds},
        {"supervisor_pid", status.supervisor_pid},
        {"supervisor_running", status.supervisor_running},
        {"secrets_returned", false},
        {"error", status.error},
    };
}

}  // namespace

McpServer::McpServer()
    : owned_manager_(std::make_unique<nxrth::bot::BotManager>(nullptr)),
      owned_proxy_pool_(std::make_unique<nxrth::proxy::ProxyPool>(
          nxrth::proxy::ProxyPool::load_default())),
      manager_(*owned_manager_),
      proxy_pool_(*owned_proxy_pool_),
      lua_engine_(manager_, proxy_pool_) {}

McpServer::McpServer(nxrth::bot::BotManager& manager,
                     nxrth::proxy::ProxyPool& proxy_pool, bool desktop_mode)
    : manager_(manager), proxy_pool_(proxy_pool), lua_engine_(manager_, proxy_pool_),
      desktop_mode_(desktop_mode) {}

json McpServer::tool_definitions() {
    const json id = bot_id_property();
    const json coord = {{"type", "integer"}, {"minimum", 0},
                        {"description", "Absolute tile coordinate."}};
    const json offset = {{"type", "integer"}, {"minimum", -4}, {"maximum", 4},
                         {"description", "Tile offset from the bot; Growtopia reach is +/-4."}};
    json tools = json::array();
    tools.push_back(tool("lua_execute",
        "Run one sandboxed Lua 5.4 script synchronously against this MCP server's current fleet. Desktop mode uses the open app's live bots, proxy pool, and shared automations; headless mode uses that MCP process's fleet. Output and errors are secret-redacted. The fixed limits are 20 million VM instructions, 30 seconds, 64 MiB VM memory, and 512 KiB output. Read nxrth://lua/api for the available API.",
        object_schema({{"source", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1048576},
                                    {"description", "Complete Lua source. It is consumed locally and is never returned or logged."}}}},
                      {"source"})));
    tools.push_back(tool("fleet_save",
        "Save the complete current fleet and shared automation config under a safe local name. protected is the default and preserves exact credentials with Windows CurrentUser DPAPI encryption. legacy_txt is an explicit plaintext compatibility export. The response contains metadata only and never returns credentials.",
        object_schema({{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 80},
                                  {"pattern", "^[A-Za-z0-9_-]+$"}}},
                       {"format", {{"type", "string"},
                                   {"enum", {"protected", "legacy_txt"}},
                                   {"default", "protected"}}}},
                      {"name"})));
    tools.push_back(tool("fleet_load",
        "Load a named local fleet backup without disclosing its stored credentials. format=auto prefers the protected backup and falls back to legacy TXT. Bot ids may change; use the returned id_remap. Loading starts the restored bots.",
        object_schema({{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 80},
                                  {"pattern", "^[A-Za-z0-9_-]+$"}}},
                       {"format", {{"type", "string"},
                                   {"enum", {"auto", "protected", "legacy_txt"}},
                                   {"default", "auto"}}},
                       {"replace_existing", {{"type", "boolean"}, {"default", false}}},
                       {"restore_automation", {{"type", "boolean"}, {"default", false}}}},
                      {"name"})));
    tools.push_back(tool("fleet_backups_list",
        "List safe-name local fleet backups. Returns only format, size, and modification metadata; backup contents and filesystem paths are never returned.",
        object_schema({})));
    tools.push_back(tool("script_list",
        "List saved Lua scripts in Nxrth's confined scripts workspace. Returns names, sizes, and secret-safe fingerprints only.",
        object_schema({})));
    tools.push_back(tool("script_read",
        "Read a saved Lua script for inspection. Credential-like values are redacted before the source leaves the local script store.",
        object_schema({{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 68},
                                  {"description", "Safe script name, with optional .lua extension."}}}},
                      {"name"})));
    tools.push_back(tool("script_write",
        "Create or replace a saved Lua script in Nxrth's confined scripts workspace. The exact source is stored locally but is never echoed in the response.",
        object_schema({{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 68}}},
                       {"source", {{"type", "string"}, {"maxLength", 1048576},
                                   {"description", "Exact Lua source, consumed locally and never returned."}}}},
                      {"name", "source"})));
    tools.push_back(tool("script_execute",
        "Execute one saved Lua script against the current fleet. Exact source is read locally and never returned; output and errors are secret-redacted.",
        object_schema({{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 68}}}},
                      {"name"})));
    tools.push_back(tool("recovery_configure",
        "Desktop app only. Checkpoint the fleet when required and configure the external crash supervisor to restore a protected snapshot, rerun a saved script, or do both. Accepts safe backup/script names only, never arbitrary paths.",
        object_schema({{"mode", {{"type", "string"},
                                 {"enum", {"snapshot_only", "script_only", "snapshot_then_script"}},
                                 {"default", "snapshot_only"}}},
                       {"fleet_name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 80},
                                       {"pattern", "^[A-Za-z0-9_-]+$"},
                                       {"default", "last_session"}}},
                       {"script_name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 68}}},
                       {"restart_delay_ms", {{"type", "integer"}, {"minimum", 500},
                                             {"maximum", 60000}, {"default", 2000}}},
                       {"max_restarts", {{"type", "integer"}, {"minimum", 1},
                                         {"maximum", 20}, {"default", 3}}},
                       {"window_seconds", {{"type", "integer"}, {"minimum", 30},
                                           {"maximum", 86400}, {"default", 600}}}})));
    tools.push_back(tool("recovery_status",
        "Desktop app only. Read the external crash recovery configuration and supervisor health. No backup or script contents are returned.",
        object_schema({})));
    tools.push_back(tool("recovery_disable",
        "Desktop app only. Disable automatic crash restart and restore while retaining the named local backups and scripts.",
        object_schema({})));
    tools.push_back(tool("session_login",
        "Start a GrowID bot, validate a Google OAuth ltoken through checktoken, or connect a provider gateway token. Provider key:value records use one pinned rotating exit for server_data and ENet. Secrets are consumed locally and never returned.",
        object_schema({{"username", {{"type", "string"}, {"minLength", 1}, {"maxLength", 512},
                                      {"description", "GrowID username; required only for method=growid."}}},
                       {"password", {{"type", "string"}, {"maxLength", 512},
                                     {"description", "Required only for method=growid."}}},
                       {"ltoken", {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096},
                                  {"description", "Google OAuth refreshToken|rid|mac|wk (or keyed refreshToken:) or a provider gateway-token key:value record containing token:/rid:/mac:/wk:; required only for method=ltoken."}}},
                       {"method", {{"type", "string"},
                                   {"enum", {"growid", "ltoken"}}, {"default", "growid"}}},
                       {"use_configured_proxy", {{"type", "boolean"}, {"default", true}}}})));
    tools.push_back(tool("session_list", "List every managed bot with connection, world and position summary.",
                         object_schema({})));
    tools.push_back(tool("session_status", "Read one bot's current connection, ping, nickname, world and position.",
                         object_schema({{"bot_id", id}}, {"bot_id"})));
    tools.push_back(tool("session_reconnect", "Queue a reconnect using the bot's current session.",
                         object_schema({{"bot_id", id}}, {"bot_id"})));
    tools.push_back(tool("session_disconnect", "Disconnect a bot without removing it from the manager.",
                         object_schema({{"bot_id", id}}, {"bot_id"})));
    tools.push_back(tool("session_stop", "Stop and remove a bot from the active shared fleet.",
                         object_schema({{"bot_id", id}}, {"bot_id"})));
    tools.push_back(tool("session_set_logging",
        "Enable or disable recording this bot's per-bot system log (what session_logs returns). Off by default; enable it before relying on session_logs, or use fleet_logs for shared recent activity.",
        object_schema({{"bot_id", id}, {"enabled", {{"type", "boolean"}}}}, {"bot_id", "enabled"})));
    tools.push_back(tool("accounts_spawn",
        "Batch-login accounts from accounts_stats.json (or pasted user:pass / JSON), mirroring the desktop Database tab's First/Random/Next/All buttons. mode=ltoken consumes each record's composite login_token/ltoken field. Spawns into the shared fleet with pool proxies, and SKIPS (never leaks the real IP) when the pool is exhausted. Provide 'path' or 'text'.",
        object_schema({{"path", {{"type", "string"}, {"maxLength", 1024},
                                 {"description", "Path to accounts_stats.json or a user:pass file. Provide path OR text."}}},
                       {"text", {{"type", "string"}, {"maxLength", 4000000},
                                 {"description", "Pasted accounts_stats.json or user:pass lines. Provide path OR text."}}},
                       {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000},
                                  {"description", "How many accounts to spawn. strategy=all is also capped at 5000 records."}}},
                       {"strategy", {{"type", "string"}, {"enum", {"first", "random", "next", "all"}},
                                     {"default", "first"},
                                     {"description", "first=[0,X); random=X distinct random; next=advancing cursor batches; all=every parsed account."}}},
                       {"mode", {{"type", "string"},
                                  {"enum", {"standard", "ltoken"}},
                                  {"default", "standard"}}},
                       {"use_pool_proxy", {{"type", "boolean"}, {"default", true}}}})));
    tools.push_back(tool("proxy_status",
        "Read the game proxy pool: per-endpoint occupancy (active/capacity/full), pool totals, bypass-login config, and the 403/rate-limit quarantine list. Correlate with session_list/session_status proxy_key to pin failures to shared IPs.",
        object_schema({})));
    tools.push_back(tool("automation_get_config",
        "Read the fleet-wide automation module flags, current params, and supported automation params.",
        object_schema({})));
    tools.push_back(tool("automation_status",
        "Read automation health, validation, active bot scopes, per-bot AutoGeiger phase and floating item counts.",
        object_schema({})));
    tools.push_back(tool("automation_validate_config",
        "Validate the current automation config and return a normalized preview without changing anything.",
        object_schema({})));
    tools.push_back(tool("automation_define_group",
        "Define or replace a named automation bot group for scoped module execution.",
        object_schema({{"group", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                       {"bot_ids", {{"type", "array"},
                                    {"items", {{"type", "integer"}, {"minimum", 0}}},
                                    {"uniqueItems", true}}}},
                      {"group", "bot_ids"})));
    tools.push_back(tool("automation_set_module",
        "Enable/disable any fleet automation module and optionally merge raw string/number/boolean params.",
        object_schema({{"module", {{"type", "string"},
                                   {"enum", {"geiger", "collect", "coordinate"}}}},
                       {"enabled", {{"type", "boolean"}}},
                       {"bot_ids", {{"type", "array"},
                                    {"items", {{"type", "integer"}, {"minimum", 0}}},
                                    {"uniqueItems", true},
                                    {"description", "Optional scope: only these bot ids run this module. Empty array clears bot-id scope."}}},
                       {"group", {{"type", "string"}, {"maxLength", 64},
                                  {"description", "Optional scope: only bot ids in this named group run this module. Empty string clears group scope."}}},
                       {"params", {{"type", "object"},
                                   {"additionalProperties",
                                    {{"type", json::array({"string", "number", "integer", "boolean"})}}}}}},
                      {"module"})));
    tools.push_back(tool("automation_configure_geiger",
        "Enable AutoGeiger with hunt, depot and pickup worlds, plus optional advanced geiger params.",
        object_schema({{"enabled", {{"type", "boolean"}, {"default", true}}},
                       {"hunt_worlds", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048},
                                        {"description", "Hunt/search worlds, comma/newline separated. Door id can be WORLD|door."}}},
                       {"depot_worlds", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048},
                                         {"description", "Depot worlds for depositing loot."}}},
                       {"pickup_worlds", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048},
                                          {"description", "Worlds where Geiger Counters can be picked up."}}},
                       {"geiger_item", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535},
                                        {"default", 2204}}},
                       {"wear", {{"type", "boolean"}, {"default", true}}},
                       {"dig", {{"type", "boolean"}, {"default", true}}},
                       {"recharge_min", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1440},
                                          {"default", 30}}},
                       {"min_y", {{"type", "integer"}, {"minimum", 0}, {"maximum", 200},
                                  {"default", 0}}},
                       {"max_y", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200},
                                  {"default", 53}}},
                       {"world_width", {{"type", "integer"}, {"minimum", 4}, {"maximum", 300},
                                        {"default", 100}}},
                       {"signal_wait_ms", {{"type", "integer"}, {"minimum", 500}, {"maximum", 60000},
                                           {"default", 4200}}},
                       {"max_steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000},
                                      {"default", 70}}},
                       {"webhook_url", {{"type", "string"}, {"maxLength", 512},
                                        {"description", "Optional Discord webhook URL for geiger fleet logs."}}}},
                      {"hunt_worlds", "depot_worlds", "pickup_worlds"})));
    tools.push_back(tool("automation_start_geiger_farm",
        "Preset shortcut for starting AutoGeiger with hunt, depot and pickup worlds.",
        object_schema({{"hunt_worlds", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048}}},
                       {"depot_worlds", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048}}},
                       {"pickup_worlds", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048}}},
                       {"bot_ids", {{"type", "array"},
                                    {"items", {{"type", "integer"}, {"minimum", 0}}},
                                    {"uniqueItems", true}}},
                       {"group", {{"type", "string"}, {"maxLength", 64}}},
                       {"dig", {{"type", "boolean"}, {"default", true}}},
                       {"wear", {{"type", "boolean"}, {"default", true}}},
                       {"recharge_min", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1440},
                                          {"default", 30}}}},
                      {"hunt_worlds", "depot_worlds", "pickup_worlds"})));
    tools.push_back(tool("automation_stop_geiger_farm",
        "Preset shortcut for disabling AutoGeiger without clearing its saved params.",
        object_schema({})));
    tools.push_back(tool("automation_pause_all",
        "Disable every automation module without clearing params, groups, scopes, or profiles.",
        object_schema({})));
    tools.push_back(tool("automation_save_profile",
        "Save the current automation config to data/automation_profiles.json under a profile name.",
        object_schema({{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64},
                                {"pattern", "^[A-Za-z0-9_-]+$"}}}},
                      {"name"})));
    tools.push_back(tool("automation_load_profile",
        "Load a saved automation profile and apply it as the live fleet automation config.",
        object_schema({{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64},
                                {"pattern", "^[A-Za-z0-9_-]+$"}}}},
                      {"name"})));
    tools.push_back(tool("automation_list_profiles",
        "List saved automation profile names and their validation status.",
        object_schema({})));
    tools.push_back(tool("world_join", "Join or warp to a world, optionally through a door id.",
        object_schema({{"bot_id", id}, {"world", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                       {"door_id", {{"type", "string"}, {"maxLength", 64}, {"default", ""}}}},
                      {"bot_id", "world"})));
    tools.push_back(tool("world_leave", "Leave the current world and return to world select.",
                         object_schema({{"bot_id", id}}, {"bot_id"})));
    tools.push_back(tool("move_to", "Pathfind and walk to an absolute tile coordinate. Execution is asynchronous.",
                         object_schema({{"bot_id", id}, {"x", coord}, {"y", coord}},
                                       {"bot_id", "x", "y"})));
    tools.push_back(tool("move_step", "Move directly to an absolute adjacent/near tile without A* planning.",
                         object_schema({{"bot_id", id}, {"x", coord}, {"y", coord}},
                                       {"bot_id", "x", "y"})));
    tools.push_back(tool("sense_environment",
        "Inspect nearby tiles, collision obstacles, dropped objects and players around the bot.",
        object_schema({{"bot_id", id},
                       {"radius", {{"type", "integer"}, {"minimum", 1}, {"maximum", 20}, {"default", 6}}},
                       {"blocked_only", {{"type", "boolean"}, {"default", false}}}}, {"bot_id"})));
    tools.push_back(tool("world_floating_items",
        "List visible dropped/floating ground items in the bot's current world, with item names and tile positions.",
        object_schema({{"bot_id", id},
                       {"radius_tiles", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 200}}},
                       {"item_id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
                       {"name_contains", {{"type", "string"}, {"maxLength", 64}}}},
                      {"bot_id"})));
    tools.push_back(tool("chat_send", "Send a message through in-game chat.",
        object_schema({{"bot_id", id}, {"message", {{"type", "string"}, {"minLength", 1},
                                                        {"maxLength", 120}}}}, {"bot_id", "message"})));
    tools.push_back(tool("chat_read", "Read buffered in-game chat/console messages.",
        object_schema({{"bot_id", id}, {"after", {{"type", "integer"}, {"minimum", 0}, {"default", 0}}},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 300}, {"default", 100}}}},
                      {"bot_id"})));
    tools.push_back(tool("session_logs",
        "Read redacted per-bot system logs for debugging login, reconnect and automation state.",
        object_schema({{"bot_id", id},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 300}, {"default", 80}}}},
                      {"bot_id"})));
    tools.push_back(tool("fleet_logs",
        "Read the desktop application's shared redacted log stream, optionally filtered to one bot.",
        object_schema({{"bot_id", id},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}, {"default", 200}}}})));
    tools.push_back(tool("inventory_list", "List inventory stacks with names, amounts and equipped state.",
                         object_schema({{"bot_id", id}}, {"bot_id"})));
    tools.push_back(tool("inventory_drop", "Drop an inventory item stack through the normal game dialog flow.",
        object_schema({{"bot_id", id}, {"item_id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
                       {"amount", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}}},
                      {"bot_id", "item_id", "amount"})));
    tools.push_back(tool("inventory_trash", "Trash an inventory item stack through the normal game dialog flow.",
        object_schema({{"bot_id", id}, {"item_id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
                       {"amount", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}}},
                      {"bot_id", "item_id", "amount"})));
    tools.push_back(tool("inventory_use", "Activate, equip, or unequip an inventory item by id.",
        object_schema({{"bot_id", id}, {"item_id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
                       {"mode", {{"type", "string"}, {"enum", {"activate", "equip", "unequip"}},
                                 {"default", "activate"}}}},
                      {"bot_id", "item_id"})));
    tools.push_back(tool("inventory_collect",
        "Collect one visible dropped object by uid, or all eligible nearby objects when uid is omitted.",
        object_schema({{"bot_id", id}, {"uid", {{"type", "integer"}, {"minimum", 1}}},
                       {"range_tiles", {{"type", "number"}, {"minimum", 1}, {"maximum", 5}, {"default", 3}}}},
                      {"bot_id"})));
    tools.push_back(tool("world_action",
        "Perform another supported player action: punch/mine, build, wrench, activate tile, enter door, face, respawn, or accept access.",
        object_schema({{"bot_id", id},
                       {"action", {{"type", "string"}, {"enum", {"punch", "place", "wrench", "wrench_player",
                           "activate_tile", "enter", "face", "respawn", "accept_access"}}}},
                       {"offset_x", offset}, {"offset_y", offset},
                       {"x", {{"type", "integer"}, {"minimum", 0}}},
                       {"y", {{"type", "integer"}, {"minimum", 0}}},
                       {"item_id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
                       {"net_id", {{"type", "integer"}, {"minimum", 1}}},
                       {"password", {{"type", "string"}, {"maxLength", 128}}},
                       {"left", {{"type", "boolean"}}}}, {"bot_id", "action"})));
    return tools;
}

std::optional<json> McpServer::handle(const json& request) {
    if (!request.is_object()) return rpc_error(nullptr, -32600, "Invalid Request");
    const json id = request.contains("id") ? request["id"] : json(nullptr);
    const bool notification = !request.contains("id");
    try {
        if (request.value("jsonrpc", "") != "2.0" || !request.contains("method") ||
            !request["method"].is_string())
            return rpc_error(id, -32600, "Invalid Request");
        const std::string method = request["method"].get<std::string>();
        const json params = request.value("params", json::object());
        json result;
        if (method == "initialize") {
            std::string version = "2025-06-18";
            if (params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
                const std::string requested = params["protocolVersion"].get<std::string>();
                if (requested == "2024-11-05" || requested == "2025-03-26" ||
                    requested == "2025-06-18")
                    version = requested;
            }
            result = {{"protocolVersion", version},
                      {"capabilities", {{"tools", {{"listChanged", false}}},
                                        {"resources", {{"subscribe", false}, {"listChanged", false}}}}},
                      {"serverInfo", {{"name", "nxrth-mcp"}, {"version", "0.1.0"}}},
                      {"instructions", "Control Growtopia bots and fleet automations. Mutating bot tools enqueue work; automation tools update the shared fleet config live. lua_execute runs sandboxed Lua against that same fleet; read nxrth://lua/api before using unfamiliar Lua APIs. Poll session_status or resources to observe completion."}};
        } else if (method == "ping") {
            result = json::object();
        } else if (method == "tools/list") {
            result = {{"tools", tool_definitions()}};
        } else if (method == "tools/call") {
            if (!params.contains("name") || !params["name"].is_string())
                throw std::invalid_argument("tools/call requires a string name");
            const json args = params.value("arguments", json::object());
            if (!args.is_object()) throw std::invalid_argument("arguments must be an object");
            result = call_tool(params["name"].get<std::string>(), args);
        } else if (method == "resources/list") {
            result = {{"resources", json::array({{{"uri", "nxrth://fleet"},
                                                   {"name", "Nxrth bot fleet"},
                                                   {"mimeType", "application/json"}},
                                                  {{"uri", "nxrth://automation"},
                                                   {"name", "Fleet automation config"},
                                                   {"mimeType", "application/json"}},
                                                  {{"uri", "nxrth://automation/status"},
                                                   {"name", "Fleet automation status"},
                                                   {"mimeType", "application/json"}},
                                                  {{"uri", "nxrth://logs"},
                                                   {"name", "Shared application logs"},
                                                   {"mimeType", "application/json"}},
                                                  {{"uri", "nxrth://lua/api"},
                                                   {"name", "Nxrth Lua 5.4 API"},
                                                   {"mimeType", "application/json"}}})}};
        } else if (method == "resources/templates/list") {
            result = {{"resourceTemplates", json::array({
                {{"uriTemplate", "nxrth://bot/{bot_id}/state"}, {"name", "Bot state"}, {"mimeType", "application/json"}},
                {{"uriTemplate", "nxrth://bot/{bot_id}/world"}, {"name", "World snapshot"}, {"mimeType", "application/json"}},
                {{"uriTemplate", "nxrth://bot/{bot_id}/objects"}, {"name", "Floating ground items"}, {"mimeType", "application/json"}},
                {{"uriTemplate", "nxrth://bot/{bot_id}/inventory"}, {"name", "Inventory"}, {"mimeType", "application/json"}},
                {{"uriTemplate", "nxrth://bot/{bot_id}/chat"}, {"name", "Chat buffer"}, {"mimeType", "application/json"}}
            })}};
        } else if (method == "resources/read") {
            if (!params.contains("uri") || !params["uri"].is_string())
                throw std::invalid_argument("resources/read requires uri");
            result = read_resource(params["uri"].get<std::string>());
        } else if (method == "notifications/initialized" || method.starts_with("notifications/")) {
            if (notification) return std::nullopt;
            result = json::object();
        } else {
            return rpc_error(id, -32601, "Method not found");
        }
        if (notification) return std::nullopt;
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
    } catch (const std::invalid_argument& e) {
        return rpc_error(id, -32602, e.what());
    } catch (const std::exception& e) {
        return rpc_error(id, -32603, e.what());
    }
}

bool McpServer::enqueue(std::uint32_t bot_id, BotCommand command) const {
    return manager_.send_cmd(bot_id, std::move(command));
}

json McpServer::bot_state_json(std::uint32_t bot_id, bool detailed) const {
    auto state = manager_.get_state(bot_id);
    if (!state) throw std::invalid_argument("unknown bot_id");
    json out = state_summary(bot_id, *state);
    if (detailed) {
        out["world_size"] = {{"width", state->world_width}, {"height", state->world_height}};
        out["inventory_size"] = state->inventory_size;
        out["inventory_stacks"] = state->inventory.size();
        out["nearby_player_count"] = state->players.size();
        out["ground_object_count"] = state->objects.size();
        out["auto_collect"] = state->auto_collect;
        out["auto_reconnect"] = state->auto_reconnect;
        out["collect_radius_tiles"] = state->collect_radius_tiles;
    }
    if (auto it = manager_.bots.find(bot_id); it != manager_.bots.end())
        out["proxy_key"] = it->second.proxy_key ? json(*it->second.proxy_key) : json(nullptr);
    return out;
}

json McpServer::inventory_json(std::uint32_t bot_id) const {
    auto state = manager_.get_state(bot_id);
    if (!state) throw std::invalid_argument("unknown bot_id");
    json stacks = json::array();
    for (const auto& slot : state->inventory) stacks.push_back(item_json(slot, manager_.items_dat()));
    return {{"bot_id", bot_id}, {"capacity", state->inventory_size},
            {"stack_count", state->inventory.size()}, {"gems", state->gems},
            {"items", std::move(stacks)}};
}

json McpServer::chat_json(std::uint32_t bot_id, std::size_t after, std::size_t limit) const {
    auto state = manager_.get_state(bot_id);
    if (!state) throw std::invalid_argument("unknown bot_id");
    const std::size_t base = static_cast<std::size_t>(state->chat_base_index);
    const std::size_t requested = after <= base ? 0 : after - base;
    const std::size_t start = std::min(requested, state->chat.size());
    const std::size_t end = std::min(state->chat.size(), start + limit);
    json messages = json::array();
    for (std::size_t i = start; i < end; ++i)
        messages.push_back({{"index", base + i}, {"text", state->chat[i]}});
    return {{"bot_id", bot_id}, {"messages", std::move(messages)},
            {"next_index", base + end}, {"oldest_index", base},
            {"buffer_size", state->chat.size()}};
}

json McpServer::logs_json(std::uint32_t bot_id, std::size_t limit) const {
    auto state = manager_.get_state(bot_id);
    if (!state) throw std::invalid_argument("unknown bot_id");
    const std::size_t n = std::min(limit, state->console.size());
    const std::size_t start = state->console.size() - n;
    json lines = json::array();
    for (std::size_t i = start; i < state->console.size(); ++i)
        lines.push_back({{"index", i}, {"text", redact_log_line(state->console[i])}});
    return {{"bot_id", bot_id}, {"logs", std::move(lines)}, {"returned", n},
            {"buffer_size", state->console.size()}};
}

json McpServer::world_json(std::uint32_t bot_id) const {
    auto state = manager_.get_state(bot_id);
    if (!state) throw std::invalid_argument("unknown bot_id");
    json players = json::array();
    for (const auto& p : state->players)
        players.push_back({{"net_id", p.net_id}, {"name", p.name}, {"country", p.country},
                           {"position", {{"x", p.pos_x}, {"y", p.pos_y}}}});
    json objects = json::array();
    for (const auto& o : state->objects) {
        json obj = {{"uid", o.uid}, {"item_id", o.item_id}, {"count", o.count},
                    {"position_pixels", {{"x", o.x}, {"y", o.y}}},
                    {"position_tiles", {{"x", o.x / 32.0f}, {"y", o.y / 32.0f}}}};
        if (const auto* info = manager_.items_dat()->find_by_id(o.item_id)) obj["name"] = info->name;
        objects.push_back(std::move(obj));
    }
    return {{"bot_id", bot_id}, {"name", state->world_name},
            {"width", state->world_width}, {"height", state->world_height},
            {"bot_position", {{"x", state->pos_x}, {"y", state->pos_y}}},
            {"players", std::move(players)}, {"objects", std::move(objects)}};
}

json McpServer::automation_status_json() {
    const auto cfg = manager_.fleet()->config_snapshot();
    json bots = json::array();
    json active_by_module = json::object();
    for (const std::string module : {"geiger", "collect", "coordinate"})
        active_by_module[module] = json::array();

    for (const auto& info : manager_.list()) {
        auto state = manager_.get_state(info.id);
        if (!state) continue;
        json active_modules = json::array();
        for (const std::string module : {"geiger", "collect", "coordinate"}) {
            if (cfg.is_on_for(module, info.id)) {
                active_modules.push_back(module);
                active_by_module[module].push_back(info.id);
            }
        }
        const auto geiger_item = static_cast<std::uint16_t>(
            std::clamp(int_param_or(cfg, "geiger_item", 2204), 1, 65535));
        bots.push_back({
            {"bot_id", info.id},
            {"username", state->username.empty() ? info.username : state->username},
            {"status", nxrth::bot::to_string(state->status)},
            {"world", state->world_name},
            {"position", {{"x", state->pos_x}, {"y", state->pos_y}}},
            {"active_modules", std::move(active_modules)},
            {"geiger",
             {{"phase", geiger_phase_for_bot(cfg, *state, info.id)},
              {"charged_counter_amount", inventory_amount(*state, geiger_item)},
              {"dead_counter_amount", inventory_amount(*state, 2286)},
              {"signal", geiger_signal_json(state->geiger_signal)}}},
            {"floating_item_count", state->objects.size()},
        });
    }

    json modules = json::object();
    for (const std::string module : {"geiger", "collect", "coordinate"}) {
        json scope = json::object();
        if (auto it = cfg.module_bot_ids.find(module); it != cfg.module_bot_ids.end())
            scope["bot_ids"] = it->second;
        if (auto it = cfg.module_groups.find(module); it != cfg.module_groups.end())
            scope["group"] = it->second;
        modules[module] = {{"enabled", cfg.is_on(module)}, {"scope", std::move(scope)},
                           {"active_bot_ids", active_by_module[module]}};
    }

    return {{"automation", automation_config_json(cfg)},
            {"validation", automation_validation_json(cfg)},
            {"modules", std::move(modules)},
            {"bots", std::move(bots)}};
}

json McpServer::floating_items_json(std::uint32_t bot_id, std::optional<float> radius_tiles,
                                    std::optional<std::uint32_t> item_id,
                                    const std::string& name_contains) const {
    auto state = manager_.get_state(bot_id);
    if (!state) throw std::invalid_argument("unknown bot_id");
    const std::string needle = lower_ascii(name_contains);
    std::vector<json> filtered;
    filtered.reserve(state->objects.size());
    for (const auto& o : state->objects) {
        if (item_id && o.item_id != *item_id) continue;
        json obj = floating_object_json(bot_id, *state, o, manager_.items_dat());
        if (!needle.empty()) {
            const std::string name = lower_ascii(obj.value("name", std::string{}));
            if (name.find(needle) == std::string::npos) continue;
        }
        if (radius_tiles && obj.value("distance_tiles", 0.0) > *radius_tiles) continue;
        filtered.push_back(std::move(obj));
    }
    std::sort(filtered.begin(), filtered.end(), [](const json& a, const json& b) {
        return a.value("distance_tiles", 0.0) < b.value("distance_tiles", 0.0);
    });
    json items = json::array();
    for (auto& obj : filtered) items.push_back(std::move(obj));
    return {{"bot_id", bot_id}, {"world", state->world_name}, {"total", state->objects.size()},
            {"returned", items.size()}, {"items", std::move(items)}};
}

json McpServer::call_tool(const std::string& name, const json& args) {
    try {
        if (name == "lua_execute") {
            const std::string source = required_string(args, "source", 1024 * 1024);
            auto result = lua_engine_.execute(source);
            result.output = nxrth::script::ScriptStore::redact_for_ai(result.output);
            result.error = nxrth::script::ScriptStore::redact_for_ai(result.error);
            json data = {
                {"ok", result.ok},
                {"output", std::move(result.output)},
                {"error", std::move(result.error)},
                {"added_bot_ids", std::move(result.added_bot_ids)},
                {"limits",
                 {{"instruction_limit", 20'000'000},
                  {"time_limit_ms", 30'000},
                  {"output_limit_bytes", 512 * 1024}}},
            };
            return structured_tool_result(std::move(data), !result.ok);
        }
        if (name == "fleet_save") {
            const std::string backup_name = required_string(args, "name", 80);
            const auto format = required_fleet_format(args, "protected");
            const auto result = format == nxrth::recovery::FleetBackupFormat::Protected
                                    ? nxrth::recovery::FleetStore::save(backup_name, manager_)
                                    : nxrth::recovery::FleetStore::save_legacy_text(backup_name,
                                                                                    manager_);
            return structured_tool_result(fleet_save_result_json(result, format), !result.ok);
        }
        if (name == "fleet_load") {
            const std::string backup_name = required_string(args, "name", 80);
            nxrth::recovery::FleetLoadOptions options;
            options.replace_existing = args.value("replace_existing", false);
            options.restore_automation = args.value("restore_automation", false);
            const std::string format = args.value("format", std::string("auto"));
            nxrth::recovery::FleetLoadResult result;
            if (format == "auto") {
                const auto protected_path = nxrth::recovery::FleetStore::backup_path(
                    backup_name, nxrth::recovery::FleetBackupFormat::Protected);
                std::error_code ec;
                if (protected_path && std::filesystem::exists(*protected_path, ec)) {
                    result = nxrth::recovery::FleetStore::load(
                        backup_name, manager_, proxy_pool_, options);
                } else {
                    result = nxrth::recovery::FleetStore::load_legacy_text(
                        backup_name, manager_, proxy_pool_, options);
                }
            } else if (format == "legacy_txt") {
                result = nxrth::recovery::FleetStore::load_legacy_text(
                    backup_name, manager_, proxy_pool_, options);
            } else if (format == "protected") {
                const auto path = nxrth::recovery::FleetStore::backup_path(
                    backup_name, nxrth::recovery::FleetBackupFormat::Protected);
                if (!path) throw std::invalid_argument("invalid backup name");
                result = nxrth::recovery::FleetStore::load_from_path(
                    *path, nxrth::recovery::FleetBackupFormat::Protected, manager_, proxy_pool_,
                    options);
                result.name = backup_name;
            } else {
                throw std::invalid_argument("format must be auto, protected, or legacy_txt");
            }
            return structured_tool_result(fleet_load_result_json(result), !result.ok);
        }
        if (name == "fleet_backups_list") {
            json backups = json::array();
            for (const auto& backup : nxrth::recovery::FleetStore::list()) {
                backups.push_back({{"name", backup.name},
                                   {"format", fleet_format_name(backup.format)},
                                   {"size_bytes", backup.file_size},
                                   {"modified_unix_ms", backup.modified_unix_ms}});
            }
            return success({{"backups", std::move(backups)}, {"secrets_returned", false}});
        }
        if (name == "script_list") {
            nxrth::script::ScriptStore store;
            std::string error;
            const auto stored = store.list(&error);
            if (!error.empty()) return failure(error);
            json scripts = json::array();
            for (const auto& metadata : stored)
                scripts.push_back(script_metadata_json(metadata));
            return success({{"scripts", std::move(scripts)}, {"source_returned", false},
                            {"secrets_returned", false}});
        }
        if (name == "script_read") {
            const std::string script_name = required_string(args, "name", 68);
            nxrth::script::ScriptStore store;
            std::string error;
            auto source = store.read_redacted(script_name, &error);
            if (!source) return failure(error.empty() ? "script is unavailable" : error);
            auto metadata = find_script_metadata(store, script_name, &error);
            if (!metadata) return failure(error);
            return success({{"script", script_metadata_json(*metadata)},
                            {"source", std::move(*source)},
                            {"source_redacted", true}, {"secrets_returned", false}});
        }
        if (name == "script_write") {
            const std::string script_name = required_string(args, "name", 68);
            if (!args.contains("source") || !args["source"].is_string())
                throw std::invalid_argument("'source' must be a string");
            const std::string source = args["source"].get<std::string>();
            if (source.size() > nxrth::script::ScriptStore::kMaxScriptBytes)
                throw std::invalid_argument("'source' is too large");
            nxrth::script::ScriptStore store;
            std::string error;
            if (!store.write(script_name, source, &error)) return failure(error);
            auto metadata = find_script_metadata(store, script_name, &error);
            if (!metadata) return failure(error);
            return success({{"saved", true}, {"script", script_metadata_json(*metadata)},
                            {"source_returned", false}, {"secrets_returned", false}});
        }
        if (name == "script_execute") {
            const std::string script_name = required_string(args, "name", 68);
            nxrth::script::ScriptStore store;
            std::string error;
            auto source = store.read_exact(script_name, &error);
            if (!source) return failure(error.empty() ? "script is unavailable" : error);
            auto metadata = find_script_metadata(store, script_name, &error);
            if (!metadata) return failure(error);
            auto result = lua_engine_.execute(*source);
            result.output = nxrth::script::ScriptStore::redact_for_ai(result.output);
            result.error = nxrth::script::ScriptStore::redact_for_ai(result.error);
            json data = {
                {"ok", result.ok},
                {"script", script_metadata_json(*metadata)},
                {"output", std::move(result.output)},
                {"error", std::move(result.error)},
                {"added_bot_ids", std::move(result.added_bot_ids)},
                {"source_returned", false},
                {"secrets_returned", false},
                {"limits",
                 {{"instruction_limit", 20'000'000},
                  {"time_limit_ms", 30'000},
                  {"output_limit_bytes", 512 * 1024}}},
            };
            return structured_tool_result(std::move(data), !result.ok);
        }
        if (name == "recovery_configure") {
            if (!desktop_mode_)
                return failure("recovery tools require the running desktop Nxrth app");
            const std::string mode_name = args.value("mode", std::string("snapshot_only"));
            const auto mode = nxrth::recovery::restore_mode_from_string(mode_name);
            if (!mode)
                throw std::invalid_argument(
                    "mode must be snapshot_only, script_only, or snapshot_then_script");
            nxrth::recovery::RecoveryOptions options;
            options.enabled = true;
            options.mode = *mode;
            options.fleet_name = args.value("fleet_name", std::string("last_session"));
            if (args.contains("script_name"))
                options.script_name = required_string(args, "script_name", 68);
            if (args.contains("restart_delay_ms"))
                options.restart_delay_ms = required_u32(args, "restart_delay_ms", 60'000);
            if (args.contains("max_restarts"))
                options.max_restarts = required_u32(args, "max_restarts", 20);
            if (args.contains("window_seconds"))
                options.window_seconds = required_u32(args, "window_seconds", 86'400);
            auto result = nxrth::recovery::RecoveryController::configure(options, manager_);
            json data = {{"configured", result.ok},
                         {"status", recovery_status_json(result.status)},
                         {"secrets_returned", false},
                         {"error", result.error}};
            if (result.checkpoint) {
                data["checkpoint"] = fleet_save_result_json(
                    *result.checkpoint, nxrth::recovery::FleetBackupFormat::Protected);
            } else {
                data["checkpoint"] = nullptr;
            }
            return structured_tool_result(std::move(data), !result.ok);
        }
        if (name == "recovery_status") {
            if (!desktop_mode_)
                return failure("recovery tools require the running desktop Nxrth app");
            return success(recovery_status_json(nxrth::recovery::RecoveryController::status()));
        }
        if (name == "recovery_disable") {
            if (!desktop_mode_)
                return failure("recovery tools require the running desktop Nxrth app");
            auto result = nxrth::recovery::RecoveryController::disable();
            json data = {{"disabled", result.ok},
                         {"status", recovery_status_json(result.status)},
                         {"secrets_returned", false},
                         {"error", result.error}};
            return structured_tool_result(std::move(data), !result.ok);
        }
        if (name == "session_login") {
            const std::string method = args.value("method", "growid");
            if (method != "growid" && method != "ltoken")
                throw std::invalid_argument("method must be growid or ltoken");

            std::string username;
            std::string password;
            std::string ltoken;
            std::optional<nxrth::bot::LtokenRecord> ltoken_record;
            if (method == "ltoken") {
                ltoken = required_string(args, "ltoken", 4096);
                ltoken_record = nxrth::bot::parse_ltoken_string(ltoken);
                if (!ltoken_record)
                    throw std::invalid_argument(
                        "ltoken must be refreshToken|rid|mac|wk or a keyed mac:/wk:/rid:/token: record");
            } else {
                username = required_string(args, "username", 512);
                password = required_string(args, "password", 512);
            }
            std::optional<nxrth::net::Socks5Config> proxy;
            std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy;
            const bool is_provider =
                ltoken_record &&
                ltoken_record->kind == nxrth::bot::LtokenRecord::Kind::ProviderToken;
            const bool use_configured_proxy = args.value("use_configured_proxy", true);
            if (use_configured_proxy) {
                proxy = proxy_pool_.choose(manager_.proxy_key_counts());
                if (!proxy)
                    throw std::runtime_error(
                        "game proxy pool is disabled or unavailable; direct fallback refused");
                // Provider validate-ltokens are NOT IP-bound (they log in from any
                // exit), so when the rotating-login pool is burned they can run on the
                // assigned game proxy instead. NXRTH_PROVIDER_NO_ROTATING=1 forces that.
                const bool provider_skip_rotating =
                    is_provider && std::getenv("NXRTH_PROVIDER_NO_ROTATING") != nullptr;
                if (method == "growid" || (is_provider && !provider_skip_rotating))
                    login_proxy = proxy_pool_.rotating_login_proxy();
            }
            std::uint32_t id = 0;
            const auto proxy_policy = use_configured_proxy
                                          ? nxrth::bot::ProxyPolicy::Pool
                                          : nxrth::bot::ProxyPolicy::Direct;
            if (method == "ltoken") {
                id = manager_.spawn_ltoken(ltoken, std::move(proxy), std::move(login_proxy),
                                            proxy_policy);
            } else {
                id = manager_.spawn(username, password, std::move(proxy),
                                    std::move(login_proxy), proxy_policy);
            }
            json platform_id = nullptr;
            if (method == "ltoken") {
                // Provider validate-ltokens send the device-bearing packet with the
                // record's "<platform>,1,1" triple; Google refresh tokens send bare "2".
                platform_id =
                    (ltoken_record &&
                     ltoken_record->kind == nxrth::bot::LtokenRecord::Kind::ProviderToken)
                        ? (ltoken_record->platform_id.value_or("1") + ",1,1")
                        : std::string("2");
            }
            return success({{"accepted", true}, {"bot_id", id}, {"status", "connecting"},
                            {"method", method},
                            {"token_kind",
                             !ltoken_record
                                 ? json(nullptr)
                                 : json(ltoken_record->kind == nxrth::bot::LtokenRecord::Kind::ProviderToken
                                            ? "provider_token"
                                            : "google_refresh_token")},
                            {"platform_id", std::move(platform_id)}});
        }
        if (name == "session_list") {
            json bots = json::array();
            for (const auto& info : manager_.list()) {
                bots.push_back({{"bot_id", info.id}, {"nickname", info.username},
                    {"status", info.status}, {"connected", info.status == "connected" || info.status == "in_game"},
                    {"world", info.world}, {"position", {{"x", info.pos_x}, {"y", info.pos_y}}},
                    {"ping_ms", info.ping_ms}, {"gems", info.gems},
                    {"proxy_key", info.proxy_key ? json(*info.proxy_key) : json(nullptr)}});
            }
            return success({{"bots", std::move(bots)}});
        }
        if (name == "accounts_spawn") {
            std::string content;
            if (args.contains("path") && args["path"].is_string() &&
                !args["path"].get<std::string>().empty()) {
                const std::string path = args["path"].get<std::string>();
                std::error_code file_error;
                if (!std::filesystem::is_regular_file(path, file_error) || file_error)
                    return failure("accounts source must be a readable regular file");
                const auto file_size = std::filesystem::file_size(path, file_error);
                if (file_error || file_size > kMaxAccountsInputBytes)
                    return failure("accounts source exceeds the 4 MB limit");
                std::ifstream in(path, std::ios::binary);
                if (!in) return failure("cannot open accounts file");
                std::array<char, 8192> buffer{};
                while (in) {
                    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const auto read = in.gcount();
                    if (read <= 0) break;
                    if (content.size() + static_cast<std::size_t>(read) >
                        kMaxAccountsInputBytes)
                        return failure("accounts source exceeds the 4 MB limit");
                    content.append(buffer.data(), static_cast<std::size_t>(read));
                }
            } else if (args.contains("text") && args["text"].is_string()) {
                content = args["text"].get<std::string>();
                if (content.size() > kMaxAccountsInputBytes)
                    throw std::invalid_argument("'text' exceeds the 4 MB limit");
            } else {
                throw std::invalid_argument("'path' or 'text' is required");
            }
            auto accounts = nxrth::core::parse_account_stats(content);
            if (accounts.empty()) return failure("no accounts parsed from the provided source");
            if (accounts.size() > kMaxAccountsSpawn)
                return failure("accounts source exceeds the 5000-record spawn limit");

            const std::string strategy = args.value("strategy", std::string("first"));
            const std::string mode = args.value("mode", std::string("standard"));
            if (strategy != "first" && strategy != "random" && strategy != "next" && strategy != "all")
                throw std::invalid_argument("strategy must be first, random, next, or all");
            if (mode != "standard" && mode != "ltoken")
                throw std::invalid_argument("mode must be standard or ltoken");
            const bool use_pool = args.value("use_pool_proxy", true);
            const std::size_t count = (strategy == "all")
                ? accounts.size()
                : (args.contains("count")
                       ? static_cast<std::size_t>(required_u32(
                             args, "count", static_cast<std::uint32_t>(kMaxAccountsSpawn)))
                       : accounts.size());
            if (count == 0) throw std::invalid_argument("'count' must be at least 1");

            std::vector<std::size_t> idxs;
            if (strategy == "all") {
                idxs.resize(accounts.size());
                std::iota(idxs.begin(), idxs.end(), std::size_t{0});
            } else if (strategy == "random") {
                idxs.resize(accounts.size());
                std::iota(idxs.begin(), idxs.end(), std::size_t{0});
                std::mt19937 rng{std::random_device{}()};
                std::shuffle(idxs.begin(), idxs.end(), rng);
                idxs.resize(std::min(count, idxs.size()));
            } else if (strategy == "next") {
                if (accounts_cursor_source_ != content) {
                    accounts_cursor_source_ = content;
                    accounts_cursor_ = 0;
                }
                if (accounts_cursor_ >= accounts.size()) accounts_cursor_ = 0;
                const std::size_t start = accounts_cursor_;
                const std::size_t end = std::min(start + count, accounts.size());
                for (std::size_t i = start; i < end; ++i) idxs.push_back(i);
                accounts_cursor_ = end;
            } else {  // first
                const std::size_t n = std::min(count, accounts.size());
                for (std::size_t i = 0; i < n; ++i) idxs.push_back(i);
            }

            int added = 0, skipped = 0, invalid_ltoken = 0;
            json bot_ids = json::array();
            for (std::size_t i : idxs) {
                const auto& a = accounts[i];
                std::optional<nxrth::bot::LtokenRecord> ltoken_record;
                if (mode == "ltoken") {
                    ltoken_record = nxrth::bot::parse_ltoken_string(a.login_token);
                    if (!ltoken_record) {
                        ++invalid_ltoken;
                        continue;
                    }
                }
                std::optional<nxrth::net::Socks5Config> proxy;
                std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy;
                if (use_pool) {
                    try { proxy = proxy_pool_.choose(manager_.proxy_key_counts()); } catch (...) {}
                    if (mode == "standard" ||
                        (ltoken_record && ltoken_record->kind ==
                                              nxrth::bot::LtokenRecord::Kind::ProviderToken)) {
                        try { login_proxy = proxy_pool_.rotating_login_proxy(); } catch (...) {}
                    }
                    // Pool exhausted: spawning proxy-less would login via the REAL IP
                    // (24h ban). Skip instead of leaking (mirrors the Database tab).
                    if (!proxy && (mode == "ltoken" || !login_proxy)) { ++skipped; continue; }
                }
                std::uint32_t id = 0;
                const auto proxy_policy = use_pool ? nxrth::bot::ProxyPolicy::Pool
                                                   : nxrth::bot::ProxyPolicy::Direct;
                if (mode == "ltoken") {
                    id = manager_.spawn_ltoken(a.login_token, std::move(proxy),
                                               std::move(login_proxy), proxy_policy);
                } else {
                    id = manager_.spawn(a.username, a.password, std::move(proxy),
                                        std::move(login_proxy), proxy_policy);
                }
                bot_ids.push_back(id);
                ++added;
            }
            return success({{"accepted", true}, {"parsed_accounts", accounts.size()},
                            {"requested", idxs.size()}, {"added", added}, {"skipped", skipped},
                            {"invalid_ltoken", invalid_ltoken},
                            {"strategy", strategy}, {"mode", mode},
                            {"next_cursor", accounts_cursor_}, {"bot_ids", std::move(bot_ids)}});
        }
        if (name == "proxy_status") {
            const auto view = proxy_pool_.view(manager_.proxy_key_counts());
            json entries = json::array();
            for (const auto& e : view.proxies)
                entries.push_back({{"index", e.index}, {"label", public_proxy_label(e.label)}, {"ip", e.ip},
                                   {"active", e.active}, {"capacity", e.capacity}, {"full", e.full}});
            json quarantined = json::array();
            for (const auto& q : nxrth::proxy::quarantined_proxies()) quarantined.push_back(q);
            return success({
                {"enabled", view.enabled},
                {"total", view.total}, {"available", view.available}, {"active", view.active},
                {"max_bots_per_ip", view.max_bots_per_ip}, {"spread_mode", view.spread_mode},
                {"shuffle_selection", view.shuffle_selection},
                {"proxies", std::move(entries)},
                {"rotating_login",
                 {{"enabled", view.rotating_login_enabled},
                  {"scheme", view.rotating_login_effective_scheme},
                  {"port_span", view.rotating_login_port_span},
                  {"label", view.rotating_login_proxy_label
                                ? json(public_proxy_label(*view.rotating_login_proxy_label))
                                : json(nullptr)}}},
                {"quarantined", std::move(quarantined)},
            });
        }
        if (name == "automation_get_config") {
            return success(automation_config_json(manager_.fleet()->config_snapshot()));
        }
        if (name == "automation_status") {
            return success(automation_status_json());
        }
        if (name == "automation_validate_config") {
            const auto cfg = manager_.fleet()->config_snapshot();
            return success(automation_validation_json(cfg));
        }
        if (name == "automation_define_group") {
            const std::string group = required_string(args, "group", 64);
            if (!args.contains("bot_ids")) throw std::invalid_argument("'bot_ids' is required");
            auto cfg = manager_.fleet()->config_snapshot();
            cfg.groups[group] = parse_bot_id_array(args.at("bot_ids"), "bot_ids");
            manager_.fleet()->set_config(cfg);
            return success({{"accepted", true}, {"group", group}, {"bot_ids", cfg.groups[group]},
                            {"automation", automation_config_json(cfg)}});
        }
        if (name == "automation_set_module") {
            const std::string module = required_string(args, "module", 32);
            if (!known_automation_module(module))
                throw std::invalid_argument("module must be geiger, collect, or coordinate");
            auto cfg = manager_.fleet()->config_snapshot();
            if (args.contains("enabled")) {
                if (!args["enabled"].is_boolean())
                    throw std::invalid_argument("'enabled' must be a boolean");
                cfg.enabled[module] = args["enabled"].get<bool>();
            }
            if (args.contains("params")) {
                if (!args["params"].is_object())
                    throw std::invalid_argument("'params' must be an object");
                for (const auto& [key, value] : args["params"].items()) {
                    if (key.empty() || key.size() > 64)
                        throw std::invalid_argument("automation param key length is invalid");
                    cfg.params[key] = scalar_to_param_string(value);
                }
            }
            if (args.contains("bot_ids")) {
                auto ids = parse_bot_id_array(args["bot_ids"], "bot_ids");
                if (ids.empty())
                    cfg.module_bot_ids.erase(module);
                else {
                    cfg.module_bot_ids[module] = std::move(ids);
                    cfg.module_groups.erase(module);
                }
            }
            if (args.contains("group")) {
                if (!args["group"].is_string())
                    throw std::invalid_argument("'group' must be a string");
                std::string group = args["group"].get<std::string>();
                if (group.empty())
                    cfg.module_groups.erase(module);
                else {
                    cfg.module_groups[module] = std::move(group);
                    cfg.module_bot_ids.erase(module);
                }
            }
            manager_.fleet()->set_config(cfg);
            return success({{"accepted", true}, {"automation", automation_config_json(cfg)},
                            {"validation", automation_validation_json(cfg)}});
        }
        if (name == "automation_configure_geiger" || name == "automation_start_geiger_farm") {
            auto cfg = manager_.fleet()->config_snapshot();
            if (name == "automation_start_geiger_farm") {
                cfg.enabled["geiger"] = true;
                if (!args.contains("dig")) cfg.params["geiger_dig"] = "1";
                if (!args.contains("wear")) cfg.params["geiger_wear"] = "1";
                if (!args.contains("recharge_min")) cfg.params["geiger_recharge_min"] = "30";
            } else if (args.contains("enabled")) {
                if (!args["enabled"].is_boolean())
                    throw std::invalid_argument("'enabled' must be a boolean");
                cfg.enabled["geiger"] = args["enabled"].get<bool>();
            } else {
                cfg.enabled["geiger"] = true;
            }

            set_string_param_if_present(cfg, args, "hunt_worlds", "geiger_hunt_worlds", 2048);
            set_string_param_if_present(cfg, args, "depot_worlds", "geiger_depot_worlds", 2048);
            set_string_param_if_present(cfg, args, "pickup_worlds", "geiger_pickup_worlds", 2048);
            set_string_param_if_present(cfg, args, "webhook_url", "geiger_webhook_url", 512);
            set_int_param_if_present(cfg, args, "geiger_item", "geiger_item", 1, 65535);
            set_bool_param_if_present(cfg, args, "wear", "geiger_wear");
            set_bool_param_if_present(cfg, args, "dig", "geiger_dig");
            set_int_param_if_present(cfg, args, "recharge_min", "geiger_recharge_min", 0, 1440);
            set_int_param_if_present(cfg, args, "min_y", "geiger_min_y", 0, 200);
            set_int_param_if_present(cfg, args, "max_y", "geiger_max_y", 1, 200);
            set_int_param_if_present(cfg, args, "world_width", "geiger_world_width", 4, 300);
            set_int_param_if_present(cfg, args, "signal_wait_ms", "geiger_signal_wait_ms", 500, 60000);
            set_int_param_if_present(cfg, args, "settle_ms", "geiger_settle_ms", 0, 10000);
            set_int_param_if_present(cfg, args, "max_steps", "geiger_max_steps", 1, 1000);
            set_int_param_if_present(cfg, args, "pickup_scan_ms", "geiger_pickup_scan_ms", 500, 60000);
            set_int_param_if_present(cfg, args, "pickup_empty_scans", "geiger_pickup_empty_scans", 1, 1000);

            const int min_y = std::stoi(cfg.param("geiger_min_y", "0"));
            const int max_y = std::stoi(cfg.param("geiger_max_y", "53"));
            if (max_y <= min_y) throw std::invalid_argument("'max_y' must be greater than 'min_y'");

            if (args.contains("bot_ids")) {
                auto ids = parse_bot_id_array(args["bot_ids"], "bot_ids");
                if (ids.empty())
                    cfg.module_bot_ids.erase("geiger");
                else {
                    cfg.module_bot_ids["geiger"] = std::move(ids);
                    cfg.module_groups.erase("geiger");
                }
            }
            if (args.contains("group")) {
                if (!args["group"].is_string())
                    throw std::invalid_argument("'group' must be a string");
                std::string group = args["group"].get<std::string>();
                if (group.empty())
                    cfg.module_groups.erase("geiger");
                else {
                    cfg.module_groups["geiger"] = std::move(group);
                    cfg.module_bot_ids.erase("geiger");
                }
            }

            manager_.fleet()->set_config(cfg);
            return success({{"accepted", true}, {"module", "geiger"},
                            {"enabled", cfg.is_on("geiger")},
                            {"automation", automation_config_json(cfg)},
                            {"validation", automation_validation_json(cfg)}});
        }
        if (name == "automation_stop_geiger_farm") {
            auto cfg = manager_.fleet()->config_snapshot();
            cfg.enabled["geiger"] = false;
            manager_.fleet()->set_config(cfg);
            return success({{"accepted", true}, {"module", "geiger"}, {"enabled", false},
                            {"automation", automation_config_json(cfg)}});
        }
        if (name == "automation_pause_all") {
            auto cfg = manager_.fleet()->config_snapshot();
            for (const std::string module : {"geiger", "collect", "coordinate"})
                cfg.enabled[module] = false;
            manager_.fleet()->set_config(cfg);
            return success({{"accepted", true}, {"automation", automation_config_json(cfg)}});
        }
        if (name == "automation_save_profile") {
            const std::string profile = required_profile_name(args);
            json profiles = read_profiles_file();
            profiles[profile] = automation_config_data_json(manager_.fleet()->config_snapshot());
            write_profiles_file(profiles);
            return success({{"accepted", true}, {"profile", profile},
                            {"path", automation_profiles_path().string()}});
        }
        if (name == "automation_load_profile") {
            const std::string profile = required_profile_name(args);
            json profiles = read_profiles_file();
            if (!profiles.contains(profile)) return failure("unknown automation profile: " + profile);
            nxrth::bot::AutomationConfig cfg;
            automation_config_from_json(profiles[profile], cfg);
            manager_.fleet()->set_config(cfg);
            return success({{"accepted", true}, {"profile", profile},
                            {"automation", automation_config_json(cfg)},
                            {"validation", automation_validation_json(cfg)}});
        }
        if (name == "automation_list_profiles") {
            json profiles = read_profiles_file();
            json out = json::array();
            for (const auto& [profile, data] : profiles.items()) {
                nxrth::bot::AutomationConfig cfg;
                json validation;
                try {
                    automation_config_from_json(data, cfg);
                    validation = automation_validation_json(cfg);
                } catch (const std::exception& e) {
                    validation = {{"valid", false},
                                  {"issues", json::array({std::string("profile parse failed: ") + e.what()})},
                                  {"warnings", json::array()}, {"preview", json::object()}};
                }
                out.push_back({{"name", profile}, {"validation", std::move(validation)}});
            }
            return success({{"profiles", std::move(out)},
                            {"path", automation_profiles_path().string()}});
        }
        if (name == "fleet_logs") {
            const auto limit = std::min<std::size_t>(
                args.value("limit", std::size_t{200}), 1000);
            std::optional<std::uint32_t> filter;
            if (args.contains("bot_id")) filter = required_u32(args, "bot_id");
            json lines = json::array();
            for (const auto& line : nxrth::Logger::Instance().Snapshot(limit)) {
                if (filter && line.bot_id != static_cast<int>(*filter)) continue;
                lines.push_back({{"bot_id", line.bot_id < 0 ? json(nullptr) : json(line.bot_id)},
                                 {"text", redact_log_line(line.text)}});
            }
            return success({{"filter_bot_id", filter ? json(*filter) : json(nullptr)},
                            {"count", lines.size()}, {"lines", std::move(lines)}});
        }
        const std::uint32_t bot_id = required_u32(args, "bot_id");
        if (name == "session_status") return success(bot_state_json(bot_id, true));
        if (name == "session_stop") {
            if (!manager_.stop(bot_id)) return failure("unknown bot_id");
            return success({{"accepted", true}, {"bot_id", bot_id}, {"removed", true}});
        }
        auto queued = [&](BotCommand command) {
            if (!enqueue(bot_id, std::move(command))) return failure("unknown bot_id or bot worker stopped");
            return success({{"accepted", true}, {"bot_id", bot_id}, {"execution", "queued"}});
        };
        if (name == "session_reconnect") return queued(cmd::Reconnect{});
        if (name == "session_set_logging")
            return queued(cmd::SetLogging{args.value("enabled", false)});
        if (name == "session_disconnect") return queued(cmd::Disconnect{});
        if (name == "world_leave") return queued(cmd::LeaveWorld{});
        if (name == "world_join")
            return queued(cmd::Warp{required_string(args, "world", 64), args.value("door_id", "")});
        if (name == "move_to")
            return queued(cmd::FindPath{required_u32(args, "x"), required_u32(args, "y")});
        if (name == "move_step")
            return queued(cmd::Move{static_cast<std::int32_t>(required_u32(args, "x", std::numeric_limits<std::int32_t>::max())),
                                    static_cast<std::int32_t>(required_u32(args, "y", std::numeric_limits<std::int32_t>::max()))});
        if (name == "chat_send") return queued(cmd::Say{required_string(args, "message", 120)});
        if (name == "chat_read") {
            const auto after = args.value("after", std::size_t{0});
            const auto limit = std::min<std::size_t>(args.value("limit", std::size_t{100}), 300);
            return success(chat_json(bot_id, after, limit));
        }
        if (name == "session_logs") {
            const auto limit = std::min<std::size_t>(args.value("limit", std::size_t{80}), 300);
            return success(logs_json(bot_id, limit));
        }
        if (name == "world_floating_items") {
            std::optional<float> radius;
            if (args.contains("radius_tiles")) {
                if (!args["radius_tiles"].is_number())
                    throw std::invalid_argument("'radius_tiles' must be a number");
                radius = args["radius_tiles"].get<float>();
                if (*radius < 0.1f || *radius > 200.0f)
                    throw std::invalid_argument("'radius_tiles' is out of range");
            }
            std::optional<std::uint32_t> item;
            if (args.contains("item_id")) item = required_u32(args, "item_id", 65535);
            return success(floating_items_json(bot_id, radius, item,
                                               args.value("name_contains", std::string{})));
        }
        if (name == "inventory_list") return success(inventory_json(bot_id));
        if (name == "inventory_drop")
            return queued(cmd::Drop{required_u32(args, "item_id", 65535), required_u32(args, "amount", 200)});
        if (name == "inventory_trash")
            return queued(cmd::Trash{required_u32(args, "item_id", 65535), required_u32(args, "amount", 200)});
        if (name == "inventory_use") {
            const auto item_id = required_u32(args, "item_id", 65535);
            const std::string mode = args.value("mode", "activate");
            if (mode == "activate") return queued(cmd::ActivateItem{item_id});
            if (mode == "equip") return queued(cmd::Wear{item_id});
            if (mode == "unequip") return queued(cmd::Unwear{item_id});
            throw std::invalid_argument("mode must be activate, equip, or unequip");
        }
        if (name == "inventory_collect") {
            if (!args.contains("uid")) return queued(cmd::CollectNearby{});
            float range = args.value("range_tiles", 3.0f);
            if (range < 1.0f || range > 5.0f) throw std::invalid_argument("range_tiles is out of range");
            return queued(cmd::CollectObject{required_u32(args, "uid"), range});
        }
        if (name == "sense_environment") {
            auto state = manager_.get_state(bot_id);
            if (!state) return failure("unknown bot_id");
            const std::uint32_t radius = std::clamp<std::uint32_t>(args.value("radius", 6u), 1, 20);
            const bool blocked_only = args.value("blocked_only", false);
            const auto cx = static_cast<std::int32_t>(state->pos_x);
            const auto cy = static_cast<std::int32_t>(state->pos_y);
            json tiles = json::array();
            for (std::int32_t y = std::max(0, cy - static_cast<int>(radius));
                 y <= cy + static_cast<int>(radius) && y < static_cast<std::int32_t>(state->world_height); ++y) {
                for (std::int32_t x = std::max(0, cx - static_cast<int>(radius));
                     x <= cx + static_cast<int>(radius) && x < static_cast<std::int32_t>(state->world_width); ++x) {
                    const std::size_t index = static_cast<std::size_t>(y) * state->world_width + x;
                    if (index >= state->tiles.size()) continue;
                    const auto& tile = state->tiles[index];
                    std::uint8_t collision = 0;
                    std::string item_name;
                    if (const auto* item = manager_.items_dat()->find_by_id(tile.fg_item_id)) {
                        collision = item->collision_type;
                        item_name = item->name;
                    }
                    const bool blocked = collision == 1 || collision == 6;
                    if (blocked_only && !blocked) continue;
                    tiles.push_back({{"x", x}, {"y", y}, {"fg_item_id", tile.fg_item_id},
                                     {"bg_item_id", tile.bg_item_id}, {"item_name", item_name},
                                     {"collision_type", collision}, {"blocked", blocked}});
                }
            }
            json out = world_json(bot_id);
            out["radius"] = radius;
            out["tiles"] = std::move(tiles);
            return success(std::move(out));
        }
        if (name == "world_action") {
            const std::string action = required_string(args, "action", 32);
            if (action == "punch") return queued(cmd::Hit{required_i32(args, "offset_x"), required_i32(args, "offset_y")});
            if (action == "place") return queued(cmd::Place{required_i32(args, "offset_x"), required_i32(args, "offset_y"), required_u32(args, "item_id", 65535)});
            if (action == "wrench") return queued(cmd::Wrench{required_i32(args, "offset_x"), required_i32(args, "offset_y")});
            if (action == "wrench_player") return queued(cmd::WrenchPlayer{required_u32(args, "net_id")});
            if (action == "activate_tile") return queued(cmd::ActivateTile{required_i32(args, "x"), required_i32(args, "y")});
            if (action == "enter") {
                std::optional<std::string> pass;
                if (args.contains("password")) pass = args["password"].get<std::string>();
                return queued(cmd::Enter{std::move(pass)});
            }
            if (action == "face") return queued(cmd::Face{args.value("left", false)});
            if (action == "respawn") return queued(cmd::Respawn{});
            if (action == "accept_access") return queued(cmd::AcceptAccess{});
            throw std::invalid_argument("unsupported world action");
        }
        return failure("unknown tool: " + name);
    } catch (const std::exception& e) {
        return failure(e.what());
    }
}

json McpServer::read_resource(const std::string& uri) {
    json data;
    if (uri == "nxrth://fleet") {
        json bots = json::array();
        for (const auto& info : manager_.list())
            bots.push_back({{"bot_id", info.id}, {"nickname", info.username}, {"status", info.status},
                            {"world", info.world}, {"x", info.pos_x}, {"y", info.pos_y},
                            {"ping_ms", info.ping_ms}, {"gems", info.gems}});
        data = {{"bots", std::move(bots)}};
    } else if (uri == "nxrth://automation") {
        data = automation_config_json(manager_.fleet()->config_snapshot());
    } else if (uri == "nxrth://automation/status") {
        data = automation_status_json();
    } else if (uri == "nxrth://logs") {
        json lines = json::array();
        for (const auto& line : nxrth::Logger::Instance().Snapshot(500)) {
            lines.push_back({{"bot_id", line.bot_id < 0 ? json(nullptr) : json(line.bot_id)},
                             {"text", redact_log_line(line.text)}});
        }
        data = {{"count", lines.size()}, {"lines", std::move(lines)}};
    } else if (uri == "nxrth://lua/api") {
        data = lua_api_reference_json();
    } else {
        auto parsed = parse_bot_uri(uri);
        if (!parsed) throw std::invalid_argument("unknown resource URI");
        const auto [id, kind] = *parsed;
        if (kind == "state") data = bot_state_json(id, true);
        else if (kind == "world") data = world_json(id);
        else if (kind == "objects") data = floating_items_json(id, std::nullopt, std::nullopt, "");
        else if (kind == "inventory") data = inventory_json(id);
        else if (kind == "chat") data = chat_json(id, 0, 300);
        else throw std::invalid_argument("unknown bot resource");
    }
    return {{"contents", json::array({{{"uri", uri}, {"mimeType", "application/json"},
                                        {"text", data.dump()}}})}};
}

int run_stdio_server() {
    enum class Route { Undecided, Desktop, Headless };
    Route route = Route::Undecided;
    bool desktop_required = false;
    if (const char* mode_env = std::getenv("NXRTH_MCP_MODE")) {
        const std::string mode = lower_ascii(mode_env);
        if (mode == "headless") route = Route::Headless;
        else if (mode == "app" || mode == "desktop") desktop_required = true;
    }

    std::unique_ptr<McpServer> headless_server;
    auto handle_headless = [&](const json& request) {
        if (!headless_server) headless_server = std::make_unique<McpServer>();
        return headless_server->handle(request);
    };
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json request;
        try {
            request = json::parse(line);
            std::optional<json> response;
            if (route == Route::Desktop) {
                if (!forward_request_to_app(request, response))
                    throw std::runtime_error("desktop Nxrth app is no longer available");
            } else if (route == Route::Headless) {
                response = handle_headless(request);
            } else if (forward_request_to_app(request, response)) {
                route = Route::Desktop;
            } else {
                if (desktop_required)
                    throw std::runtime_error(
                        "desktop Nxrth app is not running; start it before the MCP client");
                route = Route::Headless;
                response = handle_headless(request);
            }
            if (response)
                std::cout << response->dump() << '\n' << std::flush;
        } catch (const json::parse_error& e) {
            std::cout << rpc_error(nullptr, -32700, e.what()).dump() << '\n' << std::flush;
        } catch (const std::exception& e) {
            const json id = request.is_object() && request.contains("id")
                                ? request["id"]
                                : json(nullptr);
            std::cout << rpc_error(id, -32603, e.what()).dump() << '\n' << std::flush;
        }
    }
    return 0;
}

int run_self_test() {
    std::string ai_error;
    if (!nxrth::ai::AiController::self_test(&ai_error)) {
        std::cerr << "AI controller self-test failed: " << ai_error << '\n';
        return 18;
    }
    std::string fleet_store_error;
    if (!nxrth::recovery::FleetStore::self_test(&fleet_store_error)) {
        std::cerr << "Fleet store self-test failed: " << fleet_store_error << '\n';
        return 16;
    }
    std::string script_store_error;
    if (!nxrth::script::ScriptStore::self_test(&script_store_error)) {
        std::cerr << "Script store self-test failed: " << script_store_error << '\n';
        return 17;
    }
    std::string lua_error;
    if (!nxrth::lua::LuaEngine::self_test(&lua_error)) {
        std::cerr << "Lua self-test failed: " << lua_error << '\n';
        return 12;
    }
    McpServer server;
    auto init = server.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                               {"params", {{"protocolVersion", "2025-06-18"}}}});
    auto listed = server.handle({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    auto resources = server.handle({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "resources/list"}});
    if (!init || !listed || !resources) return 1;
    const auto& tools = (*listed)["result"]["tools"];
    if (!tools.is_array() || tools.size() < 15) return 2;
    for (const auto& entry : tools) {
        if (!entry.contains("name") || !entry.contains("inputSchema")) return 3;
    }
    const json* login_tool = nullptr;
    const json* lua_tool = nullptr;
    for (const auto& entry : tools) {
        if (entry.value("name", std::string{}) == "session_login") {
            login_tool = &entry;
        } else if (entry.value("name", std::string{}) == "lua_execute") {
            lua_tool = &entry;
        }
    }
    if (!login_tool || !lua_tool) return 4;
    const auto& lua_schema = (*lua_tool)["inputSchema"];
    if (lua_schema.value("additionalProperties", true) ||
        !lua_schema["properties"].contains("source") ||
        lua_schema["properties"]["source"].value("maxLength", 0) != 1048576 ||
        std::find(lua_schema["required"].begin(), lua_schema["required"].end(), "source") ==
            lua_schema["required"].end())
        return 13;

    bool found_lua_resource = false;
    for (const auto& resource : (*resources)["result"]["resources"]) {
        if (resource.value("uri", std::string{}) == "nxrth://lua/api") {
            found_lua_resource = true;
            break;
        }
    }
    if (!found_lua_resource) return 13;

    auto lua_ok = server.handle(
        {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"},
         {"params", {{"name", "lua_execute"},
                     {"arguments", {{"source", "print('MCP_LUA_OK')"}}}}}});
    if (!lua_ok || !(*lua_ok).contains("result")) return 14;
    const auto& lua_ok_result = (*lua_ok)["result"];
    if (lua_ok_result.value("isError", true) ||
        !lua_ok_result.contains("structuredContent") ||
        !lua_ok_result["structuredContent"].value("ok", false) ||
        lua_ok_result["structuredContent"].value("output", std::string{}).find("MCP_LUA_OK") ==
            std::string::npos ||
        !lua_ok_result["structuredContent"].value("error", std::string{}).empty() ||
        !lua_ok_result["structuredContent"]["added_bot_ids"].empty())
        return 14;

    const std::string mcp_lua_secret = std::string(128, 'Q') + "+/==";
    const std::string failing_source =
        "print('token:" + mcp_lua_secret + "')\nerror('" + mcp_lua_secret + "')";
    auto lua_failed = server.handle(
        {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
         {"params", {{"name", "lua_execute"},
                     {"arguments", {{"source", failing_source}}}}}});
    if (!lua_failed || !(*lua_failed).contains("result")) return 15;
    const auto& lua_failed_result = (*lua_failed)["result"];
    const std::string failed_dump = lua_failed_result.dump();
    if (!lua_failed_result.value("isError", false) ||
        !lua_failed_result["structuredContent"].contains("output") ||
        lua_failed_result["structuredContent"].value("ok", true) ||
        lua_failed_result["structuredContent"].value("output", std::string{}).find("<redacted>") ==
            std::string::npos ||
        lua_failed_result["structuredContent"].value("error", std::string{}).find("<redacted>") ==
            std::string::npos ||
        failed_dump.find(mcp_lua_secret) != std::string::npos)
        return 15;

    auto lua_api = server.handle(
        {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "resources/read"},
         {"params", {{"uri", "nxrth://lua/api"}}}});
    if (!lua_api || (*lua_api).dump().find("addBot(table)") == std::string::npos ||
        (*lua_api).dump().find("addgeigerstorageworld") == std::string::npos)
        return 13;

    const auto& methods = (*login_tool)["inputSchema"]["properties"]["method"]["enum"];
    for (const std::string required : {"growid", "ltoken"}) {
        if (std::find(methods.begin(), methods.end(), required) == methods.end()) return 4;
    }
    for (const auto& method : methods) {
        if (method.is_string() && method.get<std::string>().find("newly") != std::string::npos)
            return 4;
    }
    if (!(*login_tool)["inputSchema"]["properties"].contains("ltoken")) return 4;
    const std::string valid_ltoken =
        "oauth-refresh-token|" + std::string(32, 'A') + "|02:11:22:33:44:55|" +
        std::string(32, 'B');
    const std::string keyed_ltoken =
        "mac:02:00:00:00:00:00|wk:NONE0|platform:1|rid:" + std::string(32, 'C') +
        "|name:provider_bot|cbits:1536|playerAge:25|token:oauth-provider-token|"
        "vid:00000000-0000-0000-0000-000000000000|providerExtra:accepted";
    const auto parsed_keyed = nxrth::bot::parse_ltoken_string(keyed_ltoken);
    const auto parsed_google = nxrth::bot::parse_ltoken_string(valid_ltoken);
    const auto parsed_keyed_google = nxrth::bot::parse_ltoken_string(
        "mac:02:00:00:00:00:00|wk:NONE0|rid:" + std::string(32, 'D') +
        "|refreshToken:oauth-refresh-token");
    if (!parsed_google ||
        parsed_google->kind != nxrth::bot::LtokenRecord::Kind::GoogleRefreshToken ||
        !parsed_keyed_google ||
        parsed_keyed_google->kind != nxrth::bot::LtokenRecord::Kind::GoogleRefreshToken ||
        !parsed_keyed ||
        parsed_keyed->kind != nxrth::bot::LtokenRecord::Kind::ProviderToken ||
        parsed_keyed->token != "oauth-provider-token" || parsed_keyed->wk != "NONE0" ||
        parsed_keyed->platform_id.value_or("") != "1" ||
        parsed_keyed->name.value_or("") != "provider_bot" ||
        parsed_keyed->cbits.value_or("") != "1536" ||
        parsed_keyed->player_age.value_or("") != "25" ||
        nxrth::bot::parse_ltoken_string("token-only"))
        return 4;
    const auto keyed_accounts =
        nxrth::core::parse_account_stats(keyed_ltoken + "\n" + keyed_ltoken);
    if (keyed_accounts.size() != 1 || keyed_accounts.front().username != "provider_bot" ||
        keyed_accounts.front().login_token != keyed_ltoken)
        return 4;
    if (nxrth::bot::build_ltoken_gateway_packet("checked-token") !=
        "protocol|226\nltoken|checked-token\nplatformID|2\n")
        return 4;
    // Provider validate-ltoken gateway packet: FULL identity body + ltoken; the token
    // is byte-exact (base64 +,/,= survive — this ENet path is raw concat, NOT
    // form-encoded), protocol/platformID/meta/device fields present.
    nxrth::login::LoginIdentity pid;
    pid.game_version = "5.51"; pid.cbits = "1024"; pid.player_age = "19"; pid.gdpr = "1";
    pid.category = "_-5100"; pid.total_playtime = "0"; pid.country = "tr";
    pid.rid = std::string(32, 'C'); pid.mac = "02:00:00:00:00:00"; pid.wk = "NONE0";
    pid.hash = "-305319022"; pid.hash2 = "-305319022"; pid.klv = std::string(32, 'K');
    pid.platform_id = "1"; pid.vid = "00000000-0000-0000-0000-000000000000";
    const std::string provider_pkt =
        nxrth::bot::build_provider_gateway_packet(pid, "225", "META123", "aa+bb/cc=dd");
    for (const char* needle : {"\nltoken|aa+bb/cc=dd\n", "\nprotocol|225\n", "\nplatformID|1\n",
                               "\nmeta|META123\n", "\ngame_version|5.51\n", "\nwk|NONE0\n",
                               "\nvid|00000000-0000-0000-0000-000000000000\n", "\naid|\n",
                               "\nrid|CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC\n"}) {
        if (provider_pkt.find(needle) == std::string::npos) return 9;
    }
    // redirect packet carries user/token/UUIDToken instead of ltoken, same identity body.
    nxrth::bot::RedirectData rd;
    rd.user = "218796346"; rd.token = "6666183"; rd.uuid = "UUID"; rd.door_id = "0"; rd.lmode = "1";
    const std::string provider_redir =
        nxrth::bot::build_provider_redirect_packet(pid, "225", "META123", rd);
    if (provider_redir.find("\nuser|218796346\n") == std::string::npos ||
        provider_redir.find("\ntoken|6666183\n") == std::string::npos ||
        provider_redir.find("\nUUIDToken|UUID\n") == std::string::npos ||
        provider_redir.find("\naat|2\n") == std::string::npos ||
        provider_redir.find("ltoken|") != std::string::npos)  // redirect has NO ltoken
        return 9;

    // Secret-safe fingerprint: never echoes the secret, stable, distinct per input.
    const std::string secret_a = "aa+bb/cc=dd_super_secret_token_value";
    const std::string fp_a = nxrth::login::token_fingerprint(secret_a);
    if (fp_a.rfind("len=", 0) != 0 || fp_a.find(" sha=") == std::string::npos ||
        fp_a.find(secret_a) != std::string::npos ||
        fp_a.find("super_secret") != std::string::npos ||
        fp_a != nxrth::login::token_fingerprint(secret_a) ||
        fp_a == nxrth::login::token_fingerprint(secret_a + "x"))
        return 10;

    const std::string safe_proxy_log =
        redact_log_line("proxy socks5://secret_user:secret_pass@127.0.0.1:1080");
    if (safe_proxy_log.find("secret_user") != std::string::npos ||
        safe_proxy_log.find("secret_pass") != std::string::npos)
        return 5;
    const std::string safe_keyed_log =
        redact_log_line("mac:02:00:00:00:00:00|token:secret_oauth_value|vid:test");
    if (safe_keyed_log.find("secret_oauth_value") != std::string::npos ||
        safe_keyed_log.find("|vid:test") == std::string::npos)
        return 5;
    const std::string safe_packet_log = redact_log_line(
        "=== RAW LOGIN PACKET ===\nprotocol|226\nltoken|secret_game_token\n"
        "platformID|1\ncbits|1536\nplayer_age|25\n");
    if (safe_packet_log.find("secret_game_token") != std::string::npos ||
        safe_packet_log.find("platformID|1") == std::string::npos ||
        safe_packet_log.find("player_age|25") == std::string::npos)
        return 5;

    // Hot bot loops retain immutable config handles instead of deep-copying the
    // config maps every tick. Replacing the fleet config must publish a new
    // handle while an in-flight tick can safely finish with the old one.
    nxrth::bot::FleetState fleet;
    nxrth::bot::AutomationConfig first;
    first.enabled["geiger"] = true;
    first.params["geiger_hunt_worlds"] = "FIRST";
    fleet.set_config(first);
    const auto old_handle = fleet.config_handle();
    if (!old_handle || !old_handle->is_on("geiger") ||
        old_handle->param("geiger_hunt_worlds") != "FIRST")
        return 6;

    nxrth::bot::AutomationConfig second;
    second.enabled["geiger"] = false;
    second.params["geiger_hunt_worlds"] = "SECOND";
    fleet.set_config(second);
    const auto new_handle = fleet.config_handle();
    if (!new_handle || new_handle == old_handle || new_handle->is_on("geiger") ||
        new_handle->param("geiger_hunt_worlds") != "SECOND")
        return 7;
    if (old_handle->param("geiger_hunt_worlds") != "FIRST") return 8;

    // Group 11: world-parser resilience. A SendMapData blob whose tile carries an
    // unsupported extra-data kind must yield a PADDED PARTIAL world (tiles kept +
    // tail padded to width*height), never a total parse abort. A fully-valid blob
    // must still round-trip objects/weather.
    {
        std::vector<std::uint8_t> b;
        auto pu16 = [&](std::uint16_t v) {
            b.push_back(static_cast<std::uint8_t>(v & 0xFF));
            b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        };
        auto pu32 = [&](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        };
        pu16(25);                     // version (>= MAP_VERSION_MIN)
        pu32(0);                      // world flags
        pu16(1); b.push_back('T');    // world_name: u16 len + "T"
        pu32(2);                      // width
        pu32(1);                      // height
        pu32(2);                      // tile_count
        for (int i = 0; i < 5; ++i) b.push_back(0);  // pad
        pu16(0); pu16(0); pu16(0); pu16(0);          // tile 0: empty Basic
        pu16(6666); pu16(0); pu16(0); pu16(0x0001);  // tile 1: HAS_EXTRA_DATA
        b.push_back(7);                               // kind 7 = unsupported -> truncates
        auto partial = nxrth::world::World::try_parse(b.data(), b.size());
        if (!partial) return 11;                                    // must not abort
        if (partial->tile_map.tiles.size() != 2) return 11;         // kept + padded
        if (partial->tile_map.tiles[0].fg_item_id != 0) return 11;
        if (partial->tile_map.width != 2) return 11;

        std::vector<std::uint8_t> g;
        auto gu16 = [&](std::uint16_t v) {
            g.push_back(static_cast<std::uint8_t>(v & 0xFF));
            g.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        };
        auto gu32 = [&](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) g.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        };
        gu16(25); gu32(0);
        gu16(1); g.push_back('T'); gu32(1); gu32(1); gu32(1);
        for (int i = 0; i < 5; ++i) g.push_back(0);
        gu16(0); gu16(0); gu16(0); gu16(0);        // one empty tile
        for (int i = 0; i < 12; ++i) g.push_back(0);  // tile-array trailer
        gu32(0); gu32(0);                           // objects: count=0, last_uid=0
        gu16(0); gu16(0); gu16(0);                  // base_weather, pad, current_weather
        auto full = nxrth::world::World::try_parse(g.data(), g.size());
        if (!full || full->tile_map.tiles.size() != 1 || full->next_object_uid != 1) return 11;
    }
    return 0;
}

}  // namespace nxrth::mcp
