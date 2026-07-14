// Adonai — FleetState implementation (port spec 09 §5.3).
#include "bot/fleet_state.h"

#include <algorithm>

namespace adonai::bot {

void FleetState::upsert(const BotView& v) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    members_[v.id] = v;
}

void FleetState::erase(std::uint32_t id) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    members_.erase(id);
    for (auto it = claims_.begin(); it != claims_.end();) {
        if (it->second == id)
            it = claims_.erase(it);
        else
            ++it;
    }
}

std::optional<BotView> FleetState::get(std::uint32_t id) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    auto it = members_.find(id);
    if (it == members_.end()) return std::nullopt;
    return it->second;
}

std::vector<BotView> FleetState::snapshot() const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    std::vector<BotView> out;
    out.reserve(members_.size());
    for (const auto& [id, v] : members_) out.push_back(v);
    return out;
}

std::vector<BotView> FleetState::in_world(std::string_view world) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    std::vector<BotView> out;
    for (const auto& [id, v] : members_)
        if (v.world_name == world) out.push_back(v);
    return out;
}

std::size_t FleetState::count_on_proxy(std::string_view proxy_key) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    std::size_t n = 0;
    for (const auto& [id, v] : members_)
        if (v.proxy_key && *v.proxy_key == proxy_key) ++n;
    return n;
}

bool FleetState::claim(const std::string& key, std::uint32_t bot_id) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    auto it = claims_.find(key);
    if (it == claims_.end()) {
        claims_.emplace(key, bot_id);
        return true;
    }
    return it->second == bot_id;  // already ours == still owned
}

void FleetState::release(const std::string& key, std::uint32_t bot_id) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    auto it = claims_.find(key);
    if (it != claims_.end() && it->second == bot_id) claims_.erase(it);
}

void FleetState::release_all(std::uint32_t bot_id) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    for (auto it = claims_.begin(); it != claims_.end();) {
        if (it->second == bot_id)
            it = claims_.erase(it);
        else
            ++it;
    }
}

std::optional<std::uint32_t> FleetState::owner(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    auto it = claims_.find(key);
    if (it == claims_.end()) return std::nullopt;
    return it->second;
}

void FleetState::upsert_world(const WorldShare& w) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    worlds_[w.name] = w;
}

std::optional<WorldShare> FleetState::get_world(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    auto it = worlds_.find(name);
    if (it == worlds_.end()) return std::nullopt;
    return it->second;
}

AutomationConfig FleetState::config_snapshot() const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    return config_;
}

void FleetState::set_config(AutomationConfig cfg) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    config_ = std::move(cfg);
}

}  // namespace adonai::bot
