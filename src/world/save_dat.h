// Adonai — save.dat / variant-list key/value store (ported from Mori/save_dat.rs).
// Round-trippable: parse -> serialize is byte-stable given the same entry order.
// Per-bot state; guarded by the owning bot's external lock. GT is little-endian.
#pragma once
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace adonai::world {

// XOR helper with key "90210" (self-inverse: encode == decode). Applied to meta.
std::vector<std::uint8_t> xor_90210(const std::vector<std::uint8_t>& data);

// Tagged union of variant values. Wire type IDs annotated; 6 and 7 unsupported.
struct VFloat { float v; };                          // type_id 1
struct VString { std::vector<std::uint8_t> v; };     // type_id 2 (RAW bytes)
struct VVec2 { float x, y; };                        // type_id 3
struct VVec3 { float x, y, z; };                     // type_id 4
struct VUint { std::uint32_t v; };                   // type_id 5
struct VRect { float x, y, w, h; };                  // type_id 8
struct VInt { std::int32_t v; };                     // type_id 9

using VariantValue =
    std::variant<VFloat, VString, VVec2, VVec3, VUint, VRect, VInt>;

// Wire type id for a value (1/2/3/4/5/8/9).
std::uint32_t variant_type_id(const VariantValue& v);

struct Entry {
    std::string key;
    VariantValue value;
};

// Seed diary: packed 16-bit LE entries; bits[14:0]=item_id, bit[15]=grown flag.
inline constexpr std::uint16_t SEED_DIARY_MAX_ID = 16010;  // 0x3E8A

struct SeedDiary {
    std::set<std::uint16_t> have;
    std::set<std::uint16_t> grown;

    static SeedDiary parse(const std::vector<std::uint8_t>& data);
    std::vector<std::uint8_t> serialize() const;
};

struct SaveDat {
    std::vector<Entry> entries;

    // If key exists, update value in place (position unchanged); else append.
    void set(const std::string& key, VariantValue value);
    const VariantValue* get(const std::string& key) const;

    std::optional<std::vector<std::uint8_t>> get_meta() const;
    void set_meta(const std::vector<std::uint8_t>& plain);
    std::optional<SeedDiary> get_seed_diary() const;
    void set_seed_diary(const SeedDiary& diary);

    std::vector<std::uint8_t> serialize() const;
    // Throws std::runtime_error on bad magic / unknown variant type.
    static SaveDat parse(const std::uint8_t* data, std::size_t len);
    static SaveDat parse(const std::vector<std::uint8_t>& data) {
        return parse(data.data(), data.size());
    }
};

}  // namespace adonai::world
