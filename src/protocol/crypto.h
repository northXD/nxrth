// Adonai — Growtopia login crypto (ported from Mori/protocol/crypto.rs).
// KLV computation + GT rotate-left-5 hash. Uppercase MD5 hex is mandatory.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace adonai::protocol {

// MD5 of s, formatted as UPPERCASE hex (32 chars, no separators).
std::string md5u(std::string_view s);

// GT rotate-left-5 hash, NUL-terminated variant.
// Seed 0x55555555; h = rotl(h,5) + b over each byte then one trailing 0x00;
// returned as the uint32 bit-pattern reinterpreted to int32 (may be negative).
std::int32_t hash_string(std::string_view s);

// KLV: nested md5u concatenation with the five embedded GT keys.
// game_version ×2, protocol ×3, rid ×2, hash_val ×2; final md5u over the whole.
std::string compute_klv(std::string_view game_version,
                        std::string_view protocol,
                        std::string_view rid,
                        std::int32_t hash_val);

// n independent random nibbles (0..15) as UPPERCASE hex chars.
std::string random_hex(std::size_t n);

// "02:XX:XX:XX:XX:XX" — fixed first octet 02, 5 random octets, uppercase.
std::string random_mac();

// == random_hex(32) — 32 uppercase hex chars.
std::string generate_rid();

}  // namespace adonai::protocol
