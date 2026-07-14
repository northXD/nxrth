#include "mcp/mcp_server.h"

#include <algorithm>
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

#include "core/accounts.h"
#include "core/logger.h"
#include "mcp/app_bridge.h"
#include "world/items.h"

namespace adonai::mcp {
namespace {

using json = nlohmann::json;
using adonai::bot::BotCommand;
namespace cmd = adonai::bot::cmd;

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

json failure(std::string message) {
    return {{"content", json::array({{{"type", "text"}, {"text", message}}})},
            {"isError", true}};
}

json rpc_error(const json& id, int code, std::string message) {
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", {{"code", code}, {"message", std::move(message)}}}};
}

json state_summary(std::uint32_t id, const adonai::bot::BotState& state) {
    const std::string status = adonai::bot::to_string(state.status);
    const bool connected = state.status == adonai::bot::BotStatus::Connected ||
                           state.status == adonai::bot::BotStatus::InGame;
    return {{"bot_id", id}, {"nickname", state.username}, {"status", status},
            {"connected", connected}, {"world", state.world_name},
            {"position", {{"x", state.pos_x}, {"y", state.pos_y}, {"unit", "tiles"}}},
            {"ping_ms", state.ping_ms}, {"gems", state.gems}};
}

json item_json(const adonai::bot::InvSlot& slot,
               const std::shared_ptr<const adonai::world::ItemsDat>& items) {
    json out = {{"item_id", slot.item_id}, {"amount", slot.amount},
                {"active", slot.is_active}, {"action_type", slot.action_type}};
    if (items) {
        if (const auto* info = items->find_by_id(slot.item_id)) out["name"] = info->name;
    }
    return out;
}

bool known_automation_module(const std::string& module) {
    return module == "geiger" || module == "collect" || module == "coordinate" ||
           module == "webhook";
}

json supported_automation_json() {
    return {
        {"modules", json::array({"geiger", "collect", "coordinate", "webhook"})},
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
             {"webhook",
              {
                  {"webhook_url", "Discord webhook URL for fleet status."},
                  {"webhook_interval_secs", "Webhook update interval in seconds. Default 60."},
              }},
         }},
    };
}

json automation_config_data_json(const adonai::bot::AutomationConfig& cfg) {
    json enabled = json::object();
    for (const auto& [key, value] : cfg.enabled) enabled[key] = value;
    json params = json::object();
    for (const auto& [key, value] : cfg.params) params[key] = value;
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

json automation_config_json(const adonai::bot::AutomationConfig& cfg) {
    json out = automation_config_data_json(cfg);
    out["supported"] = supported_automation_json();
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

void automation_config_from_json(const json& j, adonai::bot::AutomationConfig& cfg) {
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

void set_string_param_if_present(adonai::bot::AutomationConfig& cfg, const json& args,
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

void set_int_param_if_present(adonai::bot::AutomationConfig& cfg, const json& args,
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

void set_bool_param_if_present(adonai::bot::AutomationConfig& cfg, const json& args,
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
    const std::vector<std::string> markers = {
        "Got token:", "token|", "ltoken|", "requestedName|", "tankIDPass|", "password|"};
    for (const auto& marker : markers) {
        const auto pos = line.find(marker);
        if (pos != std::string::npos) return line.substr(0, pos + marker.size()) + " <redacted>";
    }
    return line;
}

bool blank_param(const adonai::bot::AutomationConfig& cfg, const std::string& key) {
    return trim_copy(cfg.param(key)).empty();
}

int int_param_or(const adonai::bot::AutomationConfig& cfg, const std::string& key,
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

json automation_validation_json(const adonai::bot::AutomationConfig& cfg) {
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
    if (cfg.is_on("webhook") && blank_param(cfg, "webhook_url"))
        warnings.push_back("webhook_url is empty; webhook module cannot post");

    return {{"valid", issues.empty()}, {"issues", std::move(issues)},
            {"warnings", std::move(warnings)}, {"preview", std::move(preview)}};
}

std::uint32_t inventory_amount(const adonai::bot::BotState& state, std::uint16_t item_id) {
    std::uint32_t total = 0;
    for (const auto& slot : state.inventory)
        if (slot.item_id == item_id) total += slot.amount;
    return total;
}

json geiger_signal_json(const std::optional<adonai::bot::GeigerSignal>& sig) {
    if (!sig) return nullptr;
    return {{"x", sig->x}, {"y", sig->y}, {"type", adonai::bot::as_str(sig->area_type)},
            {"timestamp_ms", sig->timestamp_ms}};
}

std::string geiger_phase_for_bot(const adonai::bot::AutomationConfig& cfg,
                                 const adonai::bot::BotState& state,
                                 std::uint32_t bot_id) {
    if (!cfg.is_on_for("geiger", bot_id)) return "disabled";
    if (blank_param(cfg, "geiger_hunt_worlds")) return "blocked:no_hunt_worlds";
    if (state.status != adonai::bot::BotStatus::InGame) return "waiting_for_world";
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

json floating_object_json(std::uint32_t bot_id, const adonai::bot::BotState& state,
                          const adonai::bot::WorldObjectInfo& o,
                          const std::shared_ptr<const adonai::world::ItemsDat>& items) {
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
    constexpr std::string_view prefix = "adonai://bot/";
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

}  // namespace

McpServer::McpServer()
    : owned_manager_(std::make_unique<adonai::bot::BotManager>(nullptr)),
      owned_proxy_pool_(std::make_unique<adonai::proxy::ProxyPool>(
          adonai::proxy::ProxyPool::load_default())),
      manager_(*owned_manager_),
      proxy_pool_(*owned_proxy_pool_) {}

McpServer::McpServer(adonai::bot::BotManager& manager,
                     adonai::proxy::ProxyPool& proxy_pool)
    : manager_(manager), proxy_pool_(proxy_pool) {}

json McpServer::tool_definitions() {
    const json id = bot_id_property();
    const json coord = {{"type", "integer"}, {"minimum", 0},
                        {"description", "Absolute tile coordinate."}};
    const json offset = {{"type", "integer"}, {"minimum", -4}, {"maximum", 4},
                         {"description", "Tile offset from the bot; Growtopia reach is +/-4."}};
    json tools = json::array();
    tools.push_back(tool("session_login",
        "Start a bot and connect it to Growtopia. Credentials are consumed locally and never returned.",
        object_schema({{"username", {{"type", "string"}, {"minLength", 1}, {"maxLength", 512}}},
                       {"password", {{"type", "string"}, {"maxLength", 512},
                                     {"description", "Required for growid/newly; omit for ltoken."}}},
                       {"method", {{"type", "string"}, {"enum", {"growid", "newly", "ltoken"}},
                                   {"default", "growid"}}},
                       {"use_configured_proxy", {{"type", "boolean"}, {"default", true}}}},
                      {"username"}))); 
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
    tools.push_back(tool("accounts_spawn",
        "Batch-login accounts from accounts_stats.json (or pasted user:pass / JSON), mirroring the desktop Database tab's First/Random/Next/All buttons. Spawns into the shared fleet with pool proxies, and SKIPS (never leaks the real IP) when the pool is exhausted. Provide 'path' or 'text'.",
        object_schema({{"path", {{"type", "string"}, {"maxLength", 1024},
                                 {"description", "Path to accounts_stats.json or a user:pass file. Provide path OR text."}}},
                       {"text", {{"type", "string"}, {"maxLength", 4000000},
                                 {"description", "Pasted accounts_stats.json or user:pass lines. Provide path OR text."}}},
                       {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000},
                                  {"description", "How many accounts to spawn. Ignored when strategy=all."}}},
                       {"strategy", {{"type", "string"}, {"enum", {"first", "random", "next", "all"}},
                                     {"default", "first"},
                                     {"description", "first=[0,X); random=X distinct random; next=advancing cursor batches; all=every parsed account."}}},
                       {"mode", {{"type", "string"}, {"enum", {"standard", "newly"}}, {"default", "standard"}}},
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
                                   {"enum", {"geiger", "collect", "coordinate", "webhook"}}}},
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
                      {"serverInfo", {{"name", "adonai-mcp"}, {"version", "0.1.0"}}},
                      {"instructions", "Control Growtopia bots and fleet automations. Mutating bot tools enqueue work; automation tools update the shared fleet config live. Poll session_status or resources to observe completion."}};
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
            result = {{"resources", json::array({{{"uri", "adonai://fleet"},
                                                   {"name", "Adonai bot fleet"},
                                                   {"mimeType", "application/json"}},
                                                  {{"uri", "adonai://automation"},
                                                   {"name", "Fleet automation config"},
                                                   {"mimeType", "application/json"}},
                                                  {{"uri", "adonai://automation/status"},
                                                   {"name", "Fleet automation status"},
                                                   {"mimeType", "application/json"}},
                                                  {{"uri", "adonai://logs"},
                                                   {"name", "Shared application logs"},
                                                   {"mimeType", "application/json"}}})}};
        } else if (method == "resources/templates/list") {
            result = {{"resourceTemplates", json::array({
                {{"uriTemplate", "adonai://bot/{bot_id}/state"}, {"name", "Bot state"}, {"mimeType", "application/json"}},
                {{"uriTemplate", "adonai://bot/{bot_id}/world"}, {"name", "World snapshot"}, {"mimeType", "application/json"}},
                {{"uriTemplate", "adonai://bot/{bot_id}/objects"}, {"name", "Floating ground items"}, {"mimeType", "application/json"}},
                {{"uriTemplate", "adonai://bot/{bot_id}/inventory"}, {"name", "Inventory"}, {"mimeType", "application/json"}},
                {{"uriTemplate", "adonai://bot/{bot_id}/chat"}, {"name", "Chat buffer"}, {"mimeType", "application/json"}}
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
    for (const std::string module : {"geiger", "collect", "coordinate", "webhook"})
        active_by_module[module] = json::array();

    for (const auto& info : manager_.list()) {
        auto state = manager_.get_state(info.id);
        if (!state) continue;
        json active_modules = json::array();
        for (const std::string module : {"geiger", "collect", "coordinate", "webhook"}) {
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
            {"status", adonai::bot::to_string(state->status)},
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
    for (const std::string module : {"geiger", "collect", "coordinate", "webhook"}) {
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
        if (name == "session_login") {
            const std::string username = required_string(args, "username", 512);
            const std::string method = args.value("method", "growid");
            const std::string password = args.value("password", "");
            if (method != "growid" && method != "newly" && method != "ltoken")
                throw std::invalid_argument("method must be growid, newly, or ltoken");
            if (method != "ltoken" && password.empty())
                throw std::invalid_argument("password is required for growid/newly");
            if (method == "ltoken" && username.find('|') == std::string::npos)
                throw std::invalid_argument("ltoken username must contain the token fields");
            std::optional<adonai::net::Socks5Config> proxy;
            std::optional<adonai::proxy::RotatingLoginProxy> login_proxy;
            if (args.value("use_configured_proxy", true)) {
                proxy = proxy_pool_.choose(manager_.proxy_key_counts());
                login_proxy = proxy_pool_.rotating_login_proxy();
            }
            std::uint32_t id = method == "newly"
                ? manager_.spawn_newly(username, password, std::move(proxy), std::move(login_proxy))
                : (method == "ltoken"
                    ? manager_.spawn_ltoken(username, std::move(proxy), std::move(login_proxy))
                    : manager_.spawn(username, password, std::move(proxy), std::move(login_proxy)));
            return success({{"accepted", true}, {"bot_id", id}, {"status", "connecting"}});
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
                std::ifstream in(path, std::ios::binary);
                if (!in) return failure("cannot open accounts file: " + path);
                std::stringstream ss;
                ss << in.rdbuf();
                content = ss.str();
            } else if (args.contains("text") && args["text"].is_string()) {
                content = args["text"].get<std::string>();
            } else {
                throw std::invalid_argument("'path' or 'text' is required");
            }
            auto accounts = adonai::core::parse_account_stats(content);
            if (accounts.empty()) return failure("no accounts parsed from the provided source");

            const std::string strategy = args.value("strategy", std::string("first"));
            const std::string mode = args.value("mode", std::string("standard"));
            if (strategy != "first" && strategy != "random" && strategy != "next" && strategy != "all")
                throw std::invalid_argument("strategy must be first, random, next, or all");
            if (mode != "standard" && mode != "newly")
                throw std::invalid_argument("mode must be standard or newly");
            const bool use_pool = args.value("use_pool_proxy", true);
            const std::size_t count = (strategy == "all")
                ? accounts.size()
                : (args.contains("count")
                       ? static_cast<std::size_t>(required_u32(args, "count"))
                       : accounts.size());

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

            int added = 0, skipped = 0;
            json bot_ids = json::array();
            for (std::size_t i : idxs) {
                const auto& a = accounts[i];
                std::optional<adonai::net::Socks5Config> proxy;
                std::optional<adonai::proxy::RotatingLoginProxy> login_proxy;
                if (use_pool) {
                    try { proxy = proxy_pool_.choose(manager_.proxy_key_counts()); } catch (...) {}
                    try { login_proxy = proxy_pool_.rotating_login_proxy(); } catch (...) {}
                    // Pool exhausted: spawning proxy-less would login via the REAL IP
                    // (24h ban). Skip instead of leaking (mirrors the Database tab).
                    if (!proxy && !login_proxy) { ++skipped; continue; }
                }
                const std::uint32_t id = (mode == "newly")
                    ? manager_.spawn_newly(a.username, a.password, std::move(proxy),
                                           std::move(login_proxy))
                    : manager_.spawn(a.username, a.password, std::move(proxy),
                                     std::move(login_proxy));
                bot_ids.push_back(id);
                ++added;
            }
            return success({{"accepted", true}, {"parsed_accounts", accounts.size()},
                            {"requested", idxs.size()}, {"added", added}, {"skipped", skipped},
                            {"strategy", strategy}, {"mode", mode},
                            {"next_cursor", accounts_cursor_}, {"bot_ids", std::move(bot_ids)}});
        }
        if (name == "proxy_status") {
            const auto view = proxy_pool_.view(manager_.proxy_key_counts());
            json entries = json::array();
            for (const auto& e : view.proxies)
                entries.push_back({{"index", e.index}, {"label", e.label}, {"ip", e.ip},
                                   {"active", e.active}, {"capacity", e.capacity}, {"full", e.full}});
            json quarantined = json::array();
            for (const auto& q : adonai::proxy::quarantined_proxies()) quarantined.push_back(q);
            return success({
                {"enabled", view.enabled},
                {"total", view.total}, {"available", view.available}, {"active", view.active},
                {"max_bots_per_ip", view.max_bots_per_ip}, {"spread_mode", view.spread_mode},
                {"proxies", std::move(entries)},
                {"rotating_login",
                 {{"enabled", view.rotating_login_enabled},
                  {"scheme", view.rotating_login_effective_scheme},
                  {"port_span", view.rotating_login_port_span},
                  {"label", view.rotating_login_proxy_label
                                ? json(*view.rotating_login_proxy_label)
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
                throw std::invalid_argument("module must be geiger, collect, coordinate, or webhook");
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
            for (const std::string module : {"geiger", "collect", "coordinate", "webhook"})
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
            adonai::bot::AutomationConfig cfg;
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
                adonai::bot::AutomationConfig cfg;
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
            for (const auto& line : adonai::Logger::Instance().Snapshot(limit)) {
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
    if (uri == "adonai://fleet") {
        json bots = json::array();
        for (const auto& info : manager_.list())
            bots.push_back({{"bot_id", info.id}, {"nickname", info.username}, {"status", info.status},
                            {"world", info.world}, {"x", info.pos_x}, {"y", info.pos_y},
                            {"ping_ms", info.ping_ms}, {"gems", info.gems}});
        data = {{"bots", std::move(bots)}};
    } else if (uri == "adonai://automation") {
        data = automation_config_json(manager_.fleet()->config_snapshot());
    } else if (uri == "adonai://automation/status") {
        data = automation_status_json();
    } else if (uri == "adonai://logs") {
        json lines = json::array();
        for (const auto& line : adonai::Logger::Instance().Snapshot(500)) {
            lines.push_back({{"bot_id", line.bot_id < 0 ? json(nullptr) : json(line.bot_id)},
                             {"text", redact_log_line(line.text)}});
        }
        data = {{"count", lines.size()}, {"lines", std::move(lines)}};
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
    if (const char* mode_env = std::getenv("ADONAI_MCP_MODE")) {
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
                    throw std::runtime_error("desktop Adonai app is no longer available");
            } else if (route == Route::Headless) {
                response = handle_headless(request);
            } else if (forward_request_to_app(request, response)) {
                route = Route::Desktop;
            } else {
                if (desktop_required)
                    throw std::runtime_error(
                        "desktop Adonai app is not running; start it before the MCP client");
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
    return 0;
}

}  // namespace adonai::mcp
