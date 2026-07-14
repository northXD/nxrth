// Adonai — Discord webhook reporter (fleet-wide, in-engine).
//
// Ported intent: Mori's lua/webhook.rs Discord reporting. In Mori each bot could
// report itself; Adonai reports the WHOLE fleet in one message from a single
// "leader" bot (the lowest live bot id), built from the shared FleetState
// snapshot. Rate-limited, and — fixing the Mori bug where the webhook mutex was
// held across the HTTP request and serialized the fleet — the lock guards ONLY
// the quick rate-limit check; the POST runs unlocked.
//
// Config (AutomationConfig):
//   enabled["webhook"]              = true
//   params ["webhook_url"]          = "https://discord.com/api/webhooks/..."
//   params ["webhook_interval_secs"]= "60"   (default)
#pragma once

#include "bot/bot.h"          // adonai::bot::{AutomationModule, BotContext}
#include "bot/fleet_state.h"  // adonai::bot::FleetState

namespace adonai::automation {

class WebhookModule : public adonai::bot::AutomationModule {
public:
    static constexpr const char* kName = "webhook";

    const char* name() const override { return kName; }
    void tick(adonai::bot::BotContext& self, adonai::bot::FleetState& fleet) override;
};

}  // namespace adonai::automation
