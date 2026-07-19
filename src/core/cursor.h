// Nxrth — little-endian, bounds-checked byte reader.
// Every binary parser (items.dat, save.dat, inventory, VariantList) is driven
// by this cursor. Ported from Mori/cursor.rs. GT is little-endian throughout.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nxrth {

// A borrowed (non-owning) sequential reader over a byte buffer. On any read that
// would run past the end it throws std::runtime_error with the verbatim message:
//   "{label} truncated at offset {pos} (need {n} more bytes)"
class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t len, const char* label)
        : data_(data), len_(len), label_(label) {}

    // Convenience overloads — buffer must outlive the cursor.
    Cursor(const std::vector<std::uint8_t>& data, const char* label)
        : data_(data.data()), len_(data.size()), label_(label) {}
    Cursor(std::string_view data, const char* label)
        : data_(reinterpret_cast<const std::uint8_t*>(data.data())),
          len_(data.size()), label_(label) {}

    std::size_t pos() const { return pos_; }
    std::size_t remaining() const { return len_ - pos_; }

    // Bounds check: throws if pos + n > len.
    void need(std::size_t n) const;

    void skip(std::size_t n);

    std::uint8_t u8();
    std::uint16_t u16();  // LE
    std::uint32_t u32();  // LE
    std::int32_t i32();   // LE
    float f32();          // LE IEEE-754

    std::vector<std::uint8_t> bytes(std::size_t len);

    // u16 length prefix L, then L bytes (raw copy).
    std::string plain_string();

    // len bytes, no length prefix (caller supplies len).
    std::string string_raw(std::size_t len);

    // u16 length prefix L, then out[i] = data[pos+i] ^ key[(key_start + i) % key.len()].
    std::string xor_string(std::string_view key, std::size_t key_start);

private:
    const std::uint8_t* data_;
    std::size_t len_;
    std::size_t pos_ = 0;
    const char* label_;
};

}  // namespace nxrth
