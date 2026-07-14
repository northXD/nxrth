// Adonai — fleet coordination automation module + shared claim helpers.
//
// Ported intent: Mori's per-bot Lua `rotation.pool*` API (port spec 11 §6.5) —
// bots claim/release farm worlds from a shared pool so the fleet spreads out
// instead of colliding on the same world/target. In Adonai there is NO Lua: the
// shared pool is the process-wide FleetState (bot/fleet_state.h), and this native
// module drives each Bot through its public action helpers (bot/bot.h) while
// coordinating via FleetState::claim/release/owner.
//
// This header ALSO exposes the small claim-key + claim/release helpers that other
// automation modules (e.g. CollectModule) reuse so every module speaks the same
// FleetState key vocabulary ("tile:WORLD:x,y" / "obj:WORLD:uid" / "world:WORLD").
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bot/bot.h"          // adonai::bot::{AutomationModule, BotContext, Bot}
#include "bot/fleet_state.h"  // adonai::bot::{FleetState, AutomationConfig}

namespace adonai::automation {

// ---------------------------------------------------------------------------
// Shared claim-key builders (the fleet's coordination vocabulary). All modules
// MUST route claims through these so keys never diverge. Matches the format
// documented on FleetState::claim in bot/fleet_state.h.
// ---------------------------------------------------------------------------
std::string tile_key(std::string_view world, std::uint32_t x, std::uint32_t y);
std::string obj_key(std::string_view world, std::uint32_t uid);
std::string world_claim_key(std::string_view world);

// Thin, reusable claim/release wrappers (any module: "claim a target before
// working it"). claim_target returns true iff the caller now owns `key`.
bool claim_target(adonai::bot::FleetState& fleet, const std::string& key, std::uint32_t bot_id);
void release_target(adonai::bot::FleetState& fleet, const std::string& key, std::uint32_t bot_id);

// ASCII case-insensitive compare / upper (GT world names are uppercase on the
// wire; config-supplied names are normalized to match).
bool iequals(std::string_view a, std::string_view b);
std::string to_upper(std::string_view s);

// One world the fleet may spread across. `door` is the warp door id ("" = none).
struct WorldTarget {
    std::string world;  // uppercased
    std::string door;
};
// Parse a config value like "WORLDA|door1, WORLDB, WORLDC|door2" into targets.
// Blank entries are skipped (mirrors the rotation-pool blank-world skip).
std::vector<WorldTarget> parse_world_list(const std::string& csv);

// ---------------------------------------------------------------------------
// CoordinateModule — spreads the fleet across a configured set of worlds.
//
// Config (AutomationConfig, read each tick via FleetState::config_snapshot):
//   enabled["coordinate"] = true         -> module runs (gated by run_automation)
//   params ["coordinate_worlds"]         -> "WORLDA|door, WORLDB, ..."
//
// Each tick: if this bot already sits in a world it owns the claim for, it just
// renews the claim. Otherwise it claims the first free world from the list
// (owner == none OR self) and warps there, releasing any previously held world
// claim. A dead/reaped bot's claims are freed by FleetState::release_all, so a
// hung bot's world frees up for the rest of the fleet.
// ---------------------------------------------------------------------------
class CoordinateModule : public adonai::bot::AutomationModule {
public:
    static constexpr const char* kName = "coordinate";

    const char* name() const override { return kName; }
    void tick(adonai::bot::BotContext& self, adonai::bot::FleetState& fleet) override;

private:
    std::string claimed_world_;  // "world:" key currently owned (uppercased world)
    std::chrono::steady_clock::time_point last_warp_{};
};

}  // namespace adonai::automation
