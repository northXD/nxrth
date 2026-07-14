// Adonai — BotStatus / GeigerArea string mappings (declared in bot_state.h).
// The BotStatus strings are matched verbatim by the UI, so keep them stable.
#include "bot/bot_state.h"

namespace adonai::bot {

const char* to_string(BotStatus s) {
    switch (s) {
        case BotStatus::Connecting: return "connecting";
        case BotStatus::Connected: return "connected";
        case BotStatus::InGame: return "in_game";
        case BotStatus::TwoFactorAuth: return "two_factor_auth";
        case BotStatus::ServerOverloaded: return "server_overloaded";
        case BotStatus::TooManyLogins: return "too_many_logins";
        case BotStatus::UpdateRequired: return "update_required";
        case BotStatus::Maintenance: return "maintenance";
    }
    return "connecting";
}

const char* as_str(GeigerArea a) {
    switch (a) {
        case GeigerArea::Red: return "red";
        case GeigerArea::Yellow: return "yellow";
        case GeigerArea::Green: return "green";
        case GeigerArea::RapidGreen: return "rapid_green";
        case GeigerArea::Prize: return "prize";
    }
    return "red";
}

}  // namespace adonai::bot
