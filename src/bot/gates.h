// Nxrth — process-global fleet stagger GATES (port specs 06 §1.1-§1.2 / §2.1,
// 07 §0.1/§0.3, 08 §3). These six mutex-guarded monotonic timestamps are the
// ONLY state shared across ALL bot threads and are how the fleet becomes "aware
// of each other" for login pacing: every bot reserves slots from the same cursor
// so a bulk spawn's HTTP logins / login packets / enter-game / warps / post-
// throttle reconnects are serialized rather than stampeding the shared exit IP.
//
// They MUST be process-global singletons (never per-bot). All timing is
// std::chrono::steady_clock (monotonic) — Rust used Instant, NOT wall clock.
//
// `wait_for_global_gate` (the connected-phase wait that keeps ENet serviced) is a
// Bot METHOD (see bot.h) — this header only declares the gate primitives.
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace nxrth::bot {

// --- Stagger / backoff constants (u64 ms unless noted) ----------------------
inline constexpr std::uint64_t LOGIN_PACKET_STAGGER_MS = 1000;   // login pkt after ServerHello
inline constexpr std::uint64_t ENTER_GAME_STAGGER_MS = 2000;     // action|enter_game
inline constexpr std::uint64_t HTTP_LOGIN_STAGGER_MS = 2500;     // whole HTTP login phase
inline constexpr std::uint64_t WARP_STAGGER_MS = 1200;           // world warps
inline constexpr std::uint64_t GATEWAY_THROTTLE_COOLDOWN_MS = 32000;  // "Fail to login 30s" floor
inline constexpr std::uint64_t GATEWAY_LOGON_SPACING_MS = 1500;  // fleet retry spacing post-throttle
inline constexpr std::uint64_t DASHBOARD_STAGGER_MS = 3500;      // growid dashboard POSTs

// Backlog ceilings. Connected-phase gates hold a LIVE ENet peer while waiting, so
// their horizon must stay well under the server login timeout; the HTTP gate dead-
// sleeps with no peer, so it can preserve true spacing for large fleets.
inline constexpr std::uint64_t GATE_CONNECTED_MAX_AHEAD_MS = 2500;
inline constexpr std::uint64_t GATE_HTTP_MAX_AHEAD_MS = 300000;

// --- Gate primitive: a lazily-initialized, mutex-guarded "next allowed slot" --
struct Gate {
    std::mutex m;
    std::chrono::steady_clock::time_point next_allowed{};
    bool inited = false;
};

// The six process-global gates (function-local statics; thread-safe C++11 init).
Gate& login_packet_gate();
Gate& enter_game_gate();
Gate& http_login_gate();
Gate& warp_gate();
Gate& gateway_logon_gate();  // reconnect scheduler after a gateway throttle
Gate& dashboard_gate();

using SteadyTp = std::chrono::steady_clock::time_point;

// Atomically reserve THIS bot's slot and return the instant it may proceed. The
// lock is held ONLY for the reservation, never during the wait. next_allowed is
// monotonic; `max_ahead_ms` caps how far ahead a slot may be handed out AND pins
// next_allowed at that horizon, so the backlog can never exceed max_ahead_ms:
//   now=Instant::now; horizon=now+max_ahead; slot=clamp(next_allowed,now,horizon);
//   next_allowed=min(slot+spacing,horizon); return slot.
SteadyTp reserve_gate_slot(Gate& gate, std::uint64_t spacing_ms, std::uint64_t max_ahead_ms);

// DEAD-SLEEP pre-connection variant (no live peer): reserve with
// GATE_HTTP_MAX_AHEAD_MS; if slot>now sleep (slot-now) and return waited ms, else 0.
std::uint64_t wait_global_gate(Gate& gate, std::uint64_t spacing_ms);

// Reserve a fleet reconnect slot after a gateway throttle:
//   floor=now+cooldown; horizon=floor+60s; base=clamp(next_allowed,floor,horizon);
//   next_allowed=min(base+spacing,horizon); return base.
SteadyTp reserve_throttle_slot(Gate& gate, std::uint64_t cooldown_ms, std::uint64_t spacing_ms);

// Convenience wrappers (call from any module right before the paced HTTP action).
// NOTE: these are the authoritative fleet gates. If the login module keeps its own
// nxrth::login::pace_dashboard / pace_http_login, those should forward here so the
// whole fleet shares ONE cursor per phase (two gates = broken pacing).
void pace_dashboard();    // wait_global_gate(dashboard_gate(), DASHBOARD_STAGGER_MS)
void pace_http_login();   // wait_global_gate(http_login_gate(), HTTP_LOGIN_STAGGER_MS)

}  // namespace nxrth::bot
