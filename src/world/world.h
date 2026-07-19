// Nxrth — world map model + SendMapData byte parser (ported from Mori/world/mod_impl.rs).
// Byte-exact reimplementation of the SendMapData blob (tiles, per-tile extra
// data, ground objects) plus incremental tile updates and coordinate helpers.
// GT is little-endian throughout; strings are UTF-8 (lossy). Per-bot state,
// touched only by the owning bot's thread.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/cursor.h"
#include "world/items.h"

namespace nxrth::world {

// --- Constants (world/constants.rs) -----------------------------------------
inline constexpr std::uint16_t MAP_VERSION_MIN = 0x19;       // 25
inline constexpr std::uint32_t MAX_TILE_COUNT = 65026;       // tile_count must be <
inline constexpr std::uint32_t MAX_WORLD_OBJECTS = 0x493E1;  // 300001; count must be <

// Tiles whose fg_item_id is in this set carry a trailing CBOR blob after their
// extra data (parser only reads a u32 size and skips that many bytes).
bool is_cbor_tile_id(std::uint16_t fg_item_id);

// --- TileFlags — raw u16 bitset (do not renumber) ---------------------------
enum TileFlag : std::uint16_t {
    HAS_EXTRA_DATA       = 0x0001,
    HAS_PARENT           = 0x0002,
    WAS_SPLICED          = 0x0004,
    WILL_SPAWN_SEEDS_TOO = 0x0008,
    IS_SEEDLING          = 0x0010,
    FLIPPED_X            = 0x0020,
    IS_ON                = 0x0040,
    IS_OPEN_TO_PUBLIC    = 0x0080,
    BG_IS_ON             = 0x0100,
    FG_ALT_MODE          = 0x0200,
    IS_WET               = 0x0400,
    GLUED                = 0x0800,
    ON_FIRE              = 0x1000,
    PAINTED_RED          = 0x2000,
    PAINTED_GREEN        = 0x4000,
    PAINTED_BLUE         = 0x8000,
};

// --- TileType — per-tile extra data (std::variant, default Basic) -----------
namespace tiletype {

struct Basic {};
struct Door { std::string label; std::uint8_t flags = 0; };
struct Sign { std::string label; };
struct Lock {
    std::uint8_t settings = 0;
    std::uint32_t owner_uid = 0;
    std::uint32_t access_count = 0;
    std::vector<std::uint32_t> access_uids;
    std::uint8_t minimum_level = 0;
};
struct Seed { std::uint32_t age = 0; std::uint8_t item_on_tree = 0; };
struct Dice { std::uint8_t symbol = 0; };
struct Provider { std::uint32_t age = 0; };
struct AchievementBlock { std::uint32_t data = 0; std::uint8_t tile_type = 0; };
struct HearthMonitor { std::uint32_t player_id = 0; std::string player_name; };
struct Mannequin {
    std::string label;
    std::uint8_t unknown_1 = 0;
    std::uint16_t unknown_2 = 0, unknown_3 = 0;
    std::uint16_t hat = 0, shirt = 0, pants = 0, boots = 0, face = 0, hand = 0,
                  back = 0, hair = 0, neck = 0;
};
struct BunnyEgg { std::uint32_t egg_placed = 0; };
struct GameGrave { std::uint8_t team = 0; };
struct GameGenerator {};
struct XenoniteCrystal { std::uint8_t unknown_1 = 0; std::uint32_t unknown_2 = 0; };
struct PhoneBooth {
    std::uint16_t hat = 0, shirt = 0, pants = 0, shoes = 0, face = 0, hand = 0,
                  back = 0, hair = 0, neck = 0;
};
struct Crystal { std::vector<std::uint8_t> crystals; };
struct CrimeInProgress { std::string label; std::uint32_t unknown_1 = 0; std::uint8_t unknown_2 = 0; };
struct Spotlight {};
struct DisplayBlock { std::uint32_t item_id = 0; };
struct VendingMachine { std::uint32_t item_id = 0; std::int32_t price = 0; };
struct FishTankPort {
    std::uint8_t flags = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> fishes;  // (item_id, lbs)
};
struct SolarCollector { std::array<std::uint8_t, 5> data{}; };
struct Forge { std::uint32_t temperature = 0; };
struct GivingTree {
    std::uint8_t harvested = 0;
    std::uint16_t age = 0, unknown_1 = 0;
    std::uint8_t decoration_percentage = 0;
};
struct SteamOrgan { std::uint8_t instrument_type = 0; std::uint32_t note = 0; };
struct SilkWorm {
    std::uint8_t flags = 0;
    std::string name;
    std::uint32_t age = 0, unknown_1 = 0, unknown_2 = 0;
    std::uint8_t can_be_fed = 0;
    std::uint32_t food_saturation = 0, water_saturation = 0, color = 0, sick_duration = 0;
};
struct SewingMachine { std::vector<std::uint32_t> bolt_ids; };
struct CountryFlag { std::string country; };
struct LobsterTrap {};
struct PaintingEasel { std::uint32_t item_id = 0; std::string label; };
struct PetBattleCage {
    std::string label;
    std::uint32_t base_pet = 0, pet_1 = 0, pet_2 = 0;
};
struct PetTrainer {
    std::string label;
    std::uint32_t pet_total_count = 0, unknown_1 = 0;
    std::vector<std::uint32_t> pets_id;
};
struct SteamEngine { std::uint32_t temperature = 0; };
struct LockBot { std::uint32_t age = 0; };
struct WeatherMachine { std::uint32_t settings = 0; };
struct SpiritStorageUnit { std::uint32_t ghost_jar_count = 0; };
struct DataBedrock {};
struct Shelf {
    std::uint32_t top_left_item_id = 0, top_right_item_id = 0,
                  bottom_left_item_id = 0, bottom_right_item_id = 0;
};
struct VipEntrance {
    std::uint8_t unknown_1 = 0;
    std::uint32_t owner_uid = 0;
    std::vector<std::uint32_t> access_uids;
};
struct ChallangeTimer {};  // (sic — spelled "Challange" in source)
struct FishWallMount { std::string label; std::uint32_t item_id = 0; std::uint8_t lb = 0; };
struct Portrait {
    std::string label;
    std::uint32_t unknown_1 = 0, unknown_2 = 0;
    std::array<std::uint8_t, 5> unknown_3{};
    std::uint8_t unknown_4 = 0;
    std::uint16_t unknown_5 = 0, face = 0, hat = 0, hair = 0;
    std::uint32_t unknown_6 = 0;
    std::optional<std::string> infinity_crown_data;
};
struct WeatherMachine2 { std::uint32_t unknown_1 = 0, gravity = 0; std::uint8_t flags = 0; };
struct FossilPrepStation { std::uint32_t unknown_1 = 0; };
struct DnaExtractor {};
struct Howler {};
struct ChemsynthTank { std::uint32_t current_chem = 0, target_chem = 0; };
struct StorageBlock {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> items;  // (id, amount)
};
struct CookingOven {
    std::uint32_t temperature_level = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ingredients;  // (item_id, time_added)
    std::uint32_t unknown_1 = 0, unknown_2 = 0, unknown_3 = 0;
};
struct AudioRack { std::string note; std::uint32_t volume = 0; };
struct GeigerCharger { std::uint32_t unknown_1 = 0; };
struct AdventureBegins {};
struct TombRobber {};
struct BalloonOMatic { std::uint32_t total_rarity = 0; std::uint8_t team_type = 0; };
struct TrainingPort {
    std::uint32_t fish_lb = 0;
    std::uint16_t fish_status = 0;
    std::uint32_t fish_id = 0, fish_total_exp = 0;
    std::array<std::uint8_t, 8> unknown_1{};
    std::uint32_t fish_level = 0, unknown_2 = 0;
    std::array<std::uint8_t, 5> unknown_3{};
};
struct ItemSucker {
    std::uint32_t item_id_to_suck = 0, item_amount = 0;
    std::uint16_t flags = 0;
    std::uint32_t limit = 0;
};
struct CyBot {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> command_datas;  // (command_id, is_command_used)
    std::uint32_t sync_timer = 0, activated = 0;
};
struct GuildItem { std::array<std::uint8_t, 17> unknown_1{}; };
struct Growscan { std::uint8_t unknown_1 = 0; };
struct ContainmentFieldPowerNode {
    std::uint32_t time = 0;
    std::vector<std::uint32_t> linked_nodes;
};
struct SpiritBoard {
    std::uint32_t player_required = 0;
    std::string unk1, command;
    std::vector<std::uint32_t> required_items;
};
struct TesseractManipulator {
    std::uint32_t gems = 0, next_update_ms = 0, item_id = 0, enabled = 0;
};
struct StormyCloud { std::uint32_t sting_duration = 0, is_solid = 0, non_solid_duration = 0; };
struct TemporaryPlatform { std::uint32_t unknown_1 = 0; };
struct SafeVault {};
struct AngelicCountingCloud {
    std::uint32_t state = 0;
    std::uint16_t unknown_1 = 0;
    std::optional<std::uint8_t> ascii_code;
};
struct InfinityWeatherMachine {
    std::uint32_t interval_minutes = 0;
    std::vector<std::uint32_t> weather_machine_list;
};
struct PineappleGuzzler { std::uint32_t pineapple_count = 0; };
struct KrakenGalaticBlock {
    std::uint8_t pattern_index = 0;
    std::uint32_t unknown_1 = 0;
    std::uint8_t r = 0, g = 0, b = 0;
};
struct FriendsEntrance {
    std::uint32_t owner_user_id = 0;
    std::vector<std::uint32_t> allowed_friends_userid;
};
struct Unknown { std::uint8_t kind = 0; };  // fallback for unrecognized kind

}  // namespace tiletype

using TileType = std::variant<
    tiletype::Basic, tiletype::Door, tiletype::Sign, tiletype::Lock, tiletype::Seed,
    tiletype::Dice, tiletype::Provider, tiletype::AchievementBlock, tiletype::HearthMonitor,
    tiletype::Mannequin, tiletype::BunnyEgg, tiletype::GameGrave, tiletype::GameGenerator,
    tiletype::XenoniteCrystal, tiletype::PhoneBooth, tiletype::Crystal, tiletype::CrimeInProgress,
    tiletype::Spotlight, tiletype::DisplayBlock, tiletype::VendingMachine, tiletype::FishTankPort,
    tiletype::SolarCollector, tiletype::Forge, tiletype::GivingTree, tiletype::SteamOrgan,
    tiletype::SilkWorm, tiletype::SewingMachine, tiletype::CountryFlag, tiletype::LobsterTrap,
    tiletype::PaintingEasel, tiletype::PetBattleCage, tiletype::PetTrainer, tiletype::SteamEngine,
    tiletype::LockBot, tiletype::WeatherMachine, tiletype::SpiritStorageUnit, tiletype::DataBedrock,
    tiletype::Shelf, tiletype::VipEntrance, tiletype::ChallangeTimer, tiletype::FishWallMount,
    tiletype::Portrait, tiletype::WeatherMachine2, tiletype::FossilPrepStation, tiletype::DnaExtractor,
    tiletype::Howler, tiletype::ChemsynthTank, tiletype::StorageBlock, tiletype::CookingOven,
    tiletype::AudioRack, tiletype::GeigerCharger, tiletype::AdventureBegins, tiletype::TombRobber,
    tiletype::BalloonOMatic, tiletype::TrainingPort, tiletype::ItemSucker, tiletype::CyBot,
    tiletype::GuildItem, tiletype::Growscan, tiletype::ContainmentFieldPowerNode, tiletype::SpiritBoard,
    tiletype::TesseractManipulator, tiletype::StormyCloud, tiletype::TemporaryPlatform, tiletype::SafeVault,
    tiletype::AngelicCountingCloud, tiletype::InfinityWeatherMachine, tiletype::PineappleGuzzler,
    tiletype::KrakenGalaticBlock, tiletype::FriendsEntrance, tiletype::Unknown>;

// --- Tile -------------------------------------------------------------------
struct Tile {
    std::uint16_t fg_item_id = 0;    // foreground block item id (0 = empty)
    std::uint16_t bg_item_id = 0;    // background item id
    std::uint16_t parent_block = 0;  // parent block field (raw)
    std::uint16_t flags_raw = 0;     // raw flags word (retain undefined bits)
    std::uint32_t x = 0;             // tile column (filled by the map loop)
    std::uint32_t y = 0;             // tile row    (filled by the map loop)
    TileType tile_type;              // decoded extra data (default Basic)

    bool has(TileFlag f) const { return (flags_raw & static_cast<std::uint16_t>(f)) != 0; }

    // Parse one tile from the cursor. x/y are supplied by the caller (not read).
    static Tile parse(Cursor& cur, std::uint16_t map_version, std::uint32_t x, std::uint32_t y);
};

// --- WorldTileMap -----------------------------------------------------------
struct WorldTileMap {
    std::string world_name;      // UTF-8 (lossy) length-prefixed name
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Tile> tiles;     // row-major: index = y*width + x

    // Parse the tile array. Sets out_truncated=true if it stopped early on an
    // unsupported tile-extra block (tail padded with empty tiles; caller must then
    // skip the trailer + object stream, which is desynced from that point).
    static WorldTileMap parse(Cursor& cur, std::uint16_t map_version, bool& out_truncated);
};

// --- WorldObject (dropped ground item) --------------------------------------
struct WorldObject {
    std::uint16_t item_id = 0;   // 5996 and 1626 are remapped to 0 at parse time
    float x = 0.0f;              // pixel coordinate (world space)
    float y = 0.0f;             // pixel coordinate
    std::uint8_t count = 0;      // stack size
    std::uint8_t flags = 0;
    std::uint32_t uid = 0;       // object unique id
};

// --- World ------------------------------------------------------------------
struct World {
    std::uint16_t version = 0;         // map format version (>= MAP_VERSION_MIN)
    std::uint32_t flags = 0;           // world flags bitfield (opaque)
    WorldTileMap tile_map;
    std::vector<WorldObject> objects;  // dropped items on the ground
    std::uint32_t next_object_uid = 0; // = last_dropped_uid + 1
    std::uint16_t base_weather = 0;
    std::uint16_t current_weather = 0;

    // Parse the full SendMapData extra_data payload. Throws std::runtime_error
    // (verbatim messages) on version/count guard failure or cursor truncation.
    static World parse(const std::uint8_t* data, std::size_t len);
    static World parse(const std::vector<std::uint8_t>& data) {
        return parse(data.data(), data.size());
    }
    // Non-throwing wrapper: logs "[world] World parse error: ..." and returns
    // std::nullopt on failure.
    static std::optional<World> try_parse(const std::uint8_t* data, std::size_t len);

    const Tile* get_tile(std::uint32_t x, std::uint32_t y) const;
    Tile* get_tile_mut(std::uint32_t x, std::uint32_t y);

    // Apply a raw SendTileUpdateData blob. Returns the new (fg, bg) on success,
    // or std::nullopt if the blob is too short or the tile is out of range.
    std::optional<std::pair<std::uint16_t, std::uint16_t>> update_tile_from_bytes(
        std::uint32_t x, std::uint32_t y, const std::uint8_t* data, std::size_t len);
    std::optional<std::pair<std::uint16_t, std::uint16_t>> update_tile_from_bytes(
        std::uint32_t x, std::uint32_t y, const std::vector<std::uint8_t>& data) {
        return update_tile_from_bytes(x, y, data.data(), data.size());
    }
};

// --- Coordinate helpers (§9.1) ----------------------------------------------
inline constexpr float TILE_SIZE = 32.0f;

// Pixel -> tile, truncating toward zero (pathfinding endpoints).
inline std::uint32_t pixel_to_tile(float pixel) {
    return static_cast<std::uint32_t>(pixel / TILE_SIZE);
}
// Pixel -> tile, flooring (build/place offset math).
inline std::int32_t pixel_to_tile_floor(float pixel) {
    return static_cast<std::int32_t>(std::floor(pixel / TILE_SIZE));
}
// Tile -> pixel.
inline float tile_to_pixel(std::uint32_t tile) {
    return static_cast<float>(tile) * TILE_SIZE;
}
// Linear tile index (computed in size_t to avoid overflow).
inline std::size_t tile_index(std::uint32_t x, std::uint32_t y, std::uint32_t width) {
    return static_cast<std::size_t>(y) * width + x;
}
// Collect radius: object at (ox,oy) "near" (px,py) within clamp(radius,1,5) tiles.
inline bool object_near(float px, float py, float ox, float oy, std::uint32_t radius_tiles) {
    std::uint32_t r = radius_tiles < 1 ? 1 : (radius_tiles > 5 ? 5 : radius_tiles);
    float r_px = static_cast<float>(r) * TILE_SIZE;
    return std::fabs(px - ox) <= r_px && std::fabs(py - oy) <= r_px;
}

// --- Collision-grid / access helpers (§9.2 / §9.3) --------------------------
// Lock item ids that gate world access.
inline constexpr std::array<std::uint16_t, 5> LOCK_ITEM_IDS = {242, 1796, 2408, 7188, 10410};

// Per-tile collision type used to build the A* grid.
//  item_collect == true reproduces the item-collection path (Lock -> 3, Door -> 0);
//  item_collect == false reproduces the general find_path path (Door -> 0 only,
//  Lock falls through to the items-db lookup).
std::uint8_t tile_collision_type(const Tile& tile, const ItemsDat& items, bool item_collect);

// Build the row-major (fg_item_id, collision_type) grid for AStar::update_from_tiles.
std::vector<std::pair<std::uint16_t, std::uint8_t>> build_collision_tiles(
    const World& world, const ItemsDat& items, bool item_collect);

// True if any Lock tile (with a LOCK_ITEM_IDS fg) grants this user access.
bool world_has_access(const World& world, std::uint32_t user_id);

}  // namespace nxrth::world
