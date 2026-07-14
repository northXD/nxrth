#include "world/save_dat.h"

#include <cstring>
#include <stdexcept>
#include <type_traits>

#include "core/cursor.h"

namespace adonai::world {

std::vector<std::uint8_t> xor_90210(const std::vector<std::uint8_t>& data) {
    static constexpr std::uint8_t KEY[5] = {'9', '0', '2', '1', '0'};
    std::vector<std::uint8_t> out(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(data[i] ^ KEY[i % 5]);
    }
    return out;
}

std::uint32_t variant_type_id(const VariantValue& v) {
    switch (v.index()) {
        case 0: return 1;  // VFloat
        case 1: return 2;  // VString
        case 2: return 3;  // VVec2
        case 3: return 4;  // VVec3
        case 4: return 5;  // VUint
        case 5: return 8;  // VRect
        case 6: return 9;  // VInt
    }
    return 0;
}

// --- Seed diary -------------------------------------------------------------

SeedDiary SeedDiary::parse(const std::vector<std::uint8_t>& data) {
    SeedDiary d;
    std::size_t i = 0;
    while (i + 1 < data.size()) {  // requires 2 available bytes
        std::uint8_t lo = data[i];
        std::uint8_t hi = data[i + 1];
        std::uint16_t item_id =
            static_cast<std::uint16_t>(lo | ((hi & 0x7F) << 8));
        if (item_id <= SEED_DIARY_MAX_ID) {
            d.have.insert(item_id);
            if ((hi & 0x80) != 0) d.grown.insert(item_id);
        }
        i += 2;
    }
    return d;
}

std::vector<std::uint8_t> SeedDiary::serialize() const {
    std::vector<std::uint8_t> out;
    for (std::uint32_t id = 0; id <= SEED_DIARY_MAX_ID; ++id) {
        auto uid = static_cast<std::uint16_t>(id);
        if (have.count(uid)) {
            std::uint8_t lo = static_cast<std::uint8_t>(id & 0xFF);
            std::uint8_t hi = static_cast<std::uint8_t>((id >> 8) & 0x7F);
            if (grown.count(uid)) hi |= 0x80;
            out.push_back(lo);
            out.push_back(hi);
        }
    }
    return out;
}

// --- SaveDat ----------------------------------------------------------------

void SaveDat::set(const std::string& key, VariantValue value) {
    for (Entry& e : entries) {
        if (e.key == key) {
            e.value = std::move(value);
            return;
        }
    }
    entries.push_back(Entry{key, std::move(value)});
}

const VariantValue* SaveDat::get(const std::string& key) const {
    for (const Entry& e : entries) {
        if (e.key == key) return &e.value;
    }
    return nullptr;
}

std::optional<std::vector<std::uint8_t>> SaveDat::get_meta() const {
    const VariantValue* v = get("meta");
    if (v) {
        if (const VString* s = std::get_if<VString>(v)) {
            return xor_90210(s->v);
        }
    }
    return std::nullopt;
}

void SaveDat::set_meta(const std::vector<std::uint8_t>& plain) {
    set("meta", VString{xor_90210(plain)});
}

std::optional<SeedDiary> SaveDat::get_seed_diary() const {
    const VariantValue* v = get("seed_diary_data");
    if (v) {
        if (const VString* s = std::get_if<VString>(v)) {
            return SeedDiary::parse(s->v);
        }
    }
    return std::nullopt;
}

void SaveDat::set_seed_diary(const SeedDiary& diary) {
    set("seed_diary_data", VString{diary.serialize()});
}

namespace {

void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

void put_i32(std::vector<std::uint8_t>& b, std::int32_t v) {
    put_u32(b, static_cast<std::uint32_t>(v));
}

void put_f32(std::vector<std::uint8_t>& b, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put_u32(b, bits);
}

}  // namespace

std::vector<std::uint8_t> SaveDat::serialize() const {
    std::vector<std::uint8_t> b;
    put_u32(b, 1);  // magic / version
    for (const Entry& e : entries) {
        put_u32(b, variant_type_id(e.value));
        put_u32(b, static_cast<std::uint32_t>(e.key.size()));
        b.insert(b.end(), e.key.begin(), e.key.end());
        std::visit(
            [&b](const auto& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, VFloat>) {
                    put_f32(b, val.v);
                } else if constexpr (std::is_same_v<T, VString>) {
                    put_u32(b, static_cast<std::uint32_t>(val.v.size()));
                    b.insert(b.end(), val.v.begin(), val.v.end());
                } else if constexpr (std::is_same_v<T, VVec2>) {
                    put_f32(b, val.x);
                    put_f32(b, val.y);
                } else if constexpr (std::is_same_v<T, VVec3>) {
                    put_f32(b, val.x);
                    put_f32(b, val.y);
                    put_f32(b, val.z);
                } else if constexpr (std::is_same_v<T, VUint>) {
                    put_u32(b, val.v);
                } else if constexpr (std::is_same_v<T, VRect>) {
                    put_f32(b, val.x);
                    put_f32(b, val.y);
                    put_f32(b, val.w);
                    put_f32(b, val.h);
                } else if constexpr (std::is_same_v<T, VInt>) {
                    put_i32(b, val.v);
                }
            },
            e.value);
    }
    put_u32(b, 0);  // terminator
    return b;
}

SaveDat SaveDat::parse(const std::uint8_t* data, std::size_t len) {
    Cursor cur(data, len, "save.dat");
    std::uint32_t magic = cur.u32();
    if (magic != 1) {
        throw std::runtime_error("unexpected magic: " + std::to_string(magic));
    }
    SaveDat sd;
    while (cur.remaining() > 0) {
        std::uint32_t type_id = cur.u32();
        if (type_id == 0) break;  // terminator
        std::uint32_t key_len = cur.u32();
        std::string key = cur.string_raw(key_len);
        VariantValue value;
        switch (type_id) {
            case 1: value = VFloat{cur.f32()}; break;
            case 2: {
                std::uint32_t l = cur.u32();
                value = VString{cur.bytes(l)};
                break;
            }
            case 3: value = VVec2{cur.f32(), cur.f32()}; break;
            case 4: value = VVec3{cur.f32(), cur.f32(), cur.f32()}; break;
            case 5: value = VUint{cur.u32()}; break;
            case 8: value = VRect{cur.f32(), cur.f32(), cur.f32(), cur.f32()}; break;
            case 9: value = VInt{cur.i32()}; break;
            default:
                throw std::runtime_error("unknown variant type " +
                                         std::to_string(type_id) + " for key " +
                                         key);
        }
        sd.entries.push_back(Entry{std::move(key), std::move(value)});
    }
    return sd;
}

}  // namespace adonai::world
