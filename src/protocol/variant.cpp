#include "protocol/variant.h"

#include <charconv>
#include <exception>

#include "core/cursor.h"

namespace nxrth::protocol {

namespace {

// Shortest round-trip decimal, matching Rust's f32 Display (e.g. 1.0 -> "1").
std::string float_to_string(float f) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), f);
    return std::string(buf, res.ptr);
}

void push_fffd(std::string& out) {
    out.push_back(static_cast<char>(0xEF));
    out.push_back(static_cast<char>(0xBF));
    out.push_back(static_cast<char>(0xBD));
}

// Mirror Rust String::from_utf8_lossy: emit one U+FFFD per maximal invalid
// subpart; never fails. GT strings are ASCII/UTF-8 in practice.
std::string utf8_lossy(const std::vector<std::uint8_t>& b) {
    std::string out;
    out.reserve(b.size());
    std::size_t i = 0;
    const std::size_t n = b.size();
    while (i < n) {
        const std::uint8_t b0 = b[i];
        if (b0 < 0x80) {
            out.push_back(static_cast<char>(b0));
            ++i;
            continue;
        }
        int extra;
        std::uint8_t lo1, hi1;  // valid range for the FIRST continuation byte
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            extra = 1;
            lo1 = 0x80;
            hi1 = 0xBF;
        } else if (b0 == 0xE0) {
            extra = 2;
            lo1 = 0xA0;
            hi1 = 0xBF;
        } else if (b0 >= 0xE1 && b0 <= 0xEC) {
            extra = 2;
            lo1 = 0x80;
            hi1 = 0xBF;
        } else if (b0 == 0xED) {
            extra = 2;
            lo1 = 0x80;
            hi1 = 0x9F;
        } else if (b0 >= 0xEE && b0 <= 0xEF) {
            extra = 2;
            lo1 = 0x80;
            hi1 = 0xBF;
        } else if (b0 == 0xF0) {
            extra = 3;
            lo1 = 0x90;
            hi1 = 0xBF;
        } else if (b0 >= 0xF1 && b0 <= 0xF3) {
            extra = 3;
            lo1 = 0x80;
            hi1 = 0xBF;
        } else if (b0 == 0xF4) {
            extra = 3;
            lo1 = 0x80;
            hi1 = 0x8F;
        } else {
            // Invalid lead byte (0x80..0xC1, 0xF5..0xFF).
            push_fffd(out);
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        bool ok = true;
        if (j >= n || b[j] < lo1 || b[j] > hi1) {
            ok = false;
        } else {
            ++j;
            for (int k = 1; k < extra && ok; ++k) {
                if (j >= n || b[j] < 0x80 || b[j] > 0xBF) {
                    ok = false;
                    break;
                }
                ++j;
            }
        }
        if (ok) {
            for (std::size_t x = i; x < j; ++x) out.push_back(static_cast<char>(b[x]));
            i = j;
        } else {
            // Consume the maximal valid subpart (b0 + accepted continuations).
            push_fffd(out);
            i = j;
        }
    }
    return out;
}

}  // namespace

VariantType variant_type_from(std::uint8_t b) {
    switch (b) {
        case 1:
            return VariantType::Float;
        case 2:
            return VariantType::String;
        case 3:
            return VariantType::Vec2;
        case 4:
            return VariantType::Vec3;
        case 5:
            return VariantType::Unsigned;
        case 9:
            return VariantType::Signed;
        default:
            return VariantType::Unknown;
    }
}

std::string Variant::as_string() const {
    if (auto* p = std::get_if<float>(&value)) return float_to_string(*p);
    if (auto* p = std::get_if<std::string>(&value)) return *p;
    if (auto* p = std::get_if<Vec2>(&value))
        return float_to_string(p->x) + ", " + float_to_string(p->y);
    if (auto* p = std::get_if<Vec3>(&value))
        return float_to_string(p->x) + ", " + float_to_string(p->y) + ", " +
               float_to_string(p->z);
    if (auto* p = std::get_if<std::uint32_t>(&value)) return std::to_string(*p);
    if (auto* p = std::get_if<std::int32_t>(&value)) return std::to_string(*p);
    return "";  // Unknown
}

std::int32_t Variant::as_int32() const {
    if (auto* p = std::get_if<std::int32_t>(&value)) return *p;
    return 0;
}

std::uint32_t Variant::as_uint32() const {
    if (auto* p = std::get_if<std::uint32_t>(&value)) return *p;
    return 0;
}

Vec2 Variant::as_vec2() const {
    if (auto* p = std::get_if<Vec2>(&value)) return *p;
    return Vec2{0.0f, 0.0f};
}

std::optional<VariantList> VariantList::deserialize(std::span<const std::uint8_t> data) {
    try {
        Cursor c(data.data(), data.size(), "variant");
        VariantList list;
        const std::size_t count = static_cast<std::size_t>(c.u8());
        list.variants.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            c.u8();  // index byte — read and discarded (parser is positional)
            const VariantType type = variant_type_from(c.u8());
            Variant v;
            switch (type) {
                case VariantType::Float:
                    v.value = c.f32();
                    break;
                case VariantType::String: {
                    const std::uint32_t len = c.u32();
                    const std::vector<std::uint8_t> raw = c.bytes(static_cast<std::size_t>(len));
                    v.value = utf8_lossy(raw);
                    break;
                }
                case VariantType::Vec2: {
                    const float x = c.f32();
                    const float y = c.f32();
                    v.value = Vec2{x, y};
                    break;
                }
                case VariantType::Vec3: {
                    const float x = c.f32();
                    const float y = c.f32();
                    const float z = c.f32();
                    v.value = Vec3{x, y, z};
                    break;
                }
                case VariantType::Unsigned:
                    v.value = c.u32();
                    break;
                case VariantType::Signed:
                    v.value = c.i32();
                    break;
                case VariantType::Unknown:
                default:
                    // Unknown consumes NO payload bytes (may desync following entries).
                    v.value = std::monostate{};
                    break;
            }
            list.variants.push_back(std::move(v));
        }
        return list;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

const Variant* VariantList::get(std::size_t index) const {
    if (index >= variants.size()) return nullptr;
    return &variants[index];
}

}  // namespace nxrth::protocol
