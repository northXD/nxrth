// Nxrth — Growtopia items.dat binary parser (ported from Nxrth/items.rs).
// Read-only item metadata DB, loaded once at startup and shared fleet-wide as
// std::shared_ptr<const ItemsDat>. GT is little-endian throughout.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxrth::world {

// items.dat name-decryption XOR key: "PBG892FXX982ABC*" (16 bytes).
inline constexpr std::string_view ITEMS_XOR_KEY = "PBG892FXX982ABC*";

// One item's metadata. Field order below is also the on-disk fixed read order.
// All fields default to zero / empty string.
struct ItemInfo {
    std::uint32_t id = 0;
    std::uint16_t flags = 0;
    std::uint8_t action_type = 0;
    std::uint8_t material = 0;
    std::string name;                 // XOR-decoded
    std::string texture_file_name;    // plain_string
    std::uint32_t texture_hash = 0;
    std::uint8_t visual_effect = 0;
    std::uint32_t cooking_ingredient = 0;
    std::uint8_t texture_x = 0;
    std::uint8_t texture_y = 0;
    std::uint8_t render_type = 0;
    std::uint8_t is_stripey_wallpaper = 0;
    std::uint8_t collision_type = 0;
    std::uint8_t block_health = 0;
    std::uint32_t drop_chance = 0;
    std::uint8_t clothing_type = 0;
    std::uint16_t rarity = 0;
    std::uint8_t max_item = 0;
    std::string file_name;            // plain_string
    std::uint32_t file_hash = 0;
    std::uint32_t audio_volume = 0;
    std::string pet_name;             // plain_string
    std::string pet_prefix;           // plain_string
    std::string pet_suffix;           // plain_string
    std::string pet_ability;          // plain_string
    std::uint8_t seed_base_sprite = 0;
    std::uint8_t seed_overlay_sprite = 0;
    std::uint8_t tree_base_sprite = 0;
    std::uint8_t tree_overlay_sprite = 0;
    std::uint32_t base_color = 0;     // BGRA-packed
    std::uint32_t overlay_color = 0;  // BGRA-packed
    std::uint32_t ingredient = 0;
    std::uint32_t grow_time = 0;
    std::uint16_t is_rayman = 0;      // (preceded by a skipped unused u16)
    std::string extra_options;        // plain_string
    std::string texture_path_2;       // plain_string
    std::string extra_option2;        // plain_string
    // version-gated:
    std::string punch_option;         // v>=11
    std::string description;          // v>=22
};

// Color helpers — items.dat packs colors as BGRA (MSB->LSB: B,G,R,A).
struct Bgra {
    std::uint8_t b, g, r, a;
};
Bgra extract_bgra(std::uint32_t color);        // returns (b, g, r, a)
std::uint32_t bgra_to_rgb(std::uint32_t color); // -> 0x00RRGGBB, alpha dropped

struct ItemsDat {
    std::uint16_t version = 0;
    std::vector<ItemInfo> items;

    // Parse a full items.dat buffer. Throws std::runtime_error on truncation.
    static ItemsDat parse(const std::uint8_t* data, std::size_t len);
    static ItemsDat parse(const std::vector<std::uint8_t>& data) {
        return parse(data.data(), data.size());
    }

    // Fast path: items[id] if it exists and its .id == id; else linear scan.
    const ItemInfo* find_by_id(std::uint32_t id) const;
    // Locale-independent (ASCII) case-insensitive name match.
    const ItemInfo* find_by_name(std::string_view name) const;

    // Read "items.dat" from CWD; never throws. On error returns {0,{}} and logs.
    static ItemsDat load();
};

}  // namespace nxrth::world
