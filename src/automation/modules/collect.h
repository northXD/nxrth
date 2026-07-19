// Nxrth — auto-pickup automation module (fleet-coordinated).
//
// Ported intent: Mori's per-bot auto-collect + Lua `collect(range, interval)`
// (port spec 11 §1 collect / §6). The engine already has Bot::collect() /
// Bot::collect_object_at() (bot/bot.h) doing the nearest-first, path-checked
// pickup. What Mori's per-bot Lua could NOT do is coordinate: two bots standing
// on the same drop pile both chase every object. This native module adds the
// fleet coordination the whole port exists for — before a bot activates a ground
// object it CLAIMS "obj:WORLD:uid" in the shared FleetState, so each dropped item
// is chased by exactly one bot. Stale claims (item picked up / bot left world)
// are released so the claim map never leaks.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

#include "bot/bot.h"          // nxrth::bot::{AutomationModule, BotContext}
#include "bot/fleet_state.h"  // nxrth::bot::FleetState

namespace nxrth::automation {

// ---------------------------------------------------------------------------
// CollectModule — coordinated auto-pickup.
//
// Config (shared immutable AutomationConfig passed by Bot::run_automation):
//   enabled["collect"]          = true  -> module runs (gated by run_automation)
//   params ["collect_radius"]   = "3"   -> pickup radius in tiles, clamped 1..5
//
// Each tick: releases claims for objects that vanished from the world, then, for
// every ground object within radius (nearest first), claims "obj:WORLD:uid" in
// FleetState and — only if this bot now owns that claim — drives the engine's
// Bot::collect_object_at() to grab it. Objects owned by another bot are skipped.
// ---------------------------------------------------------------------------
class CollectModule : public nxrth::bot::AutomationModule {
public:
    static constexpr const char* kName = "collect";

    const char* name() const override { return kName; }
    void tick(nxrth::bot::BotContext& self, nxrth::bot::FleetState& fleet,
              const nxrth::bot::AutomationConfig& config) override;

private:
    std::string world_name_;                    // uppercased world we hold claims in
    std::unordered_set<std::string> my_claims_;  // "obj:" keys this bot currently owns
};

}  // namespace nxrth::automation
