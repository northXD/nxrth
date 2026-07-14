// Adonai — per-bot inventory model (ported from Mori/inventory.rs).
// Parses the inventory blob the server sends and maintains live inventory state.
// Per-bot mutable state, guarded by the owning bot's external lock.
#pragma once
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace adonai::world {

// Items removed on world-leave: World Key (1424), Magplant 5000 Remote (5640).
inline constexpr std::array<std::uint16_t, 2> TEMPORARY_ITEM_IDS = {1424, 5640};

struct InventoryItem {
    std::uint16_t id = 0;
    std::uint8_t amount = 0;
    std::uint8_t flag = 0;
};

struct Inventory {
    std::uint32_t size = 0;        // total capacity (slots)
    std::uint16_t item_count = 0;  // number of distinct stacks
    std::unordered_map<std::uint16_t, InventoryItem> items;
    std::int32_t gems = 0;         // NOT parsed from wire; default 0

    // Parse the wire blob. Throws std::runtime_error on truncation.
    static Inventory parse(const std::uint8_t* data, std::size_t len);
    static Inventory parse(const std::vector<std::uint8_t>& data) {
        return parse(data.data(), data.size());
    }

    void clear();  // resets size/item_count/items (leaves gems)
    void add_item(std::uint16_t id, std::uint8_t amount);   // u8 saturating add
    void add_gems(std::int32_t amount);                     // ASSIGNS gems
    bool has_item(std::uint16_t id, std::uint8_t min_amount) const;
    bool is_active(std::uint16_t id) const;
    void set_active(std::uint16_t id, bool active);
    bool can_collect(std::uint16_t item_id) const;
    void sub_item(std::uint16_t id, std::uint8_t amount);
    void remove_item(std::uint16_t id);
    bool remove_temp_items();  // true iff at least one temp item was removed
};

}  // namespace adonai::world
