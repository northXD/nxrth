// Adonai — automation module registry / factory.
//
// Builds the set of native, fleet-aware AutomationModules (bot/bot.h) from an
// AutomationConfig (bot/fleet_state.h) so BotManager can attach them to each Bot
// via Bot::add_automation_module. Replaces Mori's per-bot Lua script host: there
// is no script text, only these C++ modules coordinating through the shared
// FleetState.
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
#include "automation/modules/webhook.h"
#include "bot/bot.h"          // adonai::bot::AutomationModule
#include "bot/fleet_state.h"  // adonai::bot::AutomationConfig

namespace adonai::automation {

using ModulePtr = std::unique_ptr<adonai::bot::AutomationModule>;
using ModuleList = std::vector<ModulePtr>;

// Every known module, in tick order (coordinate positions/warps the bot first,
// then collect grabs whatever is underfoot). Per-tick config gating still applies.
inline ModuleList build_all() {
    ModuleList mods;
    mods.push_back(std::make_unique<CoordinateModule>());  // position/spread first
    mods.push_back(std::make_unique<GeigerModule>());      // home on the prize signal
    mods.push_back(std::make_unique<CollectModule>());     // grab whatever's underfoot
    mods.push_back(std::make_unique<WebhookModule>());     // report the fleet (leader only)
    return mods;
}

// Only the modules switched on in `cfg` (same tick order as build_all).
inline ModuleList build_enabled(const adonai::bot::AutomationConfig& cfg) {
    ModuleList mods;
    if (cfg.is_on(CoordinateModule::kName))
        mods.push_back(std::make_unique<CoordinateModule>());
    if (cfg.is_on(GeigerModule::kName)) mods.push_back(std::make_unique<GeigerModule>());
    if (cfg.is_on(CollectModule::kName)) mods.push_back(std::make_unique<CollectModule>());
    if (cfg.is_on(WebhookModule::kName)) mods.push_back(std::make_unique<WebhookModule>());
    return mods;
}

// Attach the enabled modules to a bot in one call (BotManager spawn helper).
inline void attach_enabled(adonai::bot::Bot& bot, const adonai::bot::AutomationConfig& cfg) {
    for (auto& m : build_enabled(cfg)) bot.add_automation_module(std::move(m));
}

}  // namespace adonai::automation
