// Nxrth - AutomationConfig disk persistence.
#include "automation/config_store.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace nxrth::automation {
namespace {

std::filesystem::path cfg_path() {
    std::filesystem::path p;
    try {
        p = std::filesystem::current_path();
    } catch (...) {
        p = ".";
    }
    return p / "data" / "automation_config.json";
}

}  // namespace

void save_automation_config(const nxrth::bot::AutomationConfig& cfg) {
    nlohmann::json j;

    nlohmann::json enabled = nlohmann::json::object();
    for (const auto& [k, v] : cfg.enabled) enabled[k] = v;
    j["enabled"] = std::move(enabled);

    nlohmann::json params = nlohmann::json::object();
    for (const auto& [k, v] : cfg.params) params[k] = v;
    j["params"] = std::move(params);

    nlohmann::json groups = nlohmann::json::object();
    for (const auto& [k, v] : cfg.groups) groups[k] = v;
    j["groups"] = std::move(groups);

    nlohmann::json module_bot_ids = nlohmann::json::object();
    for (const auto& [k, v] : cfg.module_bot_ids) module_bot_ids[k] = v;
    j["module_bot_ids"] = std::move(module_bot_ids);

    nlohmann::json module_groups = nlohmann::json::object();
    for (const auto& [k, v] : cfg.module_groups) module_groups[k] = v;
    j["module_groups"] = std::move(module_groups);

    try {
        const auto p = cfg_path();
        std::error_code ec;
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << j.dump(2);
    } catch (...) {
    }
}

bool load_automation_config(nxrth::bot::AutomationConfig& out) {
    try {
        std::ifstream in(cfg_path(), std::ios::binary);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        const auto j = nlohmann::json::parse(ss.str());

        if (j.contains("enabled") && j["enabled"].is_object())
            for (auto it = j["enabled"].begin(); it != j["enabled"].end(); ++it)
                out.enabled[it.key()] = it.value().get<bool>();
        if (j.contains("params") && j["params"].is_object())
            for (auto it = j["params"].begin(); it != j["params"].end(); ++it)
                out.params[it.key()] = it.value().get<std::string>();
        if (j.contains("groups") && j["groups"].is_object())
            for (auto it = j["groups"].begin(); it != j["groups"].end(); ++it)
                out.groups[it.key()] = it.value().get<std::vector<std::uint32_t>>();
        if (j.contains("module_bot_ids") && j["module_bot_ids"].is_object())
            for (auto it = j["module_bot_ids"].begin(); it != j["module_bot_ids"].end(); ++it)
                out.module_bot_ids[it.key()] = it.value().get<std::vector<std::uint32_t>>();
        if (j.contains("module_groups") && j["module_groups"].is_object())
            for (auto it = j["module_groups"].begin(); it != j["module_groups"].end(); ++it)
                out.module_groups[it.key()] = it.value().get<std::string>();

        // Removed legacy fleet-status webhook. Prize reporting now lives only
        // under AutoGeiger's geiger_webhook_url.
        out.enabled.erase("webhook");
        out.params.erase("webhook_url");
        out.params.erase("webhook_interval_secs");
        out.module_bot_ids.erase("webhook");
        out.module_groups.erase("webhook");
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace nxrth::automation
