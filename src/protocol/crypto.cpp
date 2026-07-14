#include "protocol/crypto.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

namespace adonai::protocol {

namespace {

// --- Self-contained public-domain MD5 (RFC 1321) ----------------------------
// Based on the widely-used public-domain reference; no external dependency.

struct Md5Ctx {
    std::uint32_t a, b, c, d;
    std::uint64_t bitlen = 0;
    std::uint8_t buffer[64];
    std::size_t buflen = 0;
};

inline std::uint32_t rotl32(std::uint32_t x, int c) {
    return (x << c) | (x >> (32 - c));
}

void md5_block(Md5Ctx& ctx, const std::uint8_t* p) {
    static const std::uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    std::uint32_t M[16];
    for (int i = 0; i < 16; ++i) {
        M[i] = static_cast<std::uint32_t>(p[i * 4]) |
               (static_cast<std::uint32_t>(p[i * 4 + 1]) << 8) |
               (static_cast<std::uint32_t>(p[i * 4 + 2]) << 16) |
               (static_cast<std::uint32_t>(p[i * 4 + 3]) << 24);
    }

    std::uint32_t A = ctx.a, B = ctx.b, C = ctx.c, D = ctx.d;
    for (int i = 0; i < 64; ++i) {
        std::uint32_t F;
        int g;
        if (i < 16) {
            F = (B & C) | (~B & D);
            g = i;
        } else if (i < 32) {
            F = (D & B) | (~D & C);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            F = B ^ C ^ D;
            g = (3 * i + 5) & 15;
        } else {
            F = C ^ (B | ~D);
            g = (7 * i) & 15;
        }
        F = F + A + K[i] + M[g];
        A = D;
        D = C;
        C = B;
        B = B + rotl32(F, S[i]);
    }
    ctx.a += A;
    ctx.b += B;
    ctx.c += C;
    ctx.d += D;
}

void md5_init(Md5Ctx& ctx) {
    ctx.a = 0x67452301;
    ctx.b = 0xefcdab89;
    ctx.c = 0x98badcfe;
    ctx.d = 0x10325476;
    ctx.bitlen = 0;
    ctx.buflen = 0;
}

void md5_update(Md5Ctx& ctx, const std::uint8_t* data, std::size_t len) {
    ctx.bitlen += static_cast<std::uint64_t>(len) * 8;
    while (len > 0) {
        std::size_t take = 64 - ctx.buflen;
        if (take > len) take = len;
        std::memcpy(ctx.buffer + ctx.buflen, data, take);
        ctx.buflen += take;
        data += take;
        len -= take;
        if (ctx.buflen == 64) {
            md5_block(ctx, ctx.buffer);
            ctx.buflen = 0;
        }
    }
}

void md5_final(Md5Ctx& ctx, std::uint8_t out[16]) {
    std::uint64_t bitlen = ctx.bitlen;
    std::uint8_t pad = 0x80;
    md5_update(ctx, &pad, 1);
    std::uint8_t zero = 0x00;
    while (ctx.buflen != 56) {
        md5_update(ctx, &zero, 1);
    }
    std::uint8_t lenbytes[8];
    for (int i = 0; i < 8; ++i) {
        lenbytes[i] = static_cast<std::uint8_t>((bitlen >> (8 * i)) & 0xFF);
    }
    md5_update(ctx, lenbytes, 8);
    // buffer processed; emit state LE.
    std::uint32_t st[4] = {ctx.a, ctx.b, ctx.c, ctx.d};
    for (int i = 0; i < 4; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(st[i] & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((st[i] >> 8) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((st[i] >> 16) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>((st[i] >> 24) & 0xFF);
    }
}

std::array<std::uint8_t, 16> md5_bytes(std::string_view s) {
    Md5Ctx ctx;
    md5_init(ctx);
    md5_update(ctx, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    std::array<std::uint8_t, 16> out{};
    md5_final(ctx, out.data());
    return out;
}

std::mt19937& rng() {
    static thread_local std::mt19937 gen(std::random_device{}());
    return gen;
}

}  // namespace

std::string md5u(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    auto d = md5_bytes(s);
    std::string out;
    out.resize(32);
    for (int i = 0; i < 16; ++i) {
        out[i * 2] = hex[(d[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[d[i] & 0xF];
    }
    return out;
}

std::int32_t hash_string(std::string_view s) {
    std::uint32_t h = 0x55555555u;
    for (unsigned char b : s) {
        h = rotl32(h, 5) + b;
    }
    // one trailing NUL byte.
    h = rotl32(h, 5) + 0u;
    return static_cast<std::int32_t>(h);
}

std::string compute_klv(std::string_view game_version,
                        std::string_view protocol,
                        std::string_view rid,
                        std::int32_t hash_val) {
    static constexpr std::string_view KEY1 = "832aac071ffbcfc15bfe1d0a7ad15221";
    static constexpr std::string_view KEY2 = "709296ddd04fc4074a7b443ecc0799aa";
    static constexpr std::string_view KEY3 = "623de1e8fff22a2b3e0d7e01593e7c22";
    static constexpr std::string_view KEY4 = "bb835e5a57e6c88e2449499ca487ced2";
    static constexpr std::string_view KEY5 = "ea76e4d6009282186063fe9465f2d9ab";

    std::string combined;
    combined += md5u(md5u(game_version));            // game_version ×2
    combined += KEY1;
    combined += md5u(md5u(md5u(protocol)));          // protocol ×3
    combined += KEY2;
    combined += KEY3;                                // adjacent, no hash between
    combined += md5u(md5u(rid));                     // rid ×2
    combined += KEY4;
    combined += md5u(md5u(std::to_string(hash_val)));// hash_val decimal ×2
    combined += KEY5;

    return md5u(combined);
}

std::string random_hex(std::size_t n) {
    static const char* hex = "0123456789ABCDEF";
    std::uniform_int_distribution<int> dist(0, 255);
    std::string out;
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = hex[dist(rng()) & 0xF];
    }
    return out;
}

std::string random_mac() {
    std::uniform_int_distribution<int> dist(0, 255);
    char buf[18];
    std::snprintf(buf, sizeof(buf), "02:%02X:%02X:%02X:%02X:%02X",
                  dist(rng()) & 0xFF, dist(rng()) & 0xFF, dist(rng()) & 0xFF,
                  dist(rng()) & 0xFF, dist(rng()) & 0xFF);
    return std::string(buf);
}

std::string generate_rid() {
    return random_hex(32);
}

}  // namespace adonai::protocol
