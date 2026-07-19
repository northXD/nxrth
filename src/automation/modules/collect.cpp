// Nxrth — CollectModule: fleet-coordinated auto-pickup (see collect.h).
#include "automation/modules/collect.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "automation/modules/coordinate.h"  // obj_key / claim / release / to_upper
#include "world/world.h"                     // nxrth::world::World / WorldObject / TILE_SIZE

namespace nxrth::automation {

namespace {

// Parse the "collect_radius" param, clamped to the engine's 1..5 collect range.
std::uint32_t parse_radius(const std::string& s) {
    long v = s.empty() ? 3 : std::strtol(s.c_str(), nullptr, 10);
    if (v < 1) v = 1;
    if (v > 5) v = 5;
    return static_cast<std::uint32_t>(v);
}

}  // namespace

void CollectModule::tick(nxrth::bot::BotContext& self, nxrth::bot::FleetState& fleet,
                         const nxrth::bot::AutomationConfig& config) {
    const std::uint32_t id = self.bot_id();
    const auto& w = self.world();

    // Not in a world (or left it): drop every claim we held and reset.
    if (!w) {
        for (const std::string& k : my_claims_) release_target(fleet, k, id);
        my_claims_.clear();
        world_name_.clear();
        return;
    }

    const std::string world = to_upper(w->tile_map.world_name);
    if (world != world_name_) {
        // World changed under us — our old-world claims are meaningless now.
        for (const std::string& k : my_claims_) release_target(fleet, k, id);
        my_claims_.clear();
        world_name_ = world;
    }

    // Release claims for objects that are no longer on the ground (collected by
    // us or despawned) so the shared claim map never grows without bound.
    if (!my_claims_.empty()) {
        std::unordered_set<std::string> present;
        present.reserve(w->objects.size());
        for (const auto& o : w->objects) present.insert(obj_key(world, o.uid));
        for (auto it = my_claims_.begin(); it != my_claims_.end();) {
            if (present.find(*it) == present.end()) {
                release_target(fleet, *it, id);
                it = my_claims_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Inventory full: nothing to pick up this tick (claims already reconciled).
    const auto& inv = self.inventory();
    if (inv.size != 0 && inv.item_count >= inv.size) return;

    const std::uint32_t radius = parse_radius(config.param("collect_radius"));
    const float r_px = static_cast<float>(radius) * nxrth::world::TILE_SIZE;
    const float px = self.pos_x();
    const float py = self.pos_y();

    // Gather nearby drops (Chebyshev box, mirroring Bot::collect), nearest first.
    struct Near {
        float ring;
        std::uint32_t uid;
    };
    std::vector<Near> nearby;
    for (const auto& o : w->objects) {
        const float dx = std::fabs(px - o.x);
        const float dy = std::fabs(py - o.y);
        if (dx > r_px || dy > r_px) continue;
        nearby.push_back(Near{std::max(dx, dy), o.uid});
    }
    if (nearby.empty()) return;
    std::sort(nearby.begin(), nearby.end(),
              [](const Near& a, const Near& b) { return a.ring < b.ring; });

    const std::size_t limit =
        std::min<std::size_t>(nearby.size(), nxrth::bot::COLLECT_MAX_PER_TICK);
    for (std::size_t i = 0; i < limit; ++i) {
        const std::string key = obj_key(world, nearby[i].uid);
        const auto owner = fleet.owner(key);
        if (owner && *owner != id) continue;             // another bot is chasing it
        if (!claim_target(fleet, key, id)) continue;     // lost the race
        my_claims_.insert(key);
        self.collect_object_at(nearby[i].uid, static_cast<float>(radius));
    }
}

}  // namespace nxrth::automation
