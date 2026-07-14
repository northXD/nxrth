// Adonai — process-global fleet stagger gates (port specs 06 §2.1 / 07 §2.12).
#include "bot/gates.h"

#include <algorithm>
#include <thread>

namespace adonai::bot {

namespace {
using Clock = std::chrono::steady_clock;
inline Clock::duration ms(std::uint64_t v) {
    return std::chrono::milliseconds(v);
}
}  // namespace

Gate& login_packet_gate() { static Gate g; return g; }
Gate& enter_game_gate()   { static Gate g; return g; }
Gate& http_login_gate()   { static Gate g; return g; }
Gate& warp_gate()         { static Gate g; return g; }
Gate& gateway_logon_gate(){ static Gate g; return g; }
Gate& dashboard_gate()    { static Gate g; return g; }

SteadyTp reserve_gate_slot(Gate& gate, std::uint64_t spacing_ms, std::uint64_t max_ahead_ms) {
    std::lock_guard<std::mutex> lk(gate.m);
    auto now = Clock::now();
    if (!gate.inited) {
        gate.next_allowed = now;
        gate.inited = true;
    }
    auto horizon = now + ms(max_ahead_ms);
    auto slot = std::clamp(gate.next_allowed, now, horizon);
    gate.next_allowed = std::min(slot + ms(spacing_ms), horizon);
    return slot;
}

std::uint64_t wait_global_gate(Gate& gate, std::uint64_t spacing_ms) {
    auto slot = reserve_gate_slot(gate, spacing_ms, GATE_HTTP_MAX_AHEAD_MS);
    auto now = Clock::now();
    if (slot > now) {
        auto delta = slot - now;
        std::this_thread::sleep_for(delta);
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
    }
    return 0;
}

SteadyTp reserve_throttle_slot(Gate& gate, std::uint64_t cooldown_ms, std::uint64_t spacing_ms) {
    std::lock_guard<std::mutex> lk(gate.m);
    auto now = Clock::now();
    if (!gate.inited) {
        gate.next_allowed = now;
        gate.inited = true;
    }
    auto floor = now + ms(cooldown_ms);
    auto horizon = floor + std::chrono::seconds(60);
    auto base = std::clamp(gate.next_allowed, floor, horizon);
    gate.next_allowed = std::min(base + ms(spacing_ms), horizon);
    return base;
}

void pace_dashboard() { wait_global_gate(dashboard_gate(), DASHBOARD_STAGGER_MS); }
void pace_http_login() { wait_global_gate(http_login_gate(), HTTP_LOGIN_STAGGER_MS); }

}  // namespace adonai::bot
