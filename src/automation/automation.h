// Nxrth — automation module registry / factory.
//
// Builds the set of native, fleet-aware AutomationModules (bot/bot.h) from an
// AutomationConfig (bot/fleet_state.h) so BotManager can attach them to each Bot
// via Bot::add_automation_module. Native modules coordinate through the shared
// FleetState; the separate fleet-level Lua executor controls these modules and
// bot command queues without creating a Lua VM inside every Bot.
//
// Note: Bot::run_automation re-checks AutomationConfig::is_on(module->name())
// every tick, so a module can be toggled live once attached. Use build_all() to
// attach every module and drive it purely by config toggles, or build_enabled()
// to attach only those switched on at spawn time.
#pragma once

#include <memory>
#include <vector>

#include "automation/modules/collect.h"
#include "automation/modules/coordinate.h"
#include "automation/modules/geiger.h"
#include "bot/bot.h"          // nxrth::bot::AutomationModule
#include "bot/fleet_state.h"  // nxrth::bot::AutomationConfig

namespace nxrth::automation {

using ModulePtr = std::unique_ptr<nxrth::bot::AutomationModule>;
using ModuleList = std::vector<ModulePtr>;

// Every known module, in tick order (coordinate positions/warps the bot first,
// then collect grabs whatever is underfoot). Per-tick config gating still applies.
inline ModuleList build_all() {
    ModuleList mods;
    mods.push_back(std::make_unique<CoordinateModule>());  // position/spread first
    mods.push_back(std::make_unique<GeigerModule>());      // home on the prize signal
    mods.push_back(std::make_unique<CollectModule>());     // grab whatever's underfoot
    return mods;
}

// Only the modules switched on in `cfg` (same tick order as build_all).
inline ModuleList build_enabled(const nxrth::bot::AutomationConfig& cfg) {
    ModuleList mods;
    if (cfg.is_on(CoordinateModule::kName))
        mods.push_back(std::make_unique<CoordinateModule>());
    if (cfg.is_on(GeigerModule::kName)) mods.push_back(std::make_unique<GeigerModule>());
    if (cfg.is_on(CollectModule::kName)) mods.push_back(std::make_unique<CollectModule>());
    return mods;
}

// Attach the enabled modules to a bot in one call (BotManager spawn helper).
inline void attach_enabled(nxrth::bot::Bot& bot, const nxrth::bot::AutomationConfig& cfg) {
    for (auto& m : build_enabled(cfg)) bot.add_automation_module(std::move(m));
}

}  // namespace nxrth::automation
