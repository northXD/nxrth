#include "world/inventory.h"

#include <algorithm>

#include "core/cursor.h"

namespace adonai::world {

Inventory Inventory::parse(const std::uint8_t* data, std::size_t len) {
    Cursor cur(data, len, "inventory");
    Inventory inv;
    cur.skip(1);  // one unknown leading byte
    inv.size = cur.u32();
    inv.item_count = cur.u16();
    for (std::uint16_t i = 0; i < inv.item_count; ++i) {
        InventoryItem it;
        it.id = cur.u16();
        it.amount = cur.u8();
        it.flag = cur.u8();
        inv.items[it.id] = it;  // duplicate ids overwrite
    }
    inv.gems = 0;
    return inv;
}

void Inventory::clear() {
    size = 0;
    item_count = 0;
    items.clear();
}

void Inventory::add_item(std::uint16_t id, std::uint8_t amount) {
    auto it = items.find(id);
    if (it != items.end()) {
        std::uint16_t sum = static_cast<std::uint16_t>(it->second.amount) + amount;
        it->second.amount = static_cast<std::uint8_t>(sum > 255 ? 255 : sum);
    } else {
        items[id] = InventoryItem{id, amount, 0};
        ++item_count;
    }
}

void Inventory::add_gems(std::int32_t amount) {
    gems = amount;  // assignment despite the name
}

bool Inventory::has_item(std::uint16_t id, std::uint8_t min_amount) const {
    auto it = items.find(id);
    return it != items.end() && it->second.amount >= min_amount;
}

bool Inventory::is_active(std::uint16_t id) const {
    auto it = items.find(id);
    return it != items.end() && (it->second.flag & 1) != 0;
}

void Inventory::set_active(std::uint16_t id, bool active) {
    auto it = items.find(id);
    if (it == items.end()) return;
    if (active) {
        it->second.flag |= 1;
    } else {
        it->second.flag &= static_cast<std::uint8_t>(~1);
    }
}

bool Inventory::can_collect(std::uint16_t item_id) const {
    if (item_id == 112) return true;  // gems always fit
    auto it = items.find(item_id);
    if (it != items.end()) return it->second.amount < 200;  // stack cap 200
    return static_cast<std::uint32_t>(item_count) < size;
}

void Inventory::sub_item(std::uint16_t id, std::uint8_t amount) {
    auto it = items.find(id);
    if (it == items.end()) return;
    if (it->second.amount <= amount) {
        items.erase(it);
        if (item_count > 0) --item_count;  // u16 saturating sub
    } else {
        it->second.amount -= amount;
    }
}

void Inventory::remove_item(std::uint16_t id) {
    auto it = items.find(id);
    if (it != items.end()) {
        items.erase(it);
        if (item_count > 0) --item_count;  // u16 saturating sub
    }
}

bool Inventory::remove_temp_items() {
    std::vector<std::uint16_t> to_remove;
    for (std::uint16_t tid : TEMPORARY_ITEM_IDS) {
        if (items.find(tid) != items.end()) to_remove.push_back(tid);
    }
    for (std::uint16_t tid : to_remove) remove_item(tid);
    return !to_remove.empty();
}

}  // namespace adonai::world
