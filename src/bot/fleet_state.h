// Nxrth — FleetState: the SHARED, mutex-guarded fleet registry that makes
// automation fleet-aware (ARCHITECTURE "FleetState + in-engine automation";
// port spec 09 §5.3). One instance owned by the BotManager, a std::shared_ptr
// handed to every bot. Every bot publishes its own compact view periodically and
// can read every other bot, claim shared targets (no double-work), and consult
// shared world knowledge.
//
// The ONE many-writer / many-reader structure: guarded by a std::shared_mutex.
// Critical sections are tiny (copy in / out, no bot logic under the lock).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bot/bot_state.h"  // BotStatus

namespace nxrth::bot {

// A compact, copyable snapshot of one bot — what every other bot can see.
struct BotView {
    std::uint32_t id = 0;
    std::string username;
    BotStatus status = BotStatus::Connecting;
    std::string world_name;
    float pos_x = 0.0f;  // TILES
    float pos_y = 0.0f;  // TILES
    std::int32_t gems = 0;
    std::uint32_t ping_ms = 0;
    std::optional<std::string> proxy_key;  // "ip:port" for peer-aware balancing
};

// Shared knowledge of a world some bot has visited (coordination hint, not the
// authoritative per-bot World model).
struct WorldShare {
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t last_seen_ms = 0;
    std::uint32_t last_seen_by = 0;  // bot id that last published it
};

// Which automation modules are enabled + their params (ARCHITECTURE config).
struct AutomationConfig {
    std::unordered_map<std::string, bool> enabled;
    std::unordered_map<std::string, std::string> params;
    std::unordered_map<std::string, std::vector<std::uint32_t>> groups;
    std::unordered_map<std::string, std::vector<std::uint32_t>> module_bot_ids;
    std::unordered_map<std::string, std::string> module_groups;

    bool is_on(const std::string& name) const {
        auto it = enabled.find(name);
        return it != enabled.end() && it->second;
    }
    bool is_on_for(const std::string& name, std::uint32_t bot_id) const {
        if (!is_on(name)) return false;
        auto bots = module_bot_ids.find(name);
        if (bots != module_bot_ids.end()) {
            // Explicit per-bot scope present (even if EMPTY). ONLY these bots run
            // the module; an empty scope means NOBODY. This lets a module stay
            // enabled + configurable while no bot auto-runs it, so a freshly-spawned
            // bot never auto-starts (e.g. AutoGeiger warping on spawn). An ABSENT
            // key still means fleet-wide (legacy / "all bots").
            for (std::uint32_t id : bots->second)
                if (id == bot_id) return true;
            return false;
        }
        auto group = module_groups.find(name);
        if (group != module_groups.end() && !group->second.empty()) {
            auto members = groups.find(group->second);
            if (members == groups.end()) return false;
            for (std::uint32_t id : members->second)
                if (id == bot_id) return true;
            return false;
        }
        return true;
    }
    std::string param(const std::string& key, const std::string& fallback = {}) const {
        auto it = params.find(key);
        return it != params.end() ? it->second : fallback;
    }
};

// The shared fleet registry. Thread-safe; all methods take the internal lock.
class FleetState {
public:
    // --- per-bot views (a bot publishes its own each tick; O(1)) -------------
    void upsert(const BotView& v);
    void erase(std::uint32_t id);  // also releases every claim owned by `id`
    std::optional<BotView> get(std::uint32_t id) const;
    std::vector<BotView> snapshot() const;                 // whole fleet
    std::vector<BotView> in_world(std::string_view world) const;  // peers in a world
    std::size_t count_on_proxy(std::string_view proxy_key) const;  // live occupancy

    // --- claims: "tile:WORLD:x,y" / "obj:WORLD:uid" -> owning bot id ----------
    // true if the caller now owns the claim (freshly acquired OR already owner).
    bool claim(const std::string& key, std::uint32_t bot_id);
    void release(const std::string& key, std::uint32_t bot_id);  // no-op unless owner
    void release_all(std::uint32_t bot_id);                       // on stop/reap
    std::optional<std::uint32_t> owner(const std::string& key) const;

    // --- shared world knowledge ---------------------------------------------
    void upsert_world(const WorldShare& w);
    std::optional<WorldShare> get_world(const std::string& name) const;

    // --- automation config ---------------------------------------------------
    // UI/MCP callers can request a value snapshot. Hot bot loops retain an
    // immutable shared handle, so config maps/strings are copied only when the
    // user actually changes the config.
    AutomationConfig config_snapshot() const;
    std::shared_ptr<const AutomationConfig> config_handle() const;
    void set_config(AutomationConfig cfg);

private:
    mutable std::shared_mutex mtx_;
    std::unordered_map<std::uint32_t, BotView> members_;
    std::unordered_map<std::string, std::uint32_t> claims_;
    std::unordered_map<std::string, WorldShare> worlds_;
    std::shared_ptr<const AutomationConfig> config_ = std::make_shared<AutomationConfig>();
};

using FleetHandle = std::shared_ptr<FleetState>;

}  // namespace nxrth::bot
