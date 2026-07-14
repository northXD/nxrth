#include "automation/modules/geiger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "automation/geiger_stats.h"  // fleet-wide aggregate + webhook message store
#include "net/http_client.h"          // Discord webhook POST/PATCH
#include "world/inventory.h"          // adonai::world::{Inventory, TEMPORARY_ITEM_IDS}
#include "world/items.h"              // adonai::world::ItemsDat::find_by_id (loot names)
#include "world/world.h"              // adonai::world::{World, pixel_to_tile} (deposit tile-hop)

namespace adonai::automation {
namespace {

using adonai::bot::GeigerArea;

constexpr std::uint16_t kDeadGeigerId = 2286;  // "dead"/uncharged counter

// The complete set of geiger prize items (GT wiki reward list). The deposit drops
// ONLY these - never the account's own inventory. Resolved to item ids at runtime
// via items.dat find_by_name (robust to items.dat version); anything not found is
// simply skipped. Extend/override with the "geiger_drop_ids" config param.
const char* const kGeigerPrizeNames[] = {
    // normal rewards
    "Star Fuel", "Nuclear Fuel", "Radioactive Chemical",
    "Orange Stuff", "Purple Stuff", "Blue Stuff", "Green Stuff", "Red Stuff", "White Stuff",
    "Black Stuff",
    "Green Crystal", "Blue Crystal", "Red Crystal", "White Crystal", "Black Crystal",
    "Glowstick", "D Battery", "Geiger Charger", "Nerd Hair",
    // geiger day limited
    "Green Geiger V-Neck", "Blue Geiger V-Neck", "Purple Geiger V-Neck", "Orange Geiger V-Neck",
    "Hazmat Helmet", "Hazmat Suit", "Hazmat Pants", "Hazmat Boots", "Digital Sign",
    "Anime Female Hair", "Anime Male Hair",
    // event limited
    "Ruby Shard", "Tree Decorations", "Rave Haze Light", "Retro Record Block", "Shamrock Lights",
    "Neon Wings", "Cosmic Force", "Sakura's Revenge", "Shattered Spear", "UbiToken",
    // geigerhills
    "Uranium Block",
    // Mori TRACKED_ROWS extras
    "Easter Egg - Rainbow", "Golden Egg Shard - Middle",
};

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

int atoi_or(const std::string& s, int fallback) {
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

// Parse a "WORLDA, WORLDB:door\nWORLDC" list into (upper world, door) pairs.
std::vector<std::pair<std::string, std::string>> parse_worlds(const std::string& csv) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string cur;
    auto flush = [&]() {
        const auto b = cur.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) {
            cur.clear();
            return;
        }
        const auto e = cur.find_last_not_of(" \t\r\n");
        std::string entry = cur.substr(b, e - b + 1);
        cur.clear();
        std::string name = entry, door;
        const auto colon = entry.find(':');
        if (colon != std::string::npos) {
            name = entry.substr(0, colon);
            door = entry.substr(colon + 1);
        }
        name = upper(name);
        if (!name.empty()) out.emplace_back(std::move(name), std::move(door));
    };
    for (char c : csv) {
        if (c == ',' || c == '\n' || c == ';')
            flush();
        else
            cur.push_back(c);
    }
    flush();
    return out;
}

// ---------------------------------------------------------------------------
// Distance-band model (ported verbatim from Mori's `ranges`, squared tiles).
// Each geiger color = an annulus around the reading tile: the prize is that far.
//   Red 15-30 tiles -> 225..900 | Yellow 7.5-15 -> 56.25..225
//   Green 4-7.5      -> 16..56.25 | RapidGreen 0-4 -> 0..16
// ---------------------------------------------------------------------------
struct Band {
    double min2;
    double max2;
};

Band area_band(GeigerArea a) {
    switch (a) {
        case GeigerArea::Red:        return {225.0, 900.0};
        case GeigerArea::Yellow:     return {56.25, 225.0};
        case GeigerArea::Green:      return {16.0, 56.25};
        case GeigerArea::RapidGreen: return {0.0, 16.0};
        default:                     return {0.0, 16.0};  // Prize handled by caller
    }
}

// Simulated color a probe would read if the prize were at a given squared
// distance (used by score_probe to bucket candidates). Same cutoffs as `ranges`.
enum class SimBand { Nil, Red, Yellow, Green, Rapid };
SimBand classify_dist2(double d2) {
    if (d2 <= 16.0) return SimBand::Rapid;
    if (d2 <= 56.25) return SimBand::Green;
    if (d2 <= 225.0) return SimBand::Yellow;
    if (d2 <= 900.0) return SimBand::Red;
    return SimBand::Nil;
}

inline double dist2i(int px, int py, int ox, int oy) {
    const double dx = static_cast<double>(px) - static_cast<double>(ox);
    const double dy = static_cast<double>(py) - static_cast<double>(oy);
    return dx * dx + dy * dy;
}

std::uint32_t inv_amount(const adonai::world::Inventory& inv, std::uint16_t id) {
    auto it = inv.items.find(id);
    return it != inv.items.end() ? static_cast<std::uint32_t>(it->second.amount) : 0u;
}

}  // namespace

// ---------------------------------------------------------------------------
// world / warp / claim plumbing (unchanged working farm-loop machinery)
// ---------------------------------------------------------------------------
void GeigerModule::release_claim(adonai::bot::FleetState& fleet) {
    if (!claim_key_.empty()) {
        fleet.release(claim_key_, bot_id_);
        claim_key_.clear();
    }
}

bool GeigerModule::warp_towards(adonai::bot::BotContext& self, const std::string& cur,
                                const std::pair<std::string, std::string>& target) {
    if (cur == target.first) return false;  // already in the target world
    const auto now = Clock::now();
    // Debounce: don't re-issue the same warp every 10 ms tick while it's in flight.
    if (target.first != last_warp_target_ || now - last_warp_ > std::chrono::seconds(10)) {
        self.warp(target.first, target.second);
        last_warp_ = now;
        last_warp_target_ = target.first;
    }
    return true;  // not there yet -> caller should wait
}

// ---------------------------------------------------------------------------
// candidate-elimination search (port of Mori reset_candidates / apply_observation
// / choose_probe / score_probe / build_probe_points)
// ---------------------------------------------------------------------------
void GeigerModule::suppress_collect(adonai::bot::BotContext& self) {
    if (!collect_off_) {
        collect_saved_ = self.auto_collect();
        self.set_auto_collect(false);
        collect_off_ = true;
    }
}

void GeigerModule::restore_collect(adonai::bot::BotContext& self) {
    if (collect_off_) {
        self.set_auto_collect(collect_saved_);
        collect_off_ = false;
    }
}

std::unordered_map<std::uint16_t, std::uint32_t> GeigerModule::build_prize_plan(
    const adonai::world::Inventory& inv, std::uint16_t item) const {
    // Every held geiger PRIZE id -> keep 0 (drop all of it). Never the counters,
    // never the account's own items. If the prize set couldn't be resolved
    // (items.dat missing) fall back to "all non-temporary loot" so deposits work.
    std::unordered_map<std::uint16_t, std::uint32_t> plan;
    for (const auto& [id, it] : inv.items) {
        if (id == item || id == kDeadGeigerId || it.amount == 0) continue;
        if (!drop_ids_.empty()) {
            if (!drop_ids_.count(id)) continue;
        } else {
            bool temp = false;
            for (auto t : adonai::world::TEMPORARY_ITEM_IDS)
                if (t == id) temp = true;
            if (temp) continue;
        }
        plan[id] = 0;
    }
    return plan;
}

bool GeigerModule::hop_to_neighbour_tile(adonai::bot::BotContext& self, int seq) {
    const auto& w = self.world();  // std::optional<World>&
    if (!w) return false;
    const int W = static_cast<int>(w->tile_map.width);
    const int H = static_cast<int>(w->tile_map.height);
    const int cx = static_cast<int>(adonai::world::pixel_to_tile(self.pos_x()));
    const int cy = static_cast<int>(adonai::world::pixel_to_tile(self.pos_y()));
    // 8 neighbours; rotate the starting direction each hop so drops fan out over
    // several tiles ("random sag/sol/ust" spread) instead of piling on one.
    static const int dirs[8][2] = {{1, 0},  {-1, 0}, {0, 1},  {0, -1},
                                   {1, 1},  {-1, 1}, {1, -1}, {-1, -1}};
    for (int k = 0; k < 8; ++k) {
        const int d = (((seq % 8) + 8) % 8 + k) % 8;
        const int nx = cx + dirs[d][0];
        const int ny = cy + dirs[d][1];
        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
        const auto* t = w->get_tile(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny));
        if (t && t->fg_item_id == 0) {  // empty foreground = walkable
            self.find_path(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny));
            return true;
        }
    }
    return false;
}

std::uint32_t GeigerModule::run_deposit(
    adonai::bot::BotContext& self,
    const std::unordered_map<std::uint16_t, std::uint32_t>& plan) {
    if (plan.empty()) return 0;
    constexpr std::uint64_t kDropPauseMs = 800;  // dialog + inventory-update settle
    constexpr int kMaxDeadTiles = 4;  // give up if NOTHING drops across this many tiles
    int hop_seq = static_cast<int>(bot_id_);      // vary the first hop direction per bot
    std::uint32_t dropped_stacks = 0;
    int dead_tiles = 0;  // consecutive failed drops (across hops) with no progress at all
    for (const auto& [id, keep] : plan) {
        int stuck = 0;
        // Drop this id down to `keep`, VERIFYING each drop landed (the pack count
        // fell). If it didn't, the tile is full (GT caps drops per tile) -> walk to
        // a neighbouring tile and retry; bail on this id after a few dead tiles.
        while (self.in_world()) {
            const std::uint32_t have = inv_amount(self.inventory(), id);
            if (have <= keep) break;  // fully deposited
            const std::uint32_t before = have;
            self.drop_item(id, have - keep);  // whole excess in one 2-step drop
            self.idle(kDropPauseMs);
            const std::uint32_t after = inv_amount(self.inventory(), id);
            if (after >= before) {  // nothing left the pack -> tile full / drop refused
                // If we've never dropped ANYTHING and several tiles all refuse, this
                // is a NO-DROP world (locked / no build access): bail immediately so
                // we don't spam "Cant place tile" and get kicked to the white door.
                if (dropped_stacks == 0 && ++dead_tiles >= kMaxDeadTiles) return 0;
                if (++stuck > 6) break;
                if (!hop_to_neighbour_tile(self, hop_seq++)) break;  // nowhere to hop
                self.idle(200);
            } else {
                ++dropped_stacks;
                stuck = 0;      // progress -> keep clearing the remainder (may need a new pile)
                dead_tiles = 0;
            }
        }
    }
    return dropped_stacks;
}

void GeigerModule::reset_hunt_state() {
    hunt_active_ = false;
    candidates_.clear();
    target_history_.clear();
    last_observation_.reset();
    steps_this_hunt_ = 0;
    // NOTE: last_signal_ts_ is deliberately NOT reset here. It is the fleet-wide
    // "last reading we already consumed" watermark; keeping it across hunts stops
    // a stale Prize particle (which lingers in shared state until a world reload)
    // from re-triggering a find at the next hunt start.
}

void GeigerModule::fill_candidate_grid() {
    candidates_.clear();
    if (grid_width_ < 1 || grid_max_y_ < grid_min_y_) return;
    candidates_.reserve(static_cast<std::size_t>(grid_width_) *
                        static_cast<std::size_t>(grid_max_y_ - grid_min_y_ + 1));
    for (int x = 0; x < grid_width_; ++x)
        for (int y = grid_min_y_; y <= grid_max_y_; ++y)
            candidates_.push_back(
                {static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y)});
}

void GeigerModule::reset_candidates(adonai::bot::BotContext& self, int min_y, int max_y_cap,
                                    int width_cap) {
    // Bound the grid by the REAL current-world tile map (fixes the "config-only
    // bounds" gap); fall back to config caps if the world isn't loaded yet.
    int W = width_cap;
    int H = max_y_cap + 1;
    if (self.world()) {
        W = static_cast<int>(self.world()->tile_map.width);
        H = static_cast<int>(self.world()->tile_map.height);
    }
    grid_width_ = std::min(W, width_cap);
    if (grid_width_ < 1) grid_width_ = 1;
    grid_min_y_ = std::max(0, min_y);
    grid_max_y_ = std::min(H - 1, max_y_cap);
    if (grid_max_y_ < grid_min_y_) grid_max_y_ = grid_min_y_;
    fill_candidate_grid();
}

void GeigerModule::apply_observation(const Obs& obs, bool rebased) {
    std::vector<Point> kept;
    kept.reserve(candidates_.size());
    if (!obs.area.has_value()) {
        // "nil": no particle within the wait window -> prize is beyond the red
        // radius (squared dist > 900). Keep only far candidates.
        for (const auto& p : candidates_)
            if (dist2i(p.x, p.y, obs.x, obs.y) > 900.0) kept.push_back(p);
    } else {
        const Band b = area_band(*obs.area);
        for (const auto& p : candidates_) {
            const double d = dist2i(p.x, p.y, obs.x, obs.y);
            if (d >= b.min2 && d <= b.max2) kept.push_back(p);
        }
    }
    if (kept.empty()) {
        // Contradiction with the surviving set. Rebuild the full grid ONCE and
        // re-filter (a stale/noisy prior reading may have over-pruned); if it is
        // still empty, keep this obs as the focus but do NOT wipe the set.
        if (!rebased) {
            fill_candidate_grid();
            apply_observation(obs, true);
            return;
        }
        last_observation_ = obs;
        return;
    }
    candidates_.swap(kept);
    last_observation_ = obs;
}

GeigerModule::Point GeigerModule::candidate_centroid() const {
    if (candidates_.empty()) return {0, 0};
    std::uint64_t sx = 0, sy = 0;
    for (const auto& p : candidates_) {
        sx += p.x;
        sy += p.y;
    }
    return {static_cast<std::uint16_t>(sx / candidates_.size()),
            static_cast<std::uint16_t>(sy / candidates_.size())};
}

std::vector<GeigerModule::Point> GeigerModule::build_probe_points(const Point& anchor,
                                                                  GeigerArea sig) const {
    int r = 30, step = 8;
    switch (sig) {
        case GeigerArea::Red:        r = 30; step = 8; break;
        case GeigerArea::Yellow:     r = 15; step = 4; break;
        case GeigerArea::Green:      r = 8;  step = 2; break;
        case GeigerArea::RapidGreen: r = 4;  step = 1; break;
        default:                     r = 30; step = 8; break;
    }
    std::vector<Point> pts;
    std::unordered_map<std::uint32_t, char> seen;
    auto add = [&](int x, int y) {
        if (x < 0) x = 0;
        if (x >= grid_width_) x = grid_width_ - 1;
        if (y < grid_min_y_) y = grid_min_y_;
        if (y > grid_max_y_) y = grid_max_y_;
        const Point p{static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y)};
        const std::uint32_t k = (static_cast<std::uint32_t>(p.x) << 16) | p.y;
        if (seen.emplace(k, 1).second) pts.push_back(p);
    };
    const int ax = anchor.x, ay = anchor.y;
    const int r2 = r * r;
    for (int dy = -r; dy <= r; dy += step)
        for (int dx = -r; dx <= r; dx += step)
            if (dx * dx + dy * dy <= r2) add(ax + dx, ay + dy);
    // Endgame: once the set is small, also probe each survivor and its neighbours
    // so we can walk onto the exact tile.
    if (candidates_.size() <= 24) {
        for (const auto& c : candidates_) {
            add(c.x, c.y);
            add(c.x + 1, c.y);
            add(c.x - 1, c.y);
            add(c.x, c.y + 1);
            add(c.x, c.y - 1);
        }
    }
    if (pts.empty()) pts.push_back(anchor);
    return pts;
}

double GeigerModule::score_probe(const Point& p, const Obs& focus) const {
    // Simulate: if the prize were at each surviving tile, what color would a
    // reading taken at `p` show? Bucket the survivors; a probe that splits them
    // evenly (small largest bucket) yields the most information.
    double nil = 0, red = 0, yellow = 0, green = 0, rapid = 0;
    for (const auto& src : candidates_) {
        switch (classify_dist2(dist2i(p.x, p.y, src.x, src.y))) {
            case SimBand::Rapid:  rapid += 1; break;
            case SimBand::Green:  green += 1; break;
            case SimBand::Yellow: yellow += 1; break;
            case SimBand::Red:    red += 1; break;
            default:              nil += 1; break;
        }
    }
    const double total = std::max<double>(1.0, static_cast<double>(candidates_.size()));
    const double biggest = std::max({nil, red, yellow, green, rapid});
    double close_bonus = (rapid * 0.65 + green * 0.35 + yellow * 0.08) / total;
    double bad_penalty = (nil * 0.30 + red * 0.16) / total;
    if (focus.area.has_value()) {
        if (*focus.area == GeigerArea::Red) {
            bad_penalty = nil * 0.35 / total;
        } else if (*focus.area == GeigerArea::RapidGreen) {
            close_bonus = (rapid * 0.9 + green * 0.15) / total;
            bad_penalty = (yellow * 0.20 + red * 0.45 + nil * 0.65) / total;
        }
    }
    const std::uint32_t k = (static_cast<std::uint32_t>(p.x) << 16) | p.y;
    auto it = target_history_.find(k);
    const double repeat = (it != target_history_.end() ? it->second : 0) * 0.18;
    const double move = std::sqrt(dist2i(p.x, p.y, focus.x, focus.y)) * 0.012;
    return biggest / total + bad_penalty - close_bonus + repeat + move;
}

GeigerModule::Point GeigerModule::choose_probe(const std::optional<Obs>& focus) {
    if (candidates_.empty()) return {0, 0};
    Point anchor;
    GeigerArea sig = GeigerArea::Red;
    Obs focus_obs;
    if (focus.has_value() && focus->area.has_value()) {
        anchor = {focus->x, focus->y};
        sig = *focus->area;
        focus_obs = *focus;
    } else {
        // No color focus: search wide at Red scale (disc radius/step), but score
        // with the DEFAULT weights (focus.area == nullopt) exactly like Mori's
        // score_probe(pt, anchor) - the Red-branch penalty override is only for a
        // real Red reading, not the initial centroid sweep.
        anchor = candidate_centroid();
        sig = GeigerArea::Red;
        focus_obs = Obs{std::nullopt, anchor.x, anchor.y};
    }
    const auto probes = build_probe_points(anchor, sig);
    Point best = probes.front();
    double best_score = std::numeric_limits<double>::infinity();
    for (const auto& pt : probes) {
        const double s = score_probe(pt, focus_obs);
        if (s < best_score) {
            best_score = s;
            best = pt;
        }
    }
    const std::uint32_t k = (static_cast<std::uint32_t>(best.x) << 16) | best.y;
    target_history_[k] += 1;
    return best;
}

bool GeigerModule::detect_prize_fallback(const adonai::world::Inventory& inv,
                                         std::uint16_t item) const {
    // Signal-independent: the charged counter was consumed (count dropped) or a
    // dead counter appeared (count rose) -> we just found a prize.
    return inv_amount(inv, item) < base_charged_ || inv_amount(inv, kDeadGeigerId) > base_dead_;
}

void GeigerModule::build_drop_ids(adonai::bot::BotContext& self,
                                  const adonai::bot::AutomationConfig& cfg) {
    drop_ids_.clear();
    if (const auto* items = self.items_dat()) {
        for (const char* nm : kGeigerPrizeNames)
            if (const auto* info = items->find_by_name(nm))
                drop_ids_.insert(static_cast<std::uint16_t>(info->id));
    }
    // explicit extra / override ids from config ("geiger_drop_ids": "2242, 2244 2246")
    const std::string ids = cfg.param("geiger_drop_ids", "");
    std::string tok;
    auto flush = [&]() {
        if (!tok.empty()) {
            try {
                drop_ids_.insert(static_cast<std::uint16_t>(std::stoul(tok)));
            } catch (...) {
            }
            tok.clear();
        }
    };
    for (char c : ids) {
        if (std::isdigit(static_cast<unsigned char>(c)))
            tok.push_back(c);
        else
            flush();
    }
    flush();
}

void GeigerModule::send_geiger_webhook(
    adonai::bot::BotContext& self, const std::string& url, const std::string& username,
    const std::string& world, const std::unordered_map<std::uint16_t, std::uint32_t>& gained,
    std::uint32_t px, std::uint32_t py) const {
    const GeigerAgg agg = geiger_stats_snapshot();
    const auto* items = self.items_dat();
    auto name_of = [&](std::uint16_t id) -> std::string {
        if (items)
            if (const auto* info = items->find_by_id(id))
                if (!info->name.empty()) return info->name;
        return "item " + std::to_string(id);
    };

    // ---- fleet-wide totals + this find's loot -> Discord embed description ----
    std::string desc = "**Geiger farm - fleet totals**\n";
    desc += "Total prizes found: **" + std::to_string(agg.total_finds) + "x**\n\n";
    std::vector<std::pair<std::uint16_t, std::uint64_t>> rows(agg.counts.begin(),
                                                              agg.counts.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::size_t shown = 0;
    for (const auto& [id, c] : rows) {
        if (shown++ >= 30) {
            desc += "- ...\n";
            break;
        }
        desc += "- " + std::to_string(c) + "x " + name_of(id) + "\n";
    }
    desc += "\n**Last prize** by " + (username.empty() ? std::string("bot") : username) + " @ " +
            (world.empty() ? std::string("?") : world) + " " + std::to_string(px) + ":" +
            std::to_string(py) + "\n";
    if (gained.empty()) {
        desc += "- (no tracked loot in the inventory diff)\n";
    } else {
        for (const auto& [id, amt] : gained)
            desc += "- " + std::to_string(amt) + "x " + name_of(id) + "\n";
    }
    if (desc.size() > 4000) desc.resize(4000);  // Discord embed description cap 4096

    nlohmann::json embed;
    embed["title"] = "Geiger Logs";
    embed["description"] = desc;
    embed["color"] = 65280;  // green, matches Mori
    nlohmann::json payload;
    payload["username"] = "Adonai Geiger";
    payload["content"] = "";
    payload["embeds"] = nlohmann::json::array({embed});
    const std::string body = payload.dump();

    adonai::net::HttpClient client;
    adonai::net::HttpRequest opts;
    opts.headers.push_back({"Content-Type", "application/json"});
    opts.timeout_secs = 8;

    // Edit the ONE shared fleet message (keyed by URL) if we know its id, else
    // POST a fresh one with ?wait=true and remember the id (no per-find spam).
    const std::string msg_id = geiger_webhook_message_id(url);
    if (!msg_id.empty()) {
        adonai::net::HttpRequest patch = opts;
        patch.url = url + "/messages/" + msg_id;
        patch.body = body;
        patch.custom_method = "PATCH";
        const auto resp = client.Request(patch);
        if (resp.status != 404) return;               // edited (or transient error) -> done
        clear_geiger_webhook_message_id(url);         // message gone -> fall through to re-post
    }
    const std::string post_url = url + (url.find('?') == std::string::npos ? "?wait=true"
                                                                           : "&wait=true");
    const auto resp = client.Post(post_url, body, opts);
    if (resp.ok() && resp.status >= 200 && resp.status < 300) {
        try {
            const auto j = nlohmann::json::parse(resp.body);
            if (j.contains("id") && j["id"].is_string())
                set_geiger_webhook_message_id(url, j["id"].get<std::string>());
        } catch (...) {
        }
    }
}

void GeigerModule::on_prize(adonai::bot::BotContext& self, adonai::bot::FleetState& fleet,
                           int recharge_min) {
    const auto cfg = fleet.config_snapshot();
    const std::uint16_t item =
        static_cast<std::uint16_t>(atoi_or(cfg.param("geiger_item", "2204"), 2204));

    // 1. gained loot = positive inventory deltas over the hunt, minus the counters
    //    and temporary items (World Key / Magplant).
    std::unordered_map<std::uint16_t, std::uint32_t> gained;
    for (const auto& [id, it] : self.inventory().items) {
        if (id == item || id == kDeadGeigerId) continue;
        bool temp = false;
        for (auto t : adonai::world::TEMPORARY_ITEM_IDS)
            if (t == id) temp = true;
        if (temp) continue;
        std::uint32_t before = 0;
        if (auto b = base_inv_.find(id); b != base_inv_.end()) before = b->second.amount;
        if (it.amount > before) gained[id] = static_cast<std::uint32_t>(it.amount - before);
    }

    // 2. fleet-wide aggregate (persisted) + the shared Discord webhook.
    geiger_record_find(gained);
    if (const std::string url = cfg.param("geiger_webhook_url", ""); !url.empty()) {
        std::uint32_t px = 0, py = 0;
        if (auto s = self.geiger_signal()) {
            px = s->x;
            py = s->y;
        }
        std::string uname;
        if (auto v = fleet.get(bot_id_)) uname = v->username;
        send_geiger_webhook(self, url, uname, world_name_, gained, px, py);
    }

    // 3. mark the current reading consumed (stops a stale Prize particle from
    //    re-triggering a find), arm the recharge wait, and schedule the loot
    //    deposit so the bot doesn't just AFK-freeze holding the prize.
    if (auto s = self.geiger_signal())
        last_signal_ts_ = std::max<std::uint64_t>(last_signal_ts_, s->timestamp_ms);
    release_claim(fleet);
    recharge_until_ = Clock::now() + std::chrono::minutes(recharge_min > 0 ? recharge_min : 30);
    pending_deposit_ = true;
    reset_hunt_state();  // hard, single-shot terminal transition: this hunt is over
}

// ---------------------------------------------------------------------------
// tick: recharge gate -> ensure counter -> deposit -> travel -> ELIMINATION SEARCH
// ---------------------------------------------------------------------------
void GeigerModule::tick(adonai::bot::BotContext& self, adonai::bot::FleetState& fleet) {
    bot_id_ = self.bot_id();

    // Read the current world FIRST. At the white door (world-select) the engine
    // reports world_name "EXIT" but clears world_ (so in_world() is false) - we
    // must still act there, to re-warp OUT of it.
    std::string world;
    if (auto v = fleet.get(bot_id_)) world = upper(v->world_name);
    const bool at_white_door = (world == "EXIT");

    // Truly out of any world (disconnected / mid-login) and NOT at a re-warpable
    // white door -> idle.
    if (!self.in_world() && !at_white_door) {
        release_claim(fleet);
        world_name_.clear();
        reset_hunt_state();
        return;
    }

    if (world != world_name_) {
        release_claim(fleet);
        world_name_ = world;
        reset_hunt_state();  // never search against candidates from a stale world
    }
    // White-door recovery: a warp that dropped + re-logged to the gateway lands us
    // here. Clear the warp debounce so the sections below re-warp to our objective
    // (hunt/depot/pickup) IMMEDIATELY instead of sitting at the white door - this is
    // how Mori recovers from a failed warp.
    if (at_white_door) last_warp_target_.clear();

    // ---- config (fleet-wide, live) ----
    const auto cfg = fleet.config_snapshot();
    const auto hunt = parse_worlds(cfg.param("geiger_hunt_worlds", ""));
    const auto depot = parse_worlds(cfg.param("geiger_depot_worlds", ""));
    const auto pickup = parse_worlds(cfg.param("geiger_pickup_worlds", ""));
    const std::uint16_t item =
        static_cast<std::uint16_t>(atoi_or(cfg.param("geiger_item", "2204"), 2204));
    const bool wear = cfg.param("geiger_wear", "1") != "0";
    const bool dig = cfg.param("geiger_dig", "1") != "0";
    const int recharge_min = atoi_or(cfg.param("geiger_recharge_min", "30"), 30);
    const int min_y = std::max(0, atoi_or(cfg.param("geiger_min_y", "0"), 0));
    const int max_y_cap = std::max(min_y + 1, atoi_or(cfg.param("geiger_max_y", "53"), 53));
    const int width_cap = std::max(4, atoi_or(cfg.param("geiger_world_width", "100"), 100));
    const std::uint64_t wait_ms = static_cast<std::uint64_t>(
        std::max(500, atoi_or(cfg.param("geiger_signal_wait_ms", "4200"), 4200)));
    const int max_steps = std::max(1, atoi_or(cfg.param("geiger_max_steps", "70"), 70));

    // Resolve the geiger PRIZE ids (deposit drops ONLY these, not the account).
    // Rebuild when items.dat first loads or the geiger_drop_ids override changes.
    if (const std::string dp = cfg.param("geiger_drop_ids", "");
        self.items_dat() && (!drop_ids_built_ || dp != drop_ids_param_)) {
        build_drop_ids(self, cfg);
        drop_ids_param_ = dp;
        drop_ids_built_ = true;
    }

    const auto& inv = self.inventory();

    // ---- 1. post-prize radioactive recharge wait (the HARD "stop after a find") ----
    if (Clock::now() < recharge_until_) {
        // Don't just freeze in place holding the prize: first go DEPOSIT the found
        // loot at a depot world (like Mori's after_prize depot_drop).
        if (pending_deposit_ && !depot.empty() && inv.size > 4) {
            suppress_collect(self);  // don't re-vacuum our own drops at the depot
            const auto tgt = depot[bot_id_ % depot.size()];
            if (warp_towards(self, world, tgt)) return;  // multi-tick travel
            release_claim(fleet);
            run_deposit(self, build_prize_plan(inv, item));  // drop ALL prizes (blocking)
            pending_deposit_ = false;  // all dropped -> head home, don't AFK at the depot
            // fall through: warp back to the assigned hunt world below.
        }
        release_claim(fleet);
        // Idle out the rest of the recharge IN THE ASSIGNED HUNT WORLD (Mori returns
        // to its own world after dropping), NOT standing AFK at the depot. The spent
        // counter is now a dead counter (2286) that recharges in-game while we idle
        // here; once it's charged again, section 2 resumes the hunt automatically.
        // (If loot is still pending but no depot was configured, just idle in place.)
        if (!pending_deposit_ && !hunt.empty()) {
            const auto tgt = hunt[bot_id_ % hunt.size()];
            warp_towards(self, world, tgt);  // go home; the counter recharges here
        }
        return;
    }
    pending_deposit_ = false;  // recharge elapsed; nothing left to deposit-then-idle

    // ---- 2. ensure we hold a CHARGED Geiger Counter ----
    if (!inv.has_item(item, 1)) {
        reset_hunt_state();
        // A DEAD counter (2286) recharges in-game over time -> WAIT for it in the
        // hunt world; do NOT go fetch another from the pickup depot (this is Mori's
        // wait_ready_geiger). Only fetch when we hold NO counter at all.
        if (inv.has_item(kDeadGeigerId, 1)) {
            if (!hunt.empty()) {
                const auto tgt = hunt[bot_id_ % hunt.size()];
                warp_towards(self, world, tgt);  // idle in the hunt world while it recharges
            }
            return;
        }
        if (!pickup.empty()) {
            const auto tgt = pickup[bot_id_ % pickup.size()];
            if (warp_towards(self, world, tgt)) return;
            self.collect();  // pick up a counter someone stocked on the ground
            return;
        }
        return;  // no counter anywhere + no pickup world -> can't hunt; idle
    }
    if (wear && !self.is_item_equipped(item)) self.wear(item);

    // ---- 2b. deposit EXCESS geiger counters --------------------------------------
    // A bot should hold exactly ONE counter. Keep the DEAD one if we have it (it is
    // recharging in-game); otherwise keep one charged counter. Everything else is
    // dropped. Mirrors Mori's rotation.dropExcess (e.g. 11 charged + 1 dead -> drop
    // all 11 charged). Deposit at the DEPOT world (where prizes already drop = the
    // bot has build access there); the PICKUP world is often locked (geiger-alinacak)
    // so dropping there fails ("Cant place tile" spam -> kick to the white door).
    // Only run in a REAL world (never at the white door -> section 4 warps us back
    // to hunt = the recovery). If depositing keeps failing, back off 5 min and hunt
    // with the extras rather than looping forever.
    {
        const std::uint32_t plain_cnt = inv_amount(inv, item);
        const std::uint32_t dead_cnt = inv_amount(inv, kDeadGeigerId);
        const auto& drop_worlds = !depot.empty() ? depot : pickup;
        if (self.in_world() && !drop_worlds.empty() && plain_cnt + dead_cnt > 1 &&
            Clock::now() >= counter_deposit_off_until_) {
            reset_hunt_state();
            suppress_collect(self);  // don't re-vacuum the counters we just dropped
            const auto tgt = drop_worlds[bot_id_ % drop_worlds.size()];
            if (warp_towards(self, world, tgt)) return;  // multi-tick travel
            release_claim(fleet);
            std::unordered_map<std::uint16_t, std::uint32_t> plan;
            if (dead_cnt >= 1) {
                plan[kDeadGeigerId] = 1;              // keep one dead counter (recharging)
                if (plain_cnt > 0) plan[item] = 0;    // drop ALL charged counters
            } else {
                plan[item] = 1;                       // no dead -> keep one charged
            }
            const std::uint32_t dropped = run_deposit(self, plan);
            restore_collect(self);
            if (dropped == 0) {
                // Couldn't drop here (no build access / world unreachable). Don't loop
                // forever: after a few dead episodes, skip for 5 min and just hunt.
                if (++counter_deposit_fails_ >= 3) {
                    counter_deposit_fails_ = 0;
                    counter_deposit_off_until_ = Clock::now() + std::chrono::minutes(5);
                    self.log("[geiger] excess-counter deposit failed (no drop access at the "
                             "depot?) - skipping 5 min, hunting with the extra counters");
                }
            } else {
                counter_deposit_fails_ = 0;
            }
            return;  // re-evaluate next tick (now holding exactly one counter)
        }
    }

    // Do we hold any PRIZE item worth a deposit run? (Prevents a full pack of
    // non-prize account items from looping the deposit forever.)
    bool have_prizes = false;
    for (const auto& [id, it] : inv.items) {
        if (id == item || id == kDeadGeigerId || it.amount == 0) continue;
        if (drop_ids_.empty() || drop_ids_.count(id)) {
            have_prizes = true;
            break;
        }
    }

    // ---- 3. deposit prize loot when the pack is (nearly) full ----
    if (!depot.empty() && have_prizes && inv.size > 4 &&
        static_cast<int>(inv.item_count) + 2 >= static_cast<int>(inv.size)) {
        reset_hunt_state();
        suppress_collect(self);  // don't re-vacuum our own drops at the depot
        const auto tgt = depot[bot_id_ % depot.size()];
        if (warp_towards(self, world, tgt)) return;
        release_claim(fleet);
        run_deposit(self, build_prize_plan(inv, item));  // drop ALL prizes (blocking)
        return;  // done; re-evaluated next tick (pack now empty of prizes)
    }

    // ---- 4. warp to our assigned hunt world (fleet-spread by bot index) ----
    if (!hunt.empty()) {
        const auto tgt = hunt[bot_id_ % hunt.size()];
        if (warp_towards(self, world, tgt)) {
            reset_hunt_state();
            return;
        }
    }

    // Reached the search phase = we should be hunting in a real world. Bail if
    // somehow still not in one (e.g. white door with no hunt world configured).
    if (!self.in_world()) return;

    // Back in the hunt world -> restore the user's auto-collect value (forced off
    // during the deposit).
    restore_collect(self);

    // ---- 5. CANDIDATE-ELIMINATION SEARCH -----------------------------------
    // 5a. start a fresh hunt: full candidate grid over the REAL world bounds
    if (!hunt_active_) {
        reset_candidates(self, min_y, max_y_cap, width_cap);
        base_charged_ = inv_amount(inv, item);
        base_dead_ = inv_amount(inv, kDeadGeigerId);
        base_inv_ = inv.items;  // snapshot for the gained-loot diff at the next find
        steps_this_hunt_ = 0;
        hunt_active_ = true;
        // Only act on a GENUINELY fresh reading (newer than the last one we
        // consumed) - never on a stale Prize particle left over from a prior find.
        if (auto sig0 = self.geiger_signal(); sig0 && sig0->timestamp_ms > last_signal_ts_) {
            last_signal_ts_ = sig0->timestamp_ms;
            if (sig0->area_type == GeigerArea::Prize) {
                on_prize(self, fleet, recharge_min);
                return;
            }
            apply_observation(Obs{sig0->area_type, static_cast<std::uint16_t>(sig0->x),
                                  static_cast<std::uint16_t>(sig0->y)});
        }
        if (detect_prize_fallback(inv, item)) {
            on_prize(self, fleet, recharge_min);
            return;
        }
        return;  // begin probing next tick
    }

    // 5b. stall guard: probe budget exhausted with no prize -> re-init next tick
    if (steps_this_hunt_ >= max_steps) {
        reset_hunt_state();
        return;
    }

    // 5c. pick the next probe (the lone survivor once the set collapses)
    if (candidates_.empty()) {
        reset_candidates(self, min_y, max_y_cap, width_cap);
        return;
    }
    // Always route through choose_probe: when the set is small (<=24) it also
    // probes each survivor + its neighbours, so a near-miss collapse to a single
    // wrong tile can still walk onto the true adjacent prize tile (rather than
    // re-standing on the lone survivor forever).
    const Point target = choose_probe(last_observation_);

    // 5d. fleet claim on the committed tile so two bots don't chase one tile
    const std::string key = "geiger:" + world_name_ + ":" + std::to_string(target.x) + "," +
                            std::to_string(target.y);
    if (!claim_key_.empty() && claim_key_ != key) release_claim(fleet);
    if (!fleet.claim(key, bot_id_)) {
        target_history_[(static_cast<std::uint32_t>(target.x) << 16) | target.y] += 4;  // steer away
        return;
    }
    claim_key_ = key;

    // 5e. probe = walk to the tile, then DWELL on it. Standing still is what makes
    //     the game (a) escalate green -> rapid_green (same tile seen >=2x within
    //     2.5s) and (b) AUTO-ADD the prize once we're on the source tile; we catch
    //     that via the inventory diff. Dwell longer when close, quick when far.
    const bool close =
        (last_observation_ && last_observation_->area &&
         (*last_observation_->area == GeigerArea::Green ||
          *last_observation_->area == GeigerArea::RapidGreen)) ||
        candidates_.size() <= 8;
    std::uint64_t baseline = last_signal_ts_;
    if (auto cur = self.geiger_signal())
        baseline = std::max<std::uint64_t>(baseline, cur->timestamp_ms);
    self.find_path(target.x, target.y);  // blocking walk to the tile (keeps ENet serviced)
    ++steps_this_hunt_;
    (void)dig;

    Obs obs{std::nullopt, target.x, target.y};
    const int slices = close ? 8 : 3;
    const std::uint64_t slice_ms = close ? 550u : (wait_ms / 3 + 1);
    for (int i = 0; i < slices; ++i) {
        auto fresh = self.wait_for_geiger(baseline, slice_ms);
        if (fresh) {
            baseline = fresh->timestamp_ms;
            last_signal_ts_ = fresh->timestamp_ms;
            obs = Obs{fresh->area_type, static_cast<std::uint16_t>(fresh->x),
                      static_cast<std::uint16_t>(fresh->y)};
            if (fresh->area_type == GeigerArea::Prize) {  // standing on the source
                self.collect();
                on_prize(self, fleet, recharge_min);
                return;
            }
        }
        // The prize auto-adds to inventory after a few moments on the source tile.
        if (detect_prize_fallback(self.inventory(), item)) {
            self.collect();
            on_prize(self, fleet, recharge_min);
            return;
        }
        if (!close && fresh) break;  // far: one reading is enough -> move on fast
    }

    // No-signal recovery: if the geiger emits NO particle for several probes, the
    // counter almost certainly isn't active (or we're mis-equipped) - blindly
    // probing the grid centroid is exactly the "stuck at ~centre" symptom. Forget
    // the (possibly wrong) equip belief so section 2 re-wears next tick, and reset
    // the hunt. Mirrors Mori's no_signal_steps recovery.
    if (obs.area.has_value()) {
        no_signal_probes_ = 0;
    } else if (++no_signal_probes_ >= 3) {
        no_signal_probes_ = 0;
        release_claim(fleet);
        self.mark_item_equipped(item, false);  // -> is_item_equipped false -> re-wear
        reset_hunt_state();
        return;
    }

    // 5g. eliminate candidates by the (hottest) reading's ring
    apply_observation(obs);

    // 5h. endgame nudge: on/near a hot tile (or the lone survivor) grab any drop
    if ((obs.area && (*obs.area == GeigerArea::Green || *obs.area == GeigerArea::RapidGreen)) ||
        candidates_.size() == 1) {
        self.collect();
    }
    // else: probe again next tick
}

}  // namespace adonai::automation
