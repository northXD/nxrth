// Adonai — world map model + SendMapData byte parser implementation.
// Byte-exact port of Mori/world/mod_impl.rs, world/constants.rs.
#include "world/world.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "core/logger.h"

namespace adonai::world {

// --- CBOR tile-id set (world/constants.rs) ----------------------------------
namespace {
// Rust "{v:#x}" formatting ("0x18", "0x0") for the diagnostic log strings.
std::string hex_str(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

constexpr std::uint16_t kCborTileIds[] = {
    15376, 15546, 3548, 14662, 14666,
    8624, 8630, 8636, 8642, 8648, 8654, 8660, 8666, 8672, 8678,
    8684, 8690, 8696, 8702, 8708, 8714,
};

// Little-endian scalar reads for the length-checked incremental updater.
std::uint16_t le_u16(const std::uint8_t* p) {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
std::uint32_t le_u32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

template <std::size_t N>
void read_array(Cursor& cur, std::array<std::uint8_t, N>& out) {
    std::vector<std::uint8_t> b = cur.bytes(N);
    std::copy(b.begin(), b.end(), out.begin());
}

// §6.5 — extra-data dispatch on the 1-byte kind discriminant.
TileType parse_tile_extra(Cursor& cur, std::uint8_t kind, std::uint16_t fg_item_id) {
    using namespace tiletype;
    switch (kind) {
        case 1: {  // Door
            Door t;
            t.label = cur.plain_string();
            t.flags = cur.u8();
            return t;
        }
        case 2: {  // Sign
            Sign t;
            t.label = cur.plain_string();
            cur.u32();  // always 0xFFFFFFFF — discard
            return t;
        }
        case 3: {  // Lock
            Lock t;
            t.settings = cur.u8();
            t.owner_uid = cur.u32();
            t.access_count = cur.u32();
            t.access_uids.reserve(t.access_count);
            for (std::uint32_t i = 0; i < t.access_count; ++i) t.access_uids.push_back(cur.u32());
            t.minimum_level = cur.u8();
            cur.skip(7);
            if (fg_item_id == 5814) cur.skip(16);  // Guild Lock
            return t;
        }
        case 4: {  // Seed
            Seed t;
            t.age = cur.u32();
            t.item_on_tree = cur.u8();
            return t;
        }
        case 8: {  // Dice
            Dice t;
            t.symbol = cur.u8();
            return t;
        }
        case 9: {  // Provider
            Provider t;
            t.age = cur.u32();
            if (fg_item_id == 10656) cur.skip(4);
            return t;
        }
        case 10: {  // AchievementBlock
            AchievementBlock t;
            t.data = cur.u32();
            t.tile_type = cur.u8();
            return t;
        }
        case 11: {  // HearthMonitor
            HearthMonitor t;
            t.player_id = cur.u32();
            t.player_name = cur.plain_string();
            return t;
        }
        case 14: {  // Mannequin
            Mannequin t;
            t.label = cur.plain_string();
            t.unknown_1 = cur.u8();
            t.unknown_2 = cur.u16();
            t.unknown_3 = cur.u16();
            t.hat = cur.u16();
            t.shirt = cur.u16();
            t.pants = cur.u16();
            t.boots = cur.u16();
            t.face = cur.u16();
            t.hand = cur.u16();
            t.back = cur.u16();
            t.hair = cur.u16();
            t.neck = cur.u16();
            return t;
        }
        case 15: {  // BunnyEgg
            BunnyEgg t;
            t.egg_placed = cur.u32();
            return t;
        }
        case 16: {  // GameGrave
            GameGrave t;
            t.team = cur.u8();
            return t;
        }
        case 17:  // GameGenerator
            return GameGenerator{};
        case 18: {  // XenoniteCrystal
            XenoniteCrystal t;
            t.unknown_1 = cur.u8();
            t.unknown_2 = cur.u32();
            return t;
        }
        case 19: {  // PhoneBooth
            PhoneBooth t;
            t.hat = cur.u16();
            t.shirt = cur.u16();
            t.pants = cur.u16();
            t.shoes = cur.u16();
            t.face = cur.u16();
            t.hand = cur.u16();
            t.back = cur.u16();
            t.hair = cur.u16();
            t.neck = cur.u16();
            return t;
        }
        case 20: {  // Crystal
            Crystal t;
            std::uint16_t crystal_count = cur.u16();
            t.crystals.reserve(crystal_count);
            for (std::uint16_t i = 0; i < crystal_count; ++i) t.crystals.push_back(cur.u8());
            return t;
        }
        case 21: {  // CrimeInProgress
            CrimeInProgress t;
            t.label = cur.plain_string();
            t.unknown_1 = cur.u32();
            t.unknown_2 = cur.u8();
            return t;
        }
        case 22:  // Spotlight
            return Spotlight{};
        case 23: {  // DisplayBlock
            DisplayBlock t;
            t.item_id = cur.u32();
            return t;
        }
        case 24: {  // VendingMachine
            VendingMachine t;
            t.item_id = cur.u32();
            t.price = cur.i32();
            return t;
        }
        case 25: {  // FishTankPort
            FishTankPort t;
            t.flags = cur.u8();
            std::uint32_t fish_count = cur.u32();
            for (std::uint32_t i = 0; i < fish_count / 2; ++i) {
                std::uint32_t fish_item_id = cur.u32();
                std::uint32_t lbs = cur.u32();
                t.fishes.emplace_back(fish_item_id, lbs);
            }
            return t;
        }
        case 26: {  // SolarCollector
            SolarCollector t;
            read_array(cur, t.data);
            return t;
        }
        case 27: {  // Forge
            Forge t;
            t.temperature = cur.u32();
            return t;
        }
        case 28: {  // GivingTree
            GivingTree t;
            t.harvested = cur.u8();
            t.age = cur.u16();
            t.unknown_1 = cur.u16();
            t.decoration_percentage = cur.u8();
            return t;
        }
        case 30: {  // SteamOrgan
            SteamOrgan t;
            t.instrument_type = cur.u8();
            t.note = cur.u32();
            return t;
        }
        case 31: {  // SilkWorm
            SilkWorm t;
            t.flags = cur.u8();
            t.name = cur.plain_string();
            t.age = cur.u32();
            t.unknown_1 = cur.u32();
            t.unknown_2 = cur.u32();
            t.can_be_fed = cur.u8();
            t.food_saturation = cur.u32();
            t.water_saturation = cur.u32();
            t.color = cur.u32();
            t.sick_duration = cur.u32();
            return t;
        }
        case 32: {  // SewingMachine
            SewingMachine t;
            std::uint32_t bolt_len = cur.u32();
            t.bolt_ids.reserve(bolt_len);
            for (std::uint32_t i = 0; i < bolt_len; ++i) t.bolt_ids.push_back(cur.u32());
            return t;
        }
        case 33: {  // CountryFlag
            CountryFlag t;
            if (fg_item_id == 3394) t.country = cur.plain_string();
            return t;
        }
        case 34:  // LobsterTrap
            return LobsterTrap{};
        case 35: {  // PaintingEasel
            PaintingEasel t;
            t.item_id = cur.u32();
            t.label = cur.plain_string();
            return t;
        }
        case 36: {  // PetBattleCage
            PetBattleCage t;
            t.label = cur.plain_string();
            t.base_pet = cur.u32();
            t.pet_1 = cur.u32();
            t.pet_2 = cur.u32();
            return t;
        }
        case 37: {  // PetTrainer
            PetTrainer t;
            t.label = cur.plain_string();
            t.pet_total_count = cur.u32();
            t.unknown_1 = cur.u32();
            t.pets_id.reserve(t.pet_total_count);
            for (std::uint32_t i = 0; i < t.pet_total_count; ++i) t.pets_id.push_back(cur.u32());
            return t;
        }
        case 38: {  // SteamEngine
            SteamEngine t;
            t.temperature = cur.u32();
            return t;
        }
        case 39: {  // LockBot
            LockBot t;
            t.age = cur.u32();
            return t;
        }
        case 40: {  // WeatherMachine
            WeatherMachine t;
            t.settings = cur.u32();
            return t;
        }
        case 41: {  // SpiritStorageUnit
            SpiritStorageUnit t;
            t.ghost_jar_count = cur.u32();
            return t;
        }
        case 42:  // DataBedrock
            cur.skip(21);  // unk1[17] + pad[4]
            return DataBedrock{};
        case 43: {  // Shelf
            Shelf t;
            t.top_left_item_id = cur.u32();
            t.top_right_item_id = cur.u32();
            t.bottom_left_item_id = cur.u32();
            t.bottom_right_item_id = cur.u32();
            return t;
        }
        case 44: {  // VipEntrance
            VipEntrance t;
            t.unknown_1 = cur.u8();
            t.owner_uid = cur.u32();
            std::uint32_t access_count = cur.u32();
            t.access_uids.reserve(access_count);
            for (std::uint32_t i = 0; i < access_count; ++i) t.access_uids.push_back(cur.u32());
            return t;
        }
        case 45:  // ChallangeTimer
            return ChallangeTimer{};
        case 47: {  // FishWallMount
            FishWallMount t;
            t.label = cur.plain_string();
            t.item_id = cur.u32();
            t.lb = cur.u8();
            return t;
        }
        case 48: {  // Portrait
            Portrait t;
            t.label = cur.plain_string();
            t.unknown_1 = cur.u32();
            t.unknown_2 = cur.u32();
            read_array(cur, t.unknown_3);
            t.unknown_4 = cur.u8();
            t.unknown_5 = cur.u16();
            t.face = cur.u16();
            t.hat = cur.u16();
            t.hair = cur.u16();
            t.unknown_6 = cur.u32();
            if (t.hat == 12958) t.infinity_crown_data = cur.plain_string();
            return t;
        }
        case 49: {  // WeatherMachine2
            WeatherMachine2 t;
            t.unknown_1 = cur.u32();
            t.gravity = cur.u32();
            t.flags = cur.u8();
            return t;
        }
        case 50: {  // FossilPrepStation
            FossilPrepStation t;
            t.unknown_1 = cur.u32();
            return t;
        }
        case 51:  // DnaExtractor
            return DnaExtractor{};
        case 52:  // Howler
            return Howler{};
        case 53: {  // ChemsynthTank
            ChemsynthTank t;
            t.current_chem = cur.u32();
            t.target_chem = cur.u32();
            return t;
        }
        case 54: {  // StorageBlock
            StorageBlock t;
            std::uint16_t data_len = cur.u16();
            for (std::uint16_t i = 0; i < data_len / 13; ++i) {
                cur.skip(3);
                std::uint32_t id = cur.u32();
                cur.skip(2);
                std::uint32_t amount = cur.u32();
                t.items.emplace_back(id, amount);
            }
            return t;
        }
        case 55: {  // CookingOven
            CookingOven t;
            t.temperature_level = cur.u32();
            std::uint32_t ingredient_count = cur.u32();
            for (std::uint32_t i = 0; i < ingredient_count / 2; ++i) {
                std::uint32_t item_id = cur.u32();
                std::uint32_t time_added = cur.u32();
                t.ingredients.emplace_back(item_id, time_added);
            }
            t.unknown_1 = cur.u32();
            t.unknown_2 = cur.u32();
            t.unknown_3 = cur.u32();
            return t;
        }
        case 56: {  // AudioRack
            AudioRack t;
            t.note = cur.plain_string();
            t.volume = cur.u32();
            return t;
        }
        case 57: {  // GeigerCharger
            GeigerCharger t;
            t.unknown_1 = cur.u32();
            return t;
        }
        case 58:  // AdventureBegins
            return AdventureBegins{};
        case 59:  // TombRobber
            return TombRobber{};
        case 60: {  // BalloonOMatic
            BalloonOMatic t;
            t.total_rarity = cur.u32();
            t.team_type = cur.u8();
            return t;
        }
        case 61: {  // TrainingPort
            TrainingPort t;
            t.fish_lb = cur.u32();
            t.fish_status = cur.u16();
            t.fish_id = cur.u32();
            t.fish_total_exp = cur.u32();
            read_array(cur, t.unknown_1);
            t.fish_level = cur.u32();
            t.unknown_2 = cur.u32();
            read_array(cur, t.unknown_3);
            return t;
        }
        case 62: {  // ItemSucker
            ItemSucker t;
            t.item_id_to_suck = cur.u32();
            t.item_amount = cur.u32();
            t.flags = cur.u16();
            t.limit = cur.u32();
            return t;
        }
        case 63: {  // CyBot
            CyBot t;
            std::uint32_t command_data_count = cur.u32();
            for (std::uint32_t i = 0; i < command_data_count; ++i) {
                std::uint32_t command_id = cur.u32();
                std::uint32_t is_command_used = cur.u32();
                cur.skip(7);
                t.command_datas.emplace_back(command_id, is_command_used);
            }
            t.sync_timer = cur.u32();
            t.activated = cur.u32();
            return t;
        }
        case 65: {  // GuildItem
            GuildItem t;
            read_array(cur, t.unknown_1);
            return t;
        }
        case 66: {  // Growscan
            Growscan t;
            t.unknown_1 = cur.u8();
            return t;
        }
        case 67: {  // ContainmentFieldPowerNode
            ContainmentFieldPowerNode t;
            t.time = cur.u32();
            std::uint32_t linked_node_count = cur.u32();
            t.linked_nodes.reserve(linked_node_count);
            for (std::uint32_t i = 0; i < linked_node_count; ++i) t.linked_nodes.push_back(cur.u32());
            return t;
        }
        case 68: {  // SpiritBoard
            SpiritBoard t;
            t.player_required = cur.u32();
            t.unk1 = cur.plain_string();
            t.command = cur.plain_string();
            std::uint32_t num_required_items = cur.u32();
            t.required_items.reserve(num_required_items);
            for (std::uint32_t i = 0; i < num_required_items; ++i) t.required_items.push_back(cur.u32());
            return t;
        }
        case 69: {  // TesseractManipulator
            TesseractManipulator t;
            t.gems = cur.u32();
            t.next_update_ms = cur.u32();
            t.item_id = cur.u32();
            t.enabled = cur.u32();
            return t;
        }
        case 72: {  // StormyCloud
            StormyCloud t;
            t.sting_duration = cur.u32();
            t.is_solid = cur.u32();
            t.non_solid_duration = cur.u32();
            return t;
        }
        case 73: {  // TemporaryPlatform
            TemporaryPlatform t;
            t.unknown_1 = cur.u32();
            return t;
        }
        case 74:  // SafeVault
            return SafeVault{};
        case 75: {  // AngelicCountingCloud
            AngelicCountingCloud t;
            t.state = cur.u32();
            t.unknown_1 = cur.u16();
            if (t.state == 2) t.ascii_code = cur.u8();
            return t;
        }
        case 77: {  // InfinityWeatherMachine
            InfinityWeatherMachine t;
            t.interval_minutes = cur.u32();
            std::uint32_t list_size = cur.u32();
            t.weather_machine_list.reserve(list_size);
            for (std::uint32_t i = 0; i < list_size; ++i) t.weather_machine_list.push_back(cur.u32());
            return t;
        }
        case 79: {  // PineappleGuzzler
            PineappleGuzzler t;
            t.pineapple_count = cur.u32();
            return t;
        }
        case 80: {  // KrakenGalaticBlock
            KrakenGalaticBlock t;
            t.pattern_index = cur.u8();
            t.unknown_1 = cur.u32();
            t.r = cur.u8();
            t.g = cur.u8();
            t.b = cur.u8();
            return t;
        }
        case 81: {  // FriendsEntrance
            FriendsEntrance t;
            t.owner_user_id = cur.u32();
            cur.skip(2);
            std::uint32_t num_allowed = cur.u16();  // u16 count, widened
            t.allowed_friends_userid.reserve(num_allowed);
            for (std::uint32_t i = 0; i < num_allowed; ++i) t.allowed_friends_userid.push_back(cur.u32());
            return t;
        }
        default:
            adonai::log("[world] WARNING: unknown TileExtraData kind " + hex_str(kind) +
                        " at fg_item=" + std::to_string(fg_item_id));
            return Unknown{kind};
    }
}

// §6.6 — ground objects. Returns (objects, last_dropped_uid).
std::pair<std::vector<WorldObject>, std::uint32_t> parse_world_objects(Cursor& cur) {
    std::uint32_t count = cur.u32();
    std::uint32_t last_dropped_uid = cur.u32();
    if (count >= MAX_WORLD_OBJECTS) {
        throw std::runtime_error("world object count " + std::to_string(count) +
                                 " >= limit 300001");
    }
    std::vector<WorldObject> objects;
    objects.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        WorldObject o;
        o.item_id = cur.u16();
        o.x = cur.f32();
        o.y = cur.f32();
        o.count = cur.u8();
        o.flags = cur.u8();
        o.uid = cur.u32();
        if (o.item_id == 5996 || o.item_id == 1626) o.item_id = 0;  // empty markers
        objects.push_back(std::move(o));
    }
    return {std::move(objects), last_dropped_uid};
}

}  // namespace

bool is_cbor_tile_id(std::uint16_t fg_item_id) {
    for (std::uint16_t id : kCborTileIds)
        if (id == fg_item_id) return true;
    return false;
}

// §6.3 — one tile.
Tile Tile::parse(Cursor& cur, std::uint16_t /*map_version*/, std::uint32_t x, std::uint32_t y) {
    Tile t;
    t.x = x;
    t.y = y;
    t.fg_item_id = cur.u16();
    t.bg_item_id = cur.u16();
    t.parent_block = cur.u16();
    t.flags_raw = cur.u16();
    if (t.has(HAS_PARENT)) cur.u16();  // parent extra — read and discard
    if (t.has(HAS_EXTRA_DATA)) {
        std::uint8_t kind = cur.u8();
        t.tile_type = parse_tile_extra(cur, kind, t.fg_item_id);
    }
    if (is_cbor_tile_id(t.fg_item_id)) {
        std::uint32_t cbor_size = cur.u32();
        cur.skip(cbor_size);  // raw CBOR — discarded
    }
    return t;
}

// §6.2 — tile map.
WorldTileMap WorldTileMap::parse(Cursor& cur, std::uint16_t map_version) {
    WorldTileMap map;
    map.world_name = cur.plain_string();  // u16 name_len + bytes
    map.width = cur.u32();
    map.height = cur.u32();
    std::uint32_t tile_count = cur.u32();
    cur.skip(5);  // pad
    if (tile_count >= MAX_TILE_COUNT) {
        throw std::runtime_error("tile_count " + std::to_string(tile_count) + " >= limit 65026");
    }
    if (map.width == 0 && tile_count > 0) {
        throw std::runtime_error("world width is 0 with non-zero tile_count");
    }
    map.tiles.reserve(tile_count);
    for (std::uint32_t idx = 0; idx < tile_count; ++idx) {
        std::uint32_t x = idx % map.width;
        std::uint32_t y = idx / map.width;
        std::size_t pos_before = cur.pos();
        try {
            map.tiles.push_back(Tile::parse(cur, map_version, x, y));
        } catch (const std::exception& e) {
            adonai::log("[world] tile " + std::to_string(idx) + " (" + std::to_string(x) + "," +
                        std::to_string(y) + ") failed at pos " + std::to_string(pos_before) + ": " +
                        e.what());
            throw;
        }
    }
    cur.skip(12);  // trailer after tile array
    return map;
}

// §6.1 — top-level blob.
World World::parse(const std::uint8_t* data, std::size_t len) {
    Cursor cur(data, len, "world map blob");
    World w;
    w.version = cur.u16();
    if (w.version < MAP_VERSION_MIN) {
        throw std::runtime_error("map version " + hex_str(w.version) + " < minimum 0x19");
    }
    w.flags = cur.u32();
    w.tile_map = WorldTileMap::parse(cur, w.version);
    auto [objects, last_dropped_uid] = parse_world_objects(cur);
    w.objects = std::move(objects);
    w.base_weather = cur.u16();
    cur.skip(2);  // pad
    w.current_weather = cur.u16();
    w.next_object_uid = last_dropped_uid + 1;
    return w;
}

std::optional<World> World::try_parse(const std::uint8_t* data, std::size_t len) {
    try {
        return World::parse(data, len);
    } catch (const std::exception& e) {
        adonai::log(std::string("[world] World parse error: ") + e.what());
        return std::nullopt;
    }
}

const Tile* World::get_tile(std::uint32_t x, std::uint32_t y) const {
    std::size_t idx = tile_index(x, y, tile_map.width);
    if (idx < tile_map.tiles.size()) return &tile_map.tiles[idx];
    return nullptr;
}

Tile* World::get_tile_mut(std::uint32_t x, std::uint32_t y) {
    std::size_t idx = tile_index(x, y, tile_map.width);
    if (idx < tile_map.tiles.size()) return &tile_map.tiles[idx];
    return nullptr;
}

// §7 — incremental tile update.
std::optional<std::pair<std::uint16_t, std::uint16_t>> World::update_tile_from_bytes(
    std::uint32_t x, std::uint32_t y, const std::uint8_t* data, std::size_t len) {
    if (len < 4) return std::nullopt;
    std::uint16_t fg = le_u16(data + 0);
    std::uint16_t bg = le_u16(data + 2);
    Tile* tile = get_tile_mut(x, y);
    if (tile == nullptr) return std::nullopt;
    tile->fg_item_id = fg;
    tile->bg_item_id = bg;
    if (len >= 8) {
        std::uint16_t flags_raw = le_u16(data + 6);  // bytes [4..6] (parent) skipped
        tile->flags_raw = flags_raw;
        if ((flags_raw & static_cast<std::uint16_t>(HAS_EXTRA_DATA)) != 0 && len >= 9) {
            std::uint8_t kind = data[8];
            if (kind == 4 && len >= 14) {
                tiletype::Seed s;
                s.age = le_u32(data + 9);
                s.item_on_tree = data[13];
                tile->tile_type = s;
            } else {
                tile->tile_type = tiletype::Basic{};
            }
        } else if (fg == 0) {
            tile->tile_type = tiletype::Basic{};
        }
    } else if (fg == 0) {
        tile->tile_type = tiletype::Basic{};
    }
    return std::make_pair(fg, bg);
}

// §9.2 — collision type for a tile.
std::uint8_t tile_collision_type(const Tile& tile, const ItemsDat& items, bool item_collect) {
    if (std::holds_alternative<tiletype::Door>(tile.tile_type)) return 0;  // doors always passable
    if (item_collect && std::holds_alternative<tiletype::Lock>(tile.tile_type)) return 3;
    const ItemInfo* it = items.find_by_id(tile.fg_item_id);
    if (it != nullptr) return it->collision_type;
    return tile.fg_item_id == 0 ? 0 : 1;  // unknown non-empty block = solid
}

std::vector<std::pair<std::uint16_t, std::uint8_t>> build_collision_tiles(
    const World& world, const ItemsDat& items, bool item_collect) {
    std::vector<std::pair<std::uint16_t, std::uint8_t>> out;
    out.reserve(world.tile_map.tiles.size());
    for (const Tile& t : world.tile_map.tiles) {
        out.emplace_back(t.fg_item_id, tile_collision_type(t, items, item_collect));
    }
    return out;
}

// §9.3 — world access from Lock tiles.
bool world_has_access(const World& world, std::uint32_t user_id) {
    for (const Tile& t : world.tile_map.tiles) {
        bool is_lock_id = std::find(LOCK_ITEM_IDS.begin(), LOCK_ITEM_IDS.end(), t.fg_item_id) !=
                          LOCK_ITEM_IDS.end();
        if (!is_lock_id) continue;
        if (const auto* lock = std::get_if<tiletype::Lock>(&t.tile_type)) {
            if (std::find(lock->access_uids.begin(), lock->access_uids.end(), user_id) !=
                lock->access_uids.end()) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace adonai::world
