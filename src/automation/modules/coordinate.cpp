// Adonai — CoordinateModule + shared claim helpers (see coordinate.h).
#include "automation/modules/coordinate.h"

#include <algorithm>
#include <cctype>

#include "core/logger.h"

namespace adonai::automation {

namespace {

// Trim ASCII whitespace from both ends.
std::string_view trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Minimum wall time between re-warps so a bot never spams the warp gate while
// its target is momentarily unclaimable / not yet loaded.
constexpr std::chrono::milliseconds kWarpCooldown{5000};

}  // namespace

std::string to_upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string tile_key(std::string_view world, std::uint32_t x, std::uint32_t y) {
    std::string k = "tile:";
    k.append(world);
    k.push_back(':');
    k.append(std::to_string(x));
    k.push_back(',');
    k.append(std::to_string(y));
    return k;
}

std::string obj_key(std::string_view world, std::uint32_t uid) {
    std::string k = "obj:";
    k.append(world);
    k.push_back(':');
    k.append(std::to_string(uid));
    return k;
}

std::string world_claim_key(std::string_view world) {
    std::string k = "world:";
    k.append(world);
    return k;
}

bool claim_target(adonai::bot::FleetState& fleet, const std::string& key, std::uint32_t bot_id) {
    return fleet.claim(key, bot_id);
}

void release_target(adonai::bot::FleetState& fleet, const std::string& key, std::uint32_t bot_id) {
    fleet.release(key, bot_id);
}

std::vector<WorldTarget> parse_world_list(const std::string& csv) {
    std::vector<WorldTarget> out;
    std::size_t start = 0;
    while (start <= csv.size()) {
        std::size_t comma = csv.find(',', start);
        std::string_view entry =
            trim(std::string_view(csv).substr(start, comma == std::string::npos
                                                          ? std::string::npos
                                                          : comma - start));
        if (!entry.empty()) {
            std::size_t bar = entry.find('|');
            std::string_view name = bar == std::string_view::npos ? entry : entry.substr(0, bar);
            std::string_view door =
                bar == std::string_view::npos ? std::string_view{} : entry.substr(bar + 1);
            name = trim(name);
            door = trim(door);
            if (!name.empty()) out.push_back(WorldTarget{to_upper(name), std::string(door)});
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

void CoordinateModule::tick(adonai::bot::BotContext& self, adonai::bot::FleetState& fleet) {
    const adonai::bot::AutomationConfig cfg = fleet.config_snapshot();
    const std::vector<WorldTarget> targets = parse_world_list(cfg.param("coordinate_worlds"));
    if (targets.empty()) return;

    const std::uint32_t id = self.bot_id();
    const auto& w = self.world();
    const std::string current = w ? to_upper(w->tile_map.world_name) : std::string();

    // Already sitting in one of our target worlds: renew the claim and hold.
    if (!current.empty()) {
        for (const WorldTarget& t : targets) {
            if (t.world != current) continue;
            const std::string key = world_claim_key(t.world);
            if (claim_target(fleet, key, id)) {
                // Drop a stale claim on a different world we may still hold.
                if (!claimed_world_.empty() && claimed_world_ != t.world)
                    release_target(fleet, world_claim_key(claimed_world_), id);
                claimed_world_ = t.world;
                return;
            }
        }
    }

    // Pick the first world that is free (unowned) or already ours.
    for (const WorldTarget& t : targets) {
        const std::string key = world_claim_key(t.world);
        const auto owner = fleet.owner(key);
        if (owner && *owner != id) continue;  // taken by another bot
        if (!claim_target(fleet, key, id)) continue;

        if (!claimed_world_.empty() && claimed_world_ != t.world)
            release_target(fleet, world_claim_key(claimed_world_), id);
        claimed_world_ = t.world;

        if (current == t.world) return;  // claimed the world we're already in

        const auto now = std::chrono::steady_clock::now();
        if (now - last_warp_ < kWarpCooldown) return;  // don't spam the warp gate
        last_warp_ = now;
        adonai::log("[coordinate] bot " + std::to_string(id) + " -> " + t.world,
                    static_cast<int>(id));
        self.warp(t.world, t.door);
        return;
    }
    // Nothing claimable this tick: every target is owned by other bots. Hold.
}

}  // namespace adonai::automation
