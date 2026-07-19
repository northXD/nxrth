// Nxrth — the IN-WORLD / AUTOMATION half of the Bot engine (port spec 08).
// Implements the in-world GameUpdate handlers (world load, tiles, objects,
// inventory), the geiger hooks, all action helpers (say/warp/place/walk/
// pathfind/collect/…), the native fleet-aware automation seam, and the per-tick
// FleetState publish. The connect/login/service_once/on_call_function halves live
// in the sibling translation units (bot_core_state.cpp / bot_core_handlers.cpp);
// this file only defines the methods bot.h assigns to the automation unit.
//
// GT is little-endian; positions are PIXELS on the Bot, TILES in BotState/UI.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "bot/bot.h"
#include "core/logger.h"

namespace nxrth::bot {

namespace {

using nxrth::protocol::GameUpdatePacket;
using GPT = nxrth::protocol::GamePacketType;
namespace PF = nxrth::protocol::PacketFlags;

// Toggle a flag bit in the raw u32 flags word (mirrors Rust bitflags .set).
inline void set_flag(std::uint32_t& flags, std::uint32_t bit, bool on) {
    if (on)
        flags |= bit;
    else
        flags &= ~bit;
}

// Fresh packet with only the type byte set (all other fields default 0).
inline GameUpdatePacket make_pkt(GPT type) {
    GameUpdatePacket p;
    p.packet_type = static_cast<std::uint8_t>(type);
    return p;
}

// Little-endian u32 read (GT wire == x86 host order).
inline std::uint32_t rd_u32_le(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

// §7.4 f32_close — integrality / marker tolerance.
inline bool f32_close(float a, float b) { return std::fabs(a - b) <= 0.01f; }

// §7.2 geiger_tile_coord — particle pixel -> tile with per-axis spawn offset.
inline std::optional<std::uint32_t> geiger_tile_coord(float pixel, float offset) {
    if (!std::isfinite(pixel) || pixel < offset) return std::nullopt;
    float t = std::round((pixel - offset) / 32.0f);
    if (t < 0.0f) t = 0.0f;
    return static_cast<std::uint32_t>(t);
}

}  // namespace

// ===========================================================================
// §5.2 World load (SendMapData) — full state reset.
// ===========================================================================
void Bot::load_world(const GameUpdatePacket& pkt) {
    players_.clear();
    local_ = LocalPlayer{};
    geiger_green_repeat_.reset();

    auto parsed = nxrth::world::World::try_parse(pkt.extra_data.data(), pkt.extra_data.size());
    if (!parsed) {
        log_console("[Bot] World parse error");
        return;
    }
    world_ = std::move(parsed);
    const auto& w = *world_;
    log_console("[Bot] World: " + std::to_string(w.tile_map.width) + "x" +
                std::to_string(w.tile_map.height) + " tiles, " +
                std::to_string(w.objects.size()) + " objects");

    std::vector<TileInfo> tiles;
    tiles.reserve(w.tile_map.tiles.size());
    for (const auto& t : w.tile_map.tiles)
        tiles.push_back(TileInfo{t.fg_item_id, t.bg_item_id, t.flags_raw, t.tile_type});

    std::vector<WorldObjectInfo> objs;
    objs.reserve(w.objects.size());
    for (const auto& o : w.objects)
        objs.push_back(WorldObjectInfo{o.uid, o.item_id, o.x, o.y, o.count});

    std::string name = w.tile_map.world_name;
    std::uint32_t width = w.tile_map.width, height = w.tile_map.height;
    state_->write([&](BotState& s) {
        s.tiles = std::move(tiles);
        s.objects = std::move(objs);
        s.players.clear();
        s.world_name = name;
        s.world_width = width;
        s.world_height = height;
        s.status = BotStatus::InGame;
        s.geiger_signal.reset();
    });
    notify_dirty();
}

// ===========================================================================
// §5.3 on_state — position updates (self / other players).
// ===========================================================================
void Bot::on_state(const GameUpdatePacket& pkt) {
    if (pkt.net_id == local_.net_id) {
        pos_x_ = pkt.vector_x;
        pos_y_ = pkt.vector_y;
        float tx = pos_x_ / 32.0f, ty = pos_y_ / 32.0f;
        state_->write([&](BotState& s) {
            s.pos_x = tx;
            s.pos_y = ty;
        });
        notify_dirty();
        return;
    }
    auto it = players_.find(pkt.net_id);
    if (it == players_.end()) return;
    it->second.position = {pkt.vector_x, pkt.vector_y};
    float tx = pkt.vector_x / 32.0f, ty = pkt.vector_y / 32.0f;
    std::uint32_t nid = pkt.net_id;
    state_->write([&](BotState& s) {
        for (auto& p : s.players) {
            if (p.net_id == nid) {
                p.pos_x = tx;
                p.pos_y = ty;
                break;
            }
        }
    });
    notify_dirty();
}

// ===========================================================================
// §5.4 on_tile_change — TileChangeRequest.
// ===========================================================================
void Bot::on_tile_change(const GameUpdatePacket& pkt) {
    if (!world_) return;
    auto x = static_cast<std::uint32_t>(pkt.int_x);
    auto y = static_cast<std::uint32_t>(pkt.int_y);
    auto item_id = static_cast<std::uint16_t>(pkt.value);
    nxrth::world::Tile* tile = world_->get_tile_mut(x, y);
    if (!tile) return;

    if (item_id == 18) {  // fist / punch — break fg first, else bg
        if (tile->fg_item_id != 0) {
            tile->fg_item_id = 0;
            tile->tile_type = nxrth::world::tiletype::Basic{};
        } else {
            tile->bg_item_id = 0;
        }
    } else {
        tile->fg_item_id = item_id;
    }
    std::uint16_t fg = tile->fg_item_id, bg = tile->bg_item_id;
    std::size_t idx = nxrth::world::tile_index(x, y, world_->tile_map.width);
    state_->write([&](BotState& s) {
        if (idx < s.tiles.size()) {
            s.tiles[idx].fg_item_id = fg;
            s.tiles[idx].bg_item_id = bg;
        }
    });
    notify_dirty();
    log_console("[Bot] TileChange (" + std::to_string(x) + "," + std::to_string(y) +
                ") item=" + std::to_string(item_id));
}

// ===========================================================================
// §5.5 on_send_tile_update_data — single-tile binary update.
// ===========================================================================
void Bot::on_send_tile_update_data(const GameUpdatePacket& pkt) {
    if (!world_) return;
    auto x = static_cast<std::uint32_t>(pkt.int_x);
    auto y = static_cast<std::uint32_t>(pkt.int_y);
    auto res = world_->update_tile_from_bytes(x, y, pkt.extra_data.data(), pkt.extra_data.size());
    if (!res) return;
    std::uint16_t fg = res->first, bg = res->second;
    std::size_t idx = nxrth::world::tile_index(x, y, world_->tile_map.width);
    state_->write([&](BotState& s) {
        if (idx < s.tiles.size()) {
            s.tiles[idx].fg_item_id = fg;
            s.tiles[idx].bg_item_id = bg;
        }
    });
    notify_dirty();
    log_console("[Bot] TileUpdateData (" + std::to_string(x) + "," + std::to_string(y) + ")");
}

// ===========================================================================
// §5.6 on_send_tile_update_data_multiple — batched (fixed 12-byte stride).
// ===========================================================================
void Bot::on_send_tile_update_data_multiple(const GameUpdatePacket& pkt) {
    const auto& data = pkt.extra_data;
    if (data.size() < 4) return;
    if (!world_) return;
    std::uint32_t count = rd_u32_le(data.data());
    std::size_t offset = 4;
    std::uint32_t width = world_->tile_map.width;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + 12 > data.size()) break;
        std::uint32_t x = rd_u32_le(data.data() + offset);
        std::uint32_t y = rd_u32_le(data.data() + offset + 4);
        const std::uint8_t* tile_data = data.data() + offset + 8;
        std::size_t tile_len = data.size() - (offset + 8);
        auto res = world_->update_tile_from_bytes(x, y, tile_data, tile_len);
        if (res) {
            std::uint16_t fg = res->first, bg = res->second;
            std::size_t idx = nxrth::world::tile_index(x, y, width);
            state_->write([&](BotState& s) {
                if (idx < s.tiles.size()) {
                    s.tiles[idx].fg_item_id = fg;
                    s.tiles[idx].bg_item_id = bg;
                }
            });
        }
        offset += 12;  // fixed stride, regardless of actual per-tile payload
    }
    notify_dirty();
    log_console("[Bot] TileUpdateDataMultiple count=" + std::to_string(count));
}

// ===========================================================================
// §5.7 on_send_tile_tree_state — tree harvested.
// ===========================================================================
void Bot::on_send_tile_tree_state(const GameUpdatePacket& pkt) {
    if (!world_) return;
    auto x = static_cast<std::uint32_t>(pkt.int_x);
    auto y = static_cast<std::uint32_t>(pkt.int_y);
    nxrth::world::Tile* tile = world_->get_tile_mut(x, y);
    if (!tile) return;
    tile->fg_item_id = 0;
    tile->tile_type = nxrth::world::tiletype::Basic{};
    std::uint16_t bg = tile->bg_item_id;
    std::size_t idx = nxrth::world::tile_index(x, y, world_->tile_map.width);
    state_->write([&](BotState& s) {
        if (idx < s.tiles.size()) s.tiles[idx].fg_item_id = 0;
    });
    (void)bg;
    notify_dirty();
    log_console("[Bot] TileTreeState (" + std::to_string(x) + "," + std::to_string(y) +
                ") harvested");
}

// ===========================================================================
// §5.8 on_modify_item_inventory + emit_inventory_update.
// ===========================================================================
void Bot::on_modify_item_inventory(const GameUpdatePacket& pkt) {
    auto item_id = static_cast<std::uint16_t>(pkt.value);
    if (pkt.jump_count != 0) {  // remove
        std::uint8_t amount = pkt.jump_count;
        inventory_.sub_item(item_id, amount);
        if (inventory_.items.find(item_id) == inventory_.items.end())
            equipped_items_.erase(item_id);
        log_console("[Bot] ModifyItemInventory item=" + std::to_string(item_id) + " -" +
                    std::to_string(amount));
    } else {
        std::uint8_t amount = pkt.animation_type;
        if (amount != 0) {
            inventory_.add_item(item_id, amount);
            log_console("[Bot] ModifyItemInventory item=" + std::to_string(item_id) + " +" +
                        std::to_string(amount));
        } else {
            log_console("[Bot] ModifyItemInventory item=" + std::to_string(item_id) +
                        " +0 ignored");
        }
    }
    emit_inventory_update();
}

void Bot::emit_inventory_update() {
    std::vector<InvSlot> slots;
    slots.reserve(inventory_.items.size());
    for (const auto& [id, item] : inventory_.items) {
        std::uint8_t action_type = 0;
        if (const auto* info = items_dat_->find_by_id(item.id)) action_type = info->action_type;
        slots.push_back(InvSlot{item.id, item.amount,
                                (item.flag & 1) != 0, action_type});
    }
    std::uint32_t inv_size = inventory_.size;
    state_->write([&](BotState& s) {
        s.inventory = std::move(slots);
        s.inventory_size = inv_size;
    });
    notify_dirty();
}

// ===========================================================================
// §5.10 on_item_change_object — dropped-object lifecycle (auto-pickup backbone).
// ===========================================================================
void Bot::on_item_change_object(const GameUpdatePacket& pkt) {
    if (!world_) return;

    auto rebuild_objects = [&]() {
        std::vector<WorldObjectInfo> objs;
        objs.reserve(world_->objects.size());
        for (const auto& o : world_->objects)
            objs.push_back(WorldObjectInfo{o.uid, o.item_id, o.x, o.y, o.count});
        state_->write([&](BotState& s) { s.objects = std::move(objs); });
        notify_dirty();
    };

    if (pkt.net_id == 0xFFFFFFFFu) {  // new drop
        nxrth::world::WorldObject o;
        o.item_id = static_cast<std::uint16_t>(pkt.value);
        o.x = std::ceil(pkt.vector_x);
        o.y = std::ceil(pkt.vector_y);
        o.count = static_cast<std::uint8_t>(pkt.float_variable);
        o.flags = static_cast<std::uint8_t>(pkt.object_type);
        o.uid = world_->next_object_uid++;
        world_->objects.push_back(o);
        rebuild_objects();
        return;
    }

    if (pkt.net_id == 0xFFFFFFFDu) {  // count update
        auto item_id = static_cast<std::uint16_t>(pkt.value);
        float x = std::ceil(pkt.vector_x), y = std::ceil(pkt.vector_y);
        for (auto& o : world_->objects) {
            if (o.item_id == item_id && o.x == x && o.y == y) {
                o.count = static_cast<std::uint8_t>(pkt.float_variable);
                break;
            }
        }
        rebuild_objects();
        return;
    }

    if (pkt.net_id > 0) {  // collected (by uid == pkt.value)
        auto it = std::find_if(world_->objects.begin(), world_->objects.end(),
                               [&](const nxrth::world::WorldObject& o) { return o.uid == pkt.value; });
        if (it == world_->objects.end()) return;
        nxrth::world::WorldObject item = *it;
        world_->objects.erase(it);
        rebuild_objects();
        if (pkt.net_id == local_.net_id) {  // we collected it
            std::uint8_t current = 0;
            auto inv_it = inventory_.items.find(item.item_id);
            if (inv_it != inventory_.items.end()) current = inv_it->second.amount;
            int headroom = static_cast<int>(PER_ITEM_INV_CAP) - static_cast<int>(current);
            if (headroom < 0) headroom = 0;
            auto to_add = static_cast<std::uint8_t>(std::min<int>(item.count, headroom));
            inventory_.add_item(item.item_id, to_add);
            log_console("[Bot] ItemCollect id=" + std::to_string(item.item_id) +
                        " count=" + std::to_string(to_add));
            emit_inventory_update();
        }
    }
}

// ===========================================================================
// §5.11 on_send_lock — lock placed.
// ===========================================================================
void Bot::on_send_lock(const GameUpdatePacket& pkt) {
    if (!world_) return;
    auto x = static_cast<std::uint32_t>(pkt.int_x);
    auto y = static_cast<std::uint32_t>(pkt.int_y);
    auto fg = static_cast<std::uint16_t>(pkt.value);
    nxrth::world::Tile* tile = world_->get_tile_mut(x, y);
    if (!tile) return;
    tile->fg_item_id = fg;
    std::size_t idx = nxrth::world::tile_index(x, y, world_->tile_map.width);
    state_->write([&](BotState& s) {
        if (idx < s.tiles.size()) s.tiles[idx].fg_item_id = fg;
    });
    notify_dirty();
    log_console("[Bot] SendLock tile=(" + std::to_string(x) + "," + std::to_string(y) +
                ") item=" + std::to_string(fg));
}

// ===========================================================================
// §7 GEIGER HOOKS
// ===========================================================================
std::optional<std::tuple<std::uint32_t, std::uint32_t, std::uint8_t>>
Bot::decode_geiger_signal_packet(const GameUpdatePacket& pkt) {
    std::uint8_t pt = pkt.packet_type;
    if (pt != static_cast<std::uint8_t>(GPT::SendParticleEffect) &&
        pt != static_cast<std::uint8_t>(GPT::SendParticleEffectV2))
        return std::nullopt;
    if (pkt.animation_type != 114) return std::nullopt;
    if (!f32_close(pkt.vector_y2, 114.0f)) return std::nullopt;
    float ra = std::round(pkt.vector_x2);
    if (!f32_close(pkt.vector_x2, ra)) return std::nullopt;
    int rai = static_cast<int>(ra);
    if (rai < 0 || rai > 3) return std::nullopt;
    auto x = geiger_tile_coord(pkt.vector_x, 10.0f);
    auto y = geiger_tile_coord(pkt.vector_y, 17.0f);
    if (!x || !y) return std::nullopt;
    return std::make_tuple(*x, *y, static_cast<std::uint8_t>(rai));
}

void Bot::update_geiger_signal(const GameUpdatePacket& pkt) {
    auto decoded = decode_geiger_signal_packet(pkt);
    if (!decoded) return;
    auto [x, y, raw_area] = *decoded;
    std::uint64_t ts = now_millis();

    GeigerArea area;
    switch (raw_area) {
        case 0: area = GeigerArea::Red; break;
        case 1: area = GeigerArea::Yellow; break;
        case 2: area = GeigerArea::Green; break;
        case 3: area = GeigerArea::Prize; break;
        default: return;
    }

    if (raw_area == 2) {  // rapid-green detection
        if (geiger_green_repeat_ && geiger_green_repeat_->x == x &&
            geiger_green_repeat_->y == y && ts - geiger_green_repeat_->last_seen_ms <= 2500) {
            if (geiger_green_repeat_->count < 10) geiger_green_repeat_->count++;
            geiger_green_repeat_->last_seen_ms = ts;
        } else {
            geiger_green_repeat_ = GeigerGreenRepeat{x, y, ts, 1};
        }
        if (geiger_green_repeat_->count >= 2) area = GeigerArea::RapidGreen;
    } else {
        geiger_green_repeat_.reset();
    }

    state_->write([&](BotState& s) {
        s.geiger_signal = GeigerSignal{x, y, area, ts};
    });
    notify_dirty();
}

void Bot::sync_geiger_state_from_console(const std::string& message) {
    if (message.find("You are detecting radiation") != std::string::npos) {
        mark_item_equipped(GEIGER_COUNTER_ITEM_ID, true);
        mark_item_equipped(DEAD_GEIGER_COUNTER_ITEM_ID, false);
        log_console("[Bot] geiger: charged counter is active");
    } else if (message.find("Charging Geiger Counter") != std::string::npos &&
               message.find("mod added") != std::string::npos) {
        mark_item_equipped(DEAD_GEIGER_COUNTER_ITEM_ID, true);
        mark_item_equipped(GEIGER_COUNTER_ITEM_ID, false);
        log_console("[Bot] geiger: dead counter is charging/active");
    } else if (message.find("Geiger Counter removed") != std::string::npos) {
        mark_item_equipped(GEIGER_COUNTER_ITEM_ID, false);
        mark_item_equipped(DEAD_GEIGER_COUNTER_ITEM_ID, false);
        log_console("[Bot] geiger: counter removed/inactive");
    }
}

// ===========================================================================
// §9.1 Movement
// ===========================================================================
void Bot::walk(std::int32_t tile_x, std::int32_t tile_y) {
    float target_x = static_cast<float>(tile_x) * 32.0f;
    float target_y = static_cast<float>(tile_y) * 32.0f;
    bool facing_left = target_x < pos_x_;
    pos_x_ = target_x;
    pos_y_ = target_y;
    float tx = pos_x_ / 32.0f, ty = pos_y_ / 32.0f;
    state_->write([&](BotState& s) {
        s.pos_x = tx;
        s.pos_y = ty;
    });
    notify_dirty();

    std::uint32_t flags = PF::WALK | PF::STANDING;
    set_flag(flags, PF::FACING_LEFT, facing_left);
    GameUpdatePacket p = make_pkt(GPT::State);
    p.vector_x = target_x;
    p.vector_y = target_y + 2.0f;
    p.int_x = -1;
    p.int_y = -1;
    p.flags = flags;
    send_game_packet(p, false);  // unreliable
    sleep_ms(delays_.walk_ms);
}

void Bot::set_direction(bool facing_left) {
    std::uint32_t flags = PF::STANDING;
    set_flag(flags, PF::FACING_LEFT, facing_left);
    GameUpdatePacket p = make_pkt(GPT::State);
    p.net_id = local_.net_id;
    p.vector_x = pos_x_;
    p.vector_y = pos_y_;
    p.int_x = -1;
    p.int_y = -1;
    p.flags = flags;
    send_game_packet(p, true);
}

// ===========================================================================
// §9.2 Pathfinding
// ===========================================================================
std::vector<std::pair<std::uint16_t, std::uint8_t>> Bot::build_collision_grid(
    bool item_collect) const {
    if (!world_) return {};
    return nxrth::world::build_collision_tiles(*world_, *items_dat_, item_collect);
}

void Bot::find_path(std::uint32_t x, std::uint32_t y) {
    pathfind_target_ = std::make_pair(x, y);
    pathfind_recalc_ = false;
    while (pathfind_target_.has_value()) {
        if (!world_) {
            pathfind_target_.reset();
            return;
        }
        auto tiles = build_collision_grid(/*item_collect=*/false);  // find_path ignores Lock
        std::uint32_t w = world_->tile_map.width, h = world_->tile_map.height;
        astar_.update_from_tiles(w, h, tiles);

        std::uint32_t from_x = nxrth::world::pixel_to_tile(pos_x_);
        std::uint32_t from_y = nxrth::world::pixel_to_tile(pos_y_);
        bool acc = has_access();
        auto [goal_x, goal_y] = *pathfind_target_;
        if (from_x == goal_x && from_y == goal_y) {
            pathfind_target_.reset();
            break;
        }
        auto path = astar_.find_path(from_x, from_y, goal_x, goal_y, acc);
        if (!path) {
            pathfind_target_.reset();
            break;
        }
        pathfind_recalc_ = false;
        for (const auto& node : *path) {
            std::uint32_t cx = nxrth::world::pixel_to_tile(pos_x_);
            std::uint32_t cy = nxrth::world::pixel_to_tile(pos_y_);
            if (node.x == cx && node.y == cy) continue;
            walk(static_cast<std::int32_t>(node.x), static_cast<std::int32_t>(node.y));
            if (pathfind_recalc_) break;  // server corrected our position
        }
        if (pathfind_recalc_)
            continue;  // replan from new server position
        pathfind_target_.reset();
    }
}

std::optional<GeigerSignal> Bot::wait_for_geiger(std::uint64_t newer_than_ms,
                                                 std::uint64_t timeout_ms) {
    // Poll the shared geiger reading, servicing ENet between polls (the particle
    // is server-pushed asynchronously after we move). Accept only a strictly
    // newer reading so we never re-consume a stale hot signal.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        auto sig = geiger_signal();
        if (sig && sig->timestamp_ms > newer_than_ms) return sig;
        if (stop_ && stop_->load()) return std::nullopt;
        if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        sleep_ms(120);  // keeps ENet serviced while we wait for the next particle
    }
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> Bot::compute_path(std::uint32_t x,
                                                                       std::uint32_t y) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    if (!world_) return out;
    auto tiles = build_collision_grid(/*item_collect=*/true);  // Lock -> 3
    std::uint32_t w = world_->tile_map.width, h = world_->tile_map.height;
    astar_.update_from_tiles(w, h, tiles);
    std::uint32_t from_x = nxrth::world::pixel_to_tile(pos_x_);
    std::uint32_t from_y = nxrth::world::pixel_to_tile(pos_y_);
    auto path = astar_.find_path(from_x, from_y, x, y, has_access());
    if (!path) return out;
    out.reserve(path->size());
    for (const auto& node : *path) out.emplace_back(node.x, node.y);
    return out;
}

// ===========================================================================
// §9.3 Tile actions
// ===========================================================================
void Bot::place(std::int32_t offset_x, std::int32_t offset_y, std::uint32_t item_id,
                bool is_punch) {
    if (!is_punch && !inventory_.has_item(static_cast<std::uint16_t>(item_id), 1)) return;

    std::int32_t base_x = nxrth::world::pixel_to_tile_floor(pos_x_);
    std::int32_t base_y = nxrth::world::pixel_to_tile_floor(pos_y_);
    std::int32_t tile_x = base_x + offset_x;
    std::int32_t tile_y = base_y + offset_y;
    if (std::abs(tile_x - base_x) > 4 || std::abs(tile_y - base_y) > 4) return;  // ±4 reach

    GameUpdatePacket p = make_pkt(GPT::TileChangeRequest);
    p.vector_x = pos_x_;
    p.vector_y = pos_y_;
    p.int_x = tile_x;
    p.int_y = tile_y;
    p.value = item_id;
    send_game_packet(p, true);

    std::uint32_t flags = (is_punch ? PF::PUNCH : PF::PLACE) | PF::STANDING;
    set_flag(flags, PF::FACING_LEFT, base_x > tile_x);
    p.packet_type = static_cast<std::uint8_t>(GPT::State);
    p.flags = flags;
    send_game_packet(p, true);

    sleep_ms(delays_.place_ms);

    if (!is_punch && item_id != 18 && item_id != 32) {  // fist/wrench never decrement
        inventory_.sub_item(static_cast<std::uint16_t>(item_id), 1);
        emit_inventory_update();
    }
}

void Bot::punch(std::int32_t ox, std::int32_t oy) { place(ox, oy, 18, /*is_punch=*/true); }
void Bot::wrench(std::int32_t ox, std::int32_t oy) { place(ox, oy, 32, /*is_punch=*/false); }

void Bot::wrench_at(std::int32_t tile_x, std::int32_t tile_y) {
    std::int32_t base_x = nxrth::world::pixel_to_tile_floor(pos_x_);
    std::int32_t base_y = nxrth::world::pixel_to_tile_floor(pos_y_);
    wrench(tile_x - base_x, tile_y - base_y);
}

void Bot::wrench_player(std::uint32_t net_id) {
    send_text("action|wrench\n|netid|" + std::to_string(net_id) + "\n");
}

void Bot::active_tile(std::int32_t tile_x, std::int32_t tile_y) {
    GameUpdatePacket p = make_pkt(GPT::TileActivateRequest);
    p.vector_x = pos_x_;
    p.vector_y = pos_y_;
    p.int_x = tile_x;
    p.int_y = tile_y;
    send_game_packet(p, true);
}

void Bot::enter(const std::optional<std::string>& pass) {
    if (pass) {
        send_text("action|input\n|text|" + *pass + "\n");
    } else {
        auto cx = static_cast<std::int32_t>(pos_x_ / 32.0f);
        auto cy = static_cast<std::int32_t>(pos_y_ / 32.0f);
        active_tile(cx, cy);
    }
}

// ===========================================================================
// §9.4 Inventory / equip
// ===========================================================================
void Bot::wear(std::uint32_t item_id) {
    if (is_item_equipped(static_cast<std::uint16_t>(item_id))) {
        log_console("[Bot] wear skip: item " + std::to_string(item_id) + " already equipped");
        return;
    }
    GameUpdatePacket p = make_pkt(GPT::ItemActivateRequest);
    p.value = item_id;
    send_game_packet(p, true);
    mark_item_equipped(static_cast<std::uint16_t>(item_id), true);
}

void Bot::activate_item(std::uint32_t item_id) {
    GameUpdatePacket p = make_pkt(GPT::ItemActivateRequest);
    p.value = item_id;
    send_game_packet(p, true);
}

void Bot::unwear(std::uint32_t item_id) {
    GameUpdatePacket p = make_pkt(GPT::ItemActivateRequest);
    p.value = item_id;
    send_game_packet(p, true);
    mark_item_equipped(static_cast<std::uint16_t>(item_id), false);
}

void Bot::drop_item(std::uint32_t item_id, std::uint32_t amount) {
    send_text("action|drop\n|itemID|" + std::to_string(item_id) + "\n");
    temporary_data_.dialog_callback = [item_id, amount](Bot& b) {
        b.send_text("action|dialog_return\ndialog_name|drop_item\nitemID|" +
                    std::to_string(item_id) + "|\ncount|" + std::to_string(amount) + "\n");
    };
}

void Bot::trash_item(std::uint32_t item_id, std::uint32_t amount) {
    send_text("action|trash\n|itemID|" + std::to_string(item_id) + "\n");
    temporary_data_.dialog_callback = [item_id, amount](Bot& b) {
        b.send_text("action|dialog_return\ndialog_name|trash_item\nitemID|" +
                    std::to_string(item_id) + "|\ncount|" + std::to_string(amount) + "\n");
    };
}

void Bot::fast_drop(std::uint32_t item_id, std::uint32_t count) {
    send_text("action|dialog_return\ndialog_name|drop_item\nitemID|" + std::to_string(item_id) +
              "|\ncount|" + std::to_string(count) + "\n");
}

void Bot::fast_trash(std::uint32_t item_id, std::uint32_t count) {
    send_text("action|dialog_return\ndialog_name|trash_item\nitemID|" + std::to_string(item_id) +
              "|\ncount|" + std::to_string(count) + "\n");
}

void Bot::accept_access() {
    wrench_player(local_.net_id);
    std::uint32_t net_id = local_.net_id;
    temporary_data_.dialog_callback = [net_id](Bot& b) {
        b.send_text("action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(net_id) +
                    "|\nbuttonClicked|acceptlock\n");
        b.temporary_data_.dialog_callback = [](Bot& b2) {
            b2.send_text("action|dialog_return\ndialog_name|acceptaccess\n");
        };
    };
}

bool Bot::is_item_equipped(std::uint16_t item_id) const {
    return equipped_items_.count(item_id) != 0 || inventory_.is_active(item_id);
}

void Bot::mark_item_equipped(std::uint16_t item_id, bool active) {
    if (active)
        equipped_items_.insert(item_id);
    else
        equipped_items_.erase(item_id);
    inventory_.set_active(item_id, active);
    emit_inventory_update();
}

// ===========================================================================
// §9.5 World / session
// ===========================================================================
void Bot::say(const std::string& text) {
    send_text("action|input\n|text|" + text + "\n");
}

void Bot::warp(const std::string& name, const std::string& id) {
    wait_for_global_gate(warp_gate(), WARP_STAGGER_MS, "warp");  // fleet-serialized
    redirect_.reset();
    redirect_attempts_ = 0;
    send_game_message("action|join_request\nname|" + name + "|" + id + "\ninvitedWorld|0\n");
}

void Bot::leave_world() { send_game_message("action|quit_to_exit\n"); }
void Bot::respawn() { send_text("action|respawn\n"); }

void Bot::disconnect() {
    if (peer_id_) host_.peer_disconnect(*peer_id_, 0);
}

void Bot::reconnect() { reconnect_main(/*refresh_token=*/false); }

// ===========================================================================
// §9.6 Object collection
// ===========================================================================
bool Bot::has_access() const {
    if (!world_) return false;
    return nxrth::world::world_has_access(*world_, local_.user_id);
}

void Bot::collect_object_at(std::uint32_t uid, float range_tiles) {
    if (!world_) return;
    const nxrth::world::WorldObject* found = nullptr;
    for (const auto& o : world_->objects) {
        if (o.uid == uid) {
            found = &o;
            break;
        }
    }
    if (!found) return;
    float dx = pos_x_ - found->x, dy = pos_y_ - found->y;
    float r_px = range_tiles * 32.0f;
    if (dx * dx + dy * dy > r_px * r_px) return;
    GameUpdatePacket p = make_pkt(GPT::ItemActivateObjectRequest);
    p.vector_x = found->x;
    p.vector_y = found->y;
    p.value = found->uid;
    send_game_packet(p, true);
}

std::size_t Bot::collect() {
    if (!world_) return 0;
    if (inventory_.item_count >= inventory_.size) return 0;  // full

    std::uint32_t radius = collect_radius_tiles_ < 1 ? 1
                           : (collect_radius_tiles_ > 5 ? 5 : collect_radius_tiles_);
    float r_px = static_cast<float>(radius) * 32.0f;

    struct Near {
        float ring;
        std::uint32_t uid;
        float x, y;
        std::uint16_t item_id;
    };
    std::vector<Near> nearby;
    for (const auto& o : world_->objects) {
        if (collect_blacklist_.count(o.item_id)) continue;
        float dx = std::fabs(pos_x_ - o.x), dy = std::fabs(pos_y_ - o.y);
        if (dx > r_px || dy > r_px) continue;  // Chebyshev box
        nearby.push_back(Near{std::max(dx, dy), o.uid, o.x, o.y, o.item_id});
    }
    if (nearby.empty()) return 0;
    std::sort(nearby.begin(), nearby.end(),
              [](const Near& a, const Near& b) { return a.ring < b.ring; });  // nearest first

    auto tiles = build_collision_grid(/*item_collect=*/true);
    astar_.update_from_tiles(world_->tile_map.width, world_->tile_map.height, tiles);
    std::uint32_t from_x = nxrth::world::pixel_to_tile(pos_x_);
    std::uint32_t from_y = nxrth::world::pixel_to_tile(pos_y_);
    bool acc = has_access();

    std::size_t sent = 0;
    std::size_t limit = std::min<std::size_t>(nearby.size(), COLLECT_MAX_PER_TICK);
    for (std::size_t i = 0; i < limit; ++i) {
        const Near& n = nearby[i];
        std::uint32_t tx = nxrth::world::pixel_to_tile(n.x);
        std::uint32_t ty = nxrth::world::pixel_to_tile(n.y);
        if (!astar_.find_path(from_x, from_y, tx, ty, acc)) continue;  // unreachable

        bool can_collect;
        if (n.item_id == GEMS_ITEM_ID) {
            can_collect = true;
        } else {
            auto it = inventory_.items.find(n.item_id);
            if (it != inventory_.items.end())
                can_collect = it->second.amount < PER_ITEM_INV_CAP;
            else
                can_collect = inventory_.item_count < inventory_.size;
        }
        if (!can_collect) continue;

        GameUpdatePacket p = make_pkt(GPT::ItemActivateObjectRequest);
        p.vector_x = n.x;
        p.vector_y = n.y;
        p.value = n.uid;
        send_game_packet(p, true);
        ++sent;
    }
    return sent;
}

void Bot::set_auto_collect(bool enabled) {
    auto_collect_ = enabled;
    // Mirror into the shared snapshot so the UI toggle reflects reality (otherwise
    // it reads the always-true default and snaps back ON every frame).
    if (state_) state_->write([&](BotState& s) { s.auto_collect = enabled; });
    notify_dirty();
}

}  // namespace nxrth::bot
