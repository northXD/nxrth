#include "world/items.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>  // GetModuleFileNameA — locate items.dat next to the exe

#include "core/cursor.h"
#include "core/logger.h"

namespace adonai::world {

Bgra extract_bgra(std::uint32_t color) {
    Bgra out;
    out.b = static_cast<std::uint8_t>((color >> 24) & 0xFF);
    out.g = static_cast<std::uint8_t>((color >> 16) & 0xFF);
    out.r = static_cast<std::uint8_t>((color >> 8) & 0xFF);
    out.a = static_cast<std::uint8_t>(color & 0xFF);
    return out;
}

std::uint32_t bgra_to_rgb(std::uint32_t color) {
    Bgra c = extract_bgra(color);
    return (static_cast<std::uint32_t>(c.r) << 16) |
           (static_cast<std::uint32_t>(c.g) << 8) |
           static_cast<std::uint32_t>(c.b);
}

namespace {

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string to_ascii_lower(std::string_view s) {
    std::string out(s.size(), '\0');
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = ascii_lower(s[i]);
    return out;
}

ItemInfo parse_item(Cursor& cur, std::uint16_t version) {
    ItemInfo it;
    it.id = cur.u32();
    it.flags = cur.u16();
    it.action_type = cur.u8();
    it.material = cur.u8();
    // Name XOR key start = item's own id mod 16.
    it.name = cur.xor_string(ITEMS_XOR_KEY, it.id % 16);
    it.texture_file_name = cur.plain_string();
    it.texture_hash = cur.u32();
    it.visual_effect = cur.u8();
    it.cooking_ingredient = cur.u32();
    it.texture_x = cur.u8();
    it.texture_y = cur.u8();
    it.render_type = cur.u8();
    it.is_stripey_wallpaper = cur.u8();
    it.collision_type = cur.u8();
    it.block_health = cur.u8();
    it.drop_chance = cur.u32();
    it.clothing_type = cur.u8();
    it.rarity = cur.u16();
    it.max_item = cur.u8();
    it.file_name = cur.plain_string();
    it.file_hash = cur.u32();
    it.audio_volume = cur.u32();
    it.pet_name = cur.plain_string();
    it.pet_prefix = cur.plain_string();
    it.pet_suffix = cur.plain_string();
    it.pet_ability = cur.plain_string();
    it.seed_base_sprite = cur.u8();
    it.seed_overlay_sprite = cur.u8();
    it.tree_base_sprite = cur.u8();
    it.tree_overlay_sprite = cur.u8();
    it.base_color = cur.u32();
    it.overlay_color = cur.u32();
    it.ingredient = cur.u32();
    it.grow_time = cur.u32();
    cur.skip(2);  // unused u16
    it.is_rayman = cur.u16();
    it.extra_options = cur.plain_string();
    it.texture_path_2 = cur.plain_string();
    it.extra_option2 = cur.plain_string();
    cur.skip(80);  // 80 unused bytes

    // Version-gated tail — cumulative independent gates. NOTE: no gate 20.
    if (version >= 11) it.punch_option = cur.plain_string();
    if (version >= 12) cur.skip(13);
    if (version >= 13) cur.skip(4);
    if (version >= 14) cur.skip(4);
    if (version >= 15) {
        cur.skip(25);
        cur.plain_string();  // discarded
    }
    if (version >= 16) cur.plain_string();  // discarded
    if (version >= 17) cur.skip(4);
    if (version >= 18) cur.skip(4);
    if (version >= 19) cur.skip(9);
    if (version >= 21) cur.skip(2);
    if (version >= 22) it.description = cur.plain_string();
    if (version >= 23) cur.skip(4);
    if (version >= 24) cur.skip(1);
    return it;
}

}  // namespace

ItemsDat ItemsDat::parse(const std::uint8_t* data, std::size_t len) {
    Cursor cur(data, len, "items.dat");
    ItemsDat db;
    db.version = cur.u16();
    std::uint32_t item_count = cur.u32();
    db.items.reserve(item_count);
    for (std::uint32_t i = 0; i < item_count; ++i) {
        db.items.push_back(parse_item(cur, db.version));
    }
    return db;
}

const ItemInfo* ItemsDat::find_by_id(std::uint32_t id) const {
    if (id < items.size() && items[id].id == id) return &items[id];
    auto it = std::find_if(items.begin(), items.end(),
                           [id](const ItemInfo& x) { return x.id == id; });
    return it == items.end() ? nullptr : &*it;
}

const ItemInfo* ItemsDat::find_by_name(std::string_view name) const {
    std::string want = to_ascii_lower(name);
    for (const ItemInfo& x : items) {
        if (to_ascii_lower(x.name) == want) return &x;
    }
    return nullptr;
}

ItemsDat ItemsDat::load() {
    // Search: CWD, ./data, then the executable's own dir + its data/ subdir, so
    // it works whether Adonai runs from the build dir or the source root.
    std::vector<std::string> candidates = {"items.dat", "data/items.dat"};
    char exe[MAX_PATH] = {0};
    const DWORD n = ::GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::string full(exe, n);
        const auto slash = full.find_last_of("\\/");
        if (slash != std::string::npos) {
            const std::string base = full.substr(0, slash + 1);
            candidates.push_back(base + "items.dat");
            candidates.push_back(base + "data\\items.dat");
        }
    }

    for (const auto& path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        try {
            std::vector<std::uint8_t> buf((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
            ItemsDat db = parse(buf);
            log("[Items] Loaded " + std::to_string(db.items.size()) + " items (v" +
                std::to_string(db.version) + ") from " + path);
            return db;
        } catch (const std::exception& e) {
            log(std::string("[Items] Parse failed for ") + path + ": " + e.what());
        }
    }
    log("[Items] items.dat not found (CWD / data / exe-dir) — item names limited");
    return ItemsDat{};
}

}  // namespace adonai::world
