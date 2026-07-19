// Nxrth — Bot infrastructure methods + packet-text helpers that the split
// bot_connection/handlers/world translation units all rely on but none defined
// (each assumed a sibling did). Implemented here once.
#include "bot/bot.h"

#include <cstddef>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "bot/bot_state.h"
#include "core/logger.h"

namespace nxrth::bot {

// ---------------------------------------------------------------------------
// Free helpers (declared in bot.h)
// ---------------------------------------------------------------------------

// Redact sensitive values (tokens / passwords) in packet text before it is
// logged. Replaces the value after "<key>|" up to the end of the line.
std::string redact_packet_text(const std::string& text) {
    std::string out = text;
    for (const char* key : {"tankIDPass", "ltoken", "token", "password"}) {
        const std::string needle = std::string(key) + "|";
        std::size_t pos = 0;
        while ((pos = out.find(needle, pos)) != std::string::npos) {
            const std::size_t vstart = pos + needle.size();
            std::size_t vend = out.find('\n', vstart);
            if (vend == std::string::npos) vend = out.size();
            out.replace(vstart, vend - vstart, "<redacted>");
            pos = vstart + 10;  // past the inserted "<redacted>"
        }
    }
    return out;
}

// Parse Growtopia's line-based "key|value" text into a map. A line's value is
// everything after the FIRST '|' (values may themselves contain '|'). Trailing
// '\r' is stripped so CRLF payloads parse cleanly.
std::unordered_map<std::string, std::string> parse_pipe_map(const std::string& s) {
    std::unordered_map<std::string, std::string> map;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t nl = s.find('\n', start);
        std::string line =
            s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t bar = line.find('|');
        if (bar != std::string::npos) map[line.substr(0, bar)] = line.substr(bar + 1);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return map;
}

// ---------------------------------------------------------------------------
// Bot infra methods (declared in bot.h)
// ---------------------------------------------------------------------------

void Bot::log_console(const std::string& msg) {
    // Per-bot SYSTEM log ring (the detail "Logs" tab reads BotState.console).
    // Opt-in: only recorded while the bot's "Enable logs" toggle is on, so a fresh
    // fleet does not accumulate per-bot logs nobody asked for. The shared/global
    // Logger below is still written (fleet_logs / shared Console stay available).
    if (state_) {
        state_->write([&](BotState& s) {
            if (!s.logs_enabled) return;
            s.console.push_back(msg);
            if (s.console.size() > CONSOLE_RING_CAP) s.console.pop_front();
        });
    }
    // Forward to the UI event sink (it writes the shared Logger the global Console
    // panel reads). Headless (no sink) -> write the Logger directly so nothing is lost.
    if (sink_)
        sink_->console(bot_id_, msg);
    else
        nxrth::log(msg, static_cast<int>(bot_id_));
}

void Bot::log_chat(const std::string& msg) {
    // In-game console/chat ring (the detail "Console" tab reads BotState.chat),
    // kept separate from the system Logs. Also forwarded to the shared Logger.
    if (state_) {
        state_->write([&](BotState& s) {
            s.chat.push_back(msg);
            if (s.chat.size() > CONSOLE_RING_CAP) {
                s.chat.pop_front();
                ++s.chat_base_index;
            }
        });
    }
    if (sink_)
        sink_->console(bot_id_, msg);
    else
        nxrth::log(msg, static_cast<int>(bot_id_));
}

void Bot::emit_status(BotStatus status) {
    if (state_) state_->write([&](BotState& s) { s.status = status; });
    notify_dirty();
}

void Bot::notify_dirty() {
    if (sink_) sink_->dirty(bot_id_);
}

void Bot::emit_traffic(const std::string& direction, const std::string& kind, std::size_t size,
                       const std::string& detail) {
    if (sink_)
        sink_->traffic(bot_id_, direction, kind, size, /*summary=*/detail, /*detail=*/detail,
                       /*timestamp_ms=*/0);
    // Mirror into the per-bot Traffic ring for the desktop UI (IN/OUT prefix drives
    // the Show Incoming / Show Outgoing filters). Kept only while capture is enabled.
    if (state_) {
        std::string line = (direction == "out" ? "OUT " : "IN  ") + kind + "  " +
                           std::to_string(size) + "b" + (detail.empty() ? "" : "  " + detail);
        state_->write([&](BotState& s) {
            if (!s.traffic_enabled) return;
            s.traffic.push_back(std::move(line));
            while (s.traffic.size() > CONSOLE_RING_CAP) s.traffic.pop_front();
        });
    }
}

// Dispatch a UI/automation command to the matching action helper.
void Bot::handle_command(const BotCommand& cmd) {
    std::visit(
        [this](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, cmd::Move>)
                walk(c.x, c.y);
            else if constexpr (std::is_same_v<T, cmd::WalkTo>)
                find_path(c.x, c.y);
            else if constexpr (std::is_same_v<T, cmd::RunScript>)
                log_console("[Bot] scripts are not supported in Nxrth (native automation only)");
            else if constexpr (std::is_same_v<T, cmd::StopScript>)
                script_stop_->store(true);
            else if constexpr (std::is_same_v<T, cmd::Say>)
                say(c.text);
            else if constexpr (std::is_same_v<T, cmd::Warp>)
                warp(c.name, c.id);
            else if constexpr (std::is_same_v<T, cmd::Disconnect>)
                disconnect();
            else if constexpr (std::is_same_v<T, cmd::Reconnect>)
                reconnect();
            else if constexpr (std::is_same_v<T, cmd::Place>)
                place(c.x, c.y, c.item, /*is_punch=*/false);
            else if constexpr (std::is_same_v<T, cmd::Hit>)
                punch(c.x, c.y);
            else if constexpr (std::is_same_v<T, cmd::Wrench>)
                wrench(c.x, c.y);
            else if constexpr (std::is_same_v<T, cmd::Wear>)
                wear(c.item_id);
            else if constexpr (std::is_same_v<T, cmd::Unwear>)
                unwear(c.item_id);
            else if constexpr (std::is_same_v<T, cmd::Drop>)
                drop_item(c.item_id, c.count);  // 2-step dialog (manual drop, like Rust bot:drop)
            else if constexpr (std::is_same_v<T, cmd::Trash>)
                trash_item(c.item_id, c.count);
            else if constexpr (std::is_same_v<T, cmd::LeaveWorld>)
                leave_world();
            else if constexpr (std::is_same_v<T, cmd::Respawn>)
                respawn();
            else if constexpr (std::is_same_v<T, cmd::FindPath>)
                find_path(c.x, c.y);
            else if constexpr (std::is_same_v<T, cmd::SetDelays>)
                delays_ = c.delays;
            else if constexpr (std::is_same_v<T, cmd::SetAutoCollect>)
                set_auto_collect(c.enabled);
            else if constexpr (std::is_same_v<T, cmd::SetCollectConfig>) {
                collect_radius_tiles_ = c.radius_tiles;
                collect_blacklist_ =
                    std::unordered_set<std::uint16_t>(c.blacklist.begin(), c.blacklist.end());
            } else if constexpr (std::is_same_v<T, cmd::SetAutoReconnect>) {
                auto_reconnect_ = c.enabled;
                // Mirror into the shared snapshot so the UI toggle reflects reality
                // (otherwise it reads the always-true default and snaps back ON,
                // silently leaving the REAL flag off -> warp redirects never recover).
                if (state_) state_->write([&](BotState& s) { s.auto_reconnect = c.enabled; });
                notify_dirty();
            }
            else if constexpr (std::is_same_v<T, cmd::AcceptAccess>)
                accept_access();
            else if constexpr (std::is_same_v<T, cmd::CollectObject>)
                collect_object_at(c.uid, c.range_tiles);
            else if constexpr (std::is_same_v<T, cmd::CollectNearby>)
                collect();
            else if constexpr (std::is_same_v<T, cmd::ActivateItem>)
                activate_item(c.item_id);
            else if constexpr (std::is_same_v<T, cmd::ActivateTile>)
                active_tile(c.x, c.y);
            else if constexpr (std::is_same_v<T, cmd::Enter>)
                enter(c.password);
            else if constexpr (std::is_same_v<T, cmd::Face>)
                set_direction(c.left);
            else if constexpr (std::is_same_v<T, cmd::WrenchPlayer>)
                wrench_player(c.net_id);
            else if constexpr (std::is_same_v<T, cmd::SetTrafficLog>) {
                const bool on = c.enabled;
                if (state_) state_->write([&](BotState& s) {
                    s.traffic_enabled = on;
                    if (!on) s.traffic.clear();
                });
            }
            else if constexpr (std::is_same_v<T, cmd::SetLogging>) {
                const bool on = c.enabled;
                if (state_) state_->write([&](BotState& s) {
                    s.logs_enabled = on;
                    if (!on) s.console.clear();
                });
            }
        },
        cmd);
}

}  // namespace nxrth::bot
