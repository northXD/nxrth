// Adonai - geiger farming automation module (fleet-coordinated).
//
// Native C++ port of Mori's web-UI-generated geiger Lua farm (Mori-2.0.0
// AutomationsPage buildGeigerScript). The SEARCH is candidate-elimination /
// trilateration by concentric squared-distance rings, faithful to Mori's
// apply_observation - NOT a per-tile lawn-mower scan. The prize sits on one
// hidden tile; each geiger reading taken at a probe tile P with color C
// constrains the prize to an annulus (ring band) around P whose inner/outer
// squared radius is fixed by C. We keep a shrinking set of every tile the prize
// could still be on; each reading keeps only candidates whose squared L2
// distance to the reading tile falls inside C's band and deletes the rest.
// Successive rings intersect and the set collapses. The next probe is chosen by
// an info-gain heuristic (ported from Mori's score_probe). The hunt STOPS the
// moment area==Prize or the inventory diff shows the counter was consumed; then
// the bot waits out the post-prize recharge and a fresh hunt resets the grid.
//
// Around the search, the loop still: equips the Geiger Counter, warps to an
// assigned HUNT world (fleet-spread), deposits loot in a DEPOT world when the
// pack fills, picks up a fresh counter from a PICKUP world, and waits out the
// post-prize recharge. Bots coordinate through FleetState (claim the committed
// probe tile "geiger:WORLD:x,y" so no two chase one tile).
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bot/bot.h"          // adonai::bot::{AutomationModule, BotContext, GeigerArea}
#include "bot/bot_state.h"    // adonai::bot::GeigerArea / GeigerSignal
#include "bot/fleet_state.h"  // adonai::bot::FleetState
#include "world/inventory.h"  // adonai::world::Inventory

namespace adonai::automation {

// ---------------------------------------------------------------------------
// GeigerModule - coordinated geiger prize farming with candidate-elimination
// search.
//
// Config (AutomationConfig params, read each tick via config_snapshot):
//   enabled["geiger"]             = true  -> module runs (gated by run_automation)
//   params["geiger_hunt_worlds"]  = "WORLDA, WORLDB:door"  hunt/search worlds (fleet-spread)
//   params["geiger_depot_worlds"] = "DEPOT1, DEPOT2"       drop loot here when full
//   params["geiger_pickup_worlds"]= "PICK1"                grab a counter here when we have none
//   params["geiger_item"]         = "2204"                 charged Geiger Counter id
//   params["geiger_wear"]         = "1"                    auto-equip the counter
//   params["geiger_dig"]          = "1"                    also punch the tile (optional; the
//                                                          real mechanic is stand-on + collect)
//   params["geiger_recharge_min"] = "30"                   post-prize radioactive wait (minutes)
//   params["geiger_min_y"]        = "0"                    search grid min Y
//   params["geiger_max_y"]        = "53"                   search grid max Y
//   params["geiger_world_width"]  = "100"                  search grid width cap
//   params["geiger_signal_wait_ms"]= "4200"               wait for a fresh particle per probe
//   params["geiger_max_steps"]    = "70"                   probe budget per hunt
// ---------------------------------------------------------------------------
class GeigerModule : public adonai::bot::AutomationModule {
public:
    static constexpr const char* kName = "geiger";

    const char* name() const override { return kName; }
    void tick(adonai::bot::BotContext& self, adonai::bot::FleetState& fleet) override;

private:
    using Clock = std::chrono::steady_clock;
    using GeigerArea = adonai::bot::GeigerArea;

    // A tile the prize could be on / a probe location.
    struct Point {
        std::uint16_t x = 0;
        std::uint16_t y = 0;
    };
    // One geiger reading: color band at tile (x,y). area==nullopt encodes a
    // "nil" reading (no fresh particle -> prize is far, outside the red radius).
    struct Obs {
        std::optional<GeigerArea> area;
        std::uint16_t x = 0;
        std::uint16_t y = 0;
    };

    // ---- world/warp/claim plumbing (unchanged from the working farm loop) ----
    void release_claim(adonai::bot::FleetState& fleet);
    bool warp_towards(adonai::bot::BotContext& self, const std::string& cur,
                      const std::pair<std::string, std::string>& target);
    // Force auto-collect OFF around a depot deposit (so the bot doesn't re-vacuum
    // its own drops / everything on the depot ground) and restore the user's value
    // once hunting resumes. Idempotent.
    void suppress_collect(adonai::bot::BotContext& self);
    void restore_collect(adonai::bot::BotContext& self);
    // Blocking deposit at the CURRENT world: for each (id -> keep_count) in `plan`
    // drop the whole (held - keep) via the reliable 2-step drop_item, VERIFYING the
    // stack actually left the pack (inventory count fell) and hopping to a walkable
    // neighbour tile when a tile fills up (GT caps item drops per tile). Sleeps
    // between drops (keeps ENet serviced). Mirrors Mori's rotation.dropExcess loop.
    // Early-aborts if nothing drops across several tiles (a no-drop-access world:
    // stops the "Cant place tile" spam). Returns the number of stacks it dropped.
    std::uint32_t run_deposit(adonai::bot::BotContext& self,
                              const std::unordered_map<std::uint16_t, std::uint32_t>& plan);
    // Walk one tile to a walkable neighbour (direction rotated by `seq`) so drops
    // don't pile past the per-tile limit. false if no empty neighbour tile.
    bool hop_to_neighbour_tile(adonai::bot::BotContext& self, int seq);
    // Prize-only drop plan: every held geiger PRIZE id mapped to keep-count 0 (drop
    // all of it), never the counters or the account's own items.
    std::unordered_map<std::uint16_t, std::uint32_t> build_prize_plan(
        const adonai::world::Inventory& inv, std::uint16_t item) const;

    // ---- candidate-elimination search (ported from Mori) ---------------------
    void reset_hunt_state();
    void reset_candidates(adonai::bot::BotContext& self, int min_y, int max_y_cap, int width_cap);
    void fill_candidate_grid();  // rebuild candidates_ from the stored grid bounds
    void apply_observation(const Obs& obs, bool rebased = false);
    Point choose_probe(const std::optional<Obs>& focus);
    double score_probe(const Point& p, const Obs& focus) const;
    std::vector<Point> build_probe_points(const Point& anchor, GeigerArea sig) const;
    Point candidate_centroid() const;
    bool detect_prize_fallback(const adonai::world::Inventory& inv, std::uint16_t item) const;
    // Resolve the geiger PRIZE item ids (from a built-in name list via items.dat +
    // the optional "geiger_drop_ids" config param) so the deposit drops ONLY those,
    // never the account's other inventory. Built once when items.dat is available.
    void build_drop_ids(adonai::bot::BotContext& self, const adonai::bot::AutomationConfig& cfg);
    void on_prize(adonai::bot::BotContext& self, adonai::bot::FleetState& fleet, int recharge_min);
    // Build + send the fleet geiger webhook (Discord embed with fleet-wide totals
    // + this find's gained loot). Edits one shared message; no-op if url empty.
    void send_geiger_webhook(adonai::bot::BotContext& self, const std::string& url,
                             const std::string& username, const std::string& world,
                             const std::unordered_map<std::uint16_t, std::uint32_t>& gained,
                             std::uint32_t px, std::uint32_t py) const;

    // ---- persistent per-bot state (GeigerModule is instantiated per Bot) ------
    std::string world_name_;   // uppercased world we're currently in
    std::string claim_key_;    // the "geiger:WORLD:x,y" we own ("" = none)
    std::uint32_t bot_id_ = 0;

    Clock::time_point recharge_until_{};  // radioactive until this time (post-prize)
    Clock::time_point last_warp_{};       // debounce warp spam
    std::string last_warp_target_;        // world we last asked to warp to

    // search state (the whole point of the redesign)
    std::vector<Point> candidates_;                          // surviving prize tiles
    std::unordered_map<std::uint32_t, int> target_history_;  // (x<<16)|y -> probe count
    std::optional<Obs> last_observation_;                    // search focus/anchor
    std::uint64_t last_signal_ts_ = 0;                       // freshness baseline
    bool hunt_active_ = false;                               // hunt lifecycle latch
    int steps_this_hunt_ = 0;                                // per-hunt probe budget
    std::uint32_t base_charged_ = 0;                         // counter count at hunt start
    std::uint32_t base_dead_ = 0;                            // dead-counter count at hunt start
    std::unordered_map<std::uint16_t, adonai::world::InventoryItem> base_inv_;  // inv at hunt start
    bool pending_deposit_ = false;  // after a find: deposit loot before idling out the recharge
    bool collect_off_ = false;      // we forced auto-collect off for a deposit
    bool collect_saved_ = true;     // the user's auto-collect value to restore after
    int no_signal_probes_ = 0;      // consecutive probes with no geiger particle
    int counter_deposit_fails_ = 0;                 // consecutive zero-progress counter deposits
    Clock::time_point counter_deposit_off_until_{};  // skip the excess-counter deposit until this time
    std::unordered_set<std::uint16_t> drop_ids_;          // geiger PRIZE ids (the only ones to drop)
    bool drop_ids_built_ = false;
    std::string drop_ids_param_;  // last geiger_drop_ids value drop_ids_ was built from
    // grid bounds captured at reset_candidates (for probe clamping)
    int grid_min_y_ = 0;
    int grid_max_y_ = 53;
    int grid_width_ = 100;
};

}  // namespace adonai::automation
