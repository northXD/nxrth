#include "core/cursor.h"

#include <cstring>
#include <stdexcept>

namespace adonai {

void Cursor::need(std::size_t n) const {
    if (pos_ + n > len_) {
        throw std::runtime_error(std::string(label_) + " truncated at offset " +
                                 std::to_string(pos_) + " (need " +
                                 std::to_string(n) + " more bytes)");
    }
}

void Cursor::skip(std::size_t n) {
    need(n);
    pos_ += n;
}

std::uint8_t Cursor::u8() {
    need(1);
    return data_[pos_++];
}

std::uint16_t Cursor::u16() {
    need(2);
    std::uint16_t v = static_cast<std::uint16_t>(data_[pos_]) |
                      (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
}

std::uint32_t Cursor::u32() {
    need(4);
    std::uint32_t v = static_cast<std::uint32_t>(data_[pos_]) |
                      (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8) |
                      (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16) |
                      (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return v;
}

std::int32_t Cursor::i32() {
    return static_cast<std::int32_t>(u32());
}

float Cursor::f32() {
    std::uint32_t bits = u32();
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

std::vector<std::uint8_t> Cursor::bytes(std::size_t len) {
    need(len);
    std::vector<std::uint8_t> out(data_ + pos_, data_ + pos_ + len);
    pos_ += len;
    return out;
}

std::string Cursor::plain_string() {
    std::uint16_t len = u16();
    need(len);
    std::string out(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return out;
}

std::string Cursor::string_raw(std::size_t len) {
    need(len);
    std::string out(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return out;
}

std::string Cursor::xor_string(std::string_view key, std::size_t key_start) {
    std::uint16_t len = u16();
    need(len);
    std::string out;
    out.resize(len);
    const std::size_t klen = key.size();
    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t k = klen ? static_cast<std::uint8_t>(key[(key_start + i) % klen]) : 0;
        out[i] = static_cast<char>(data_[pos_ + i] ^ k);
    }
    pos_ += len;
    return out;
}

}  // namespace adonai
