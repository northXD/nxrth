// Nxrth — VariantList / Variant codec (ported from Mori/protocol/variant.rs).
// The VariantList is the serialized argument list carried in the extra_data of a
// CallFunction (0x01) GameUpdatePacket. This module only DESERIALIZES; outgoing
// VariantList building lives elsewhere. GT is little-endian throughout.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace nxrth::protocol {

struct Vec2 {
    float x{};
    float y{};
};
struct Vec3 {
    float x{};
    float y{};
    float z{};
};

// Internal wire tag (u8 -> type). Anything not listed (incl. 0,6,7,8) -> Unknown.
enum class VariantType : std::uint8_t {
    Unknown = 0,
    Float = 1,
    String = 2,
    Vec2 = 3,
    Vec3 = 4,
    Unsigned = 5,
    Signed = 9,
};

// Maps a raw wire byte to a VariantType (unrecognized -> Unknown).
VariantType variant_type_from(std::uint8_t b);

// Public value type. std::monostate == Unknown (no payload).
struct Variant {
    std::variant<std::monostate, float, std::string, Vec2, Vec3, std::uint32_t, std::int32_t>
        value{};

    // Float -> shortest round-trip decimal ("1.0" -> "1"); String -> copy;
    // Vec2 -> "x, y"; Vec3 -> "x, y, z"; Unsigned/Signed -> decimal; Unknown -> "".
    std::string as_string() const;
    // Signed(v) -> v, else 0.
    std::int32_t as_int32() const;
    // Unsigned(v) -> v, else 0.
    std::uint32_t as_uint32() const;
    // Vec2(x,y) -> {x,y}, else {0,0}.
    Vec2 as_vec2() const;
};

struct VariantList {
    std::vector<Variant> variants;

    // Wire: count:u8, then per entry index:u8 (discarded), type:u8, payload.
    // Any read past the buffer -> std::nullopt. String uses lossy UTF-8 (never errors).
    static std::optional<VariantList> deserialize(std::span<const std::uint8_t> data);

    // Bounds-checked; nullptr if out of range.
    const Variant* get(std::size_t index) const;
    std::size_t size() const { return variants.size(); }
};

}  // namespace nxrth::protocol
