#include "protocol/crypto.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

namespace nxrth::protocol {

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

// --- Self-contained public-domain SHA-256 (FIPS 180-4) ----------------------
// Only used for secret-safe log fingerprints; no external dependency.
struct Sha256Ctx {
    std::uint32_t h[8];
    std::uint64_t bitlen = 0;
    std::uint8_t buffer[64];
    std::size_t buflen = 0;
};

inline std::uint32_t rotr32(std::uint32_t x, int c) {
    return (x >> c) | (x << (32 - c));
}

void sha256_block(Sha256Ctx& ctx, const std::uint8_t* p) {
    static const std::uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) |
               (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = ctx.h[0], b = ctx.h[1], c = ctx.h[2], d = ctx.h[3];
    std::uint32_t e = ctx.h[4], f = ctx.h[5], g = ctx.h[6], h = ctx.h[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + S1 + ch + K[i] + w[i];
        std::uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx.h[0] += a; ctx.h[1] += b; ctx.h[2] += c; ctx.h[3] += d;
    ctx.h[4] += e; ctx.h[5] += f; ctx.h[6] += g; ctx.h[7] += h;
}

void sha256_update(Sha256Ctx& ctx, const std::uint8_t* data, std::size_t len) {
    ctx.bitlen += static_cast<std::uint64_t>(len) * 8;
    while (len > 0) {
        std::size_t take = 64 - ctx.buflen;
        if (take > len) take = len;
        std::memcpy(ctx.buffer + ctx.buflen, data, take);
        ctx.buflen += take;
        data += take;
        len -= take;
        if (ctx.buflen == 64) {
            sha256_block(ctx, ctx.buffer);
            ctx.buflen = 0;
        }
    }
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

std::string sha256_hex(std::string_view s) {
    static const char* hex = "0123456789abcdef";
    Sha256Ctx ctx;
    ctx.h[0] = 0x6a09e667; ctx.h[1] = 0xbb67ae85; ctx.h[2] = 0x3c6ef372;
    ctx.h[3] = 0xa54ff53a; ctx.h[4] = 0x510e527f; ctx.h[5] = 0x9b05688c;
    ctx.h[6] = 0x1f83d9ab; ctx.h[7] = 0x5be0cd19;
    sha256_update(ctx, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());

    std::uint64_t bitlen = ctx.bitlen;
    std::uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);
    std::uint8_t zero = 0x00;
    while (ctx.buflen != 56) sha256_update(ctx, &zero, 1);
    std::uint8_t lenbytes[8];
    for (int i = 0; i < 8; ++i) {
        lenbytes[i] = static_cast<std::uint8_t>((bitlen >> (56 - 8 * i)) & 0xFF);
    }
    sha256_update(ctx, lenbytes, 8);

    std::string out;
    out.resize(64);
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 4; ++j) {
            std::uint8_t byte = static_cast<std::uint8_t>((ctx.h[i] >> (24 - 8 * j)) & 0xFF);
            out[i * 8 + j * 2] = hex[(byte >> 4) & 0xF];
            out[i * 8 + j * 2 + 1] = hex[byte & 0xF];
        }
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

}  // namespace nxrth::protocol
