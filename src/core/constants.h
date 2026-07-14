// Adonai — Growtopia client constants (ported from Mori/constants.rs).
// GT is little-endian throughout. Version is FORCE-enforced by the game.
#pragma once
#include <cstdint>
#include <string_view>

namespace adonai::constants {

// App identity (native ImGui client).
inline constexpr std::string_view APP_NAME = "Adonai";
inline constexpr std::string_view APP_VERSION = "0.1.0";
// Upstream lineage credited to "North" (was "Cloei" upstream in the Rust build).
inline constexpr std::string_view UPSTREAM = "North";

// --- Growtopia protocol/version ---------------------------------------------
// 5.51 is FORCE-enforced (force update 2026-07-11); 5.50 gets UPDATE REQUIRED.
// PROTOCOL = 175 + minor (5.51 -> 226), accepted at the ENet logon.
inline constexpr std::uint32_t PROTOCOL = 226;
inline constexpr std::string_view GAME_VER = "5.51";
inline constexpr std::int32_t FHASH = -716928004;

// --- Default device identity (fallback when an account has no stored device) --
inline constexpr std::string_view DEFAULT_RID = "025B42980AFB659E0394C846233653FF";
inline constexpr std::string_view DEFAULT_MAC = "74:d4:dd:6c:24:e1";
inline constexpr std::string_view DEFAULT_WK = "788E366E74D2D098398A35C3F6360DDA";
inline constexpr std::string_view DEFAULT_HASH = "381621508";
inline constexpr std::string_view DEFAULT_HASH2 = "-332772458";
inline constexpr std::string_view DEFAULT_FZ = "18274296";
// The login platformID that actually matters for Adonai's flow (mobile 15,1,0).
inline constexpr std::string_view DEFAULT_PLATFORM_ID = "15,1,0";
inline constexpr std::string_view DEFAULT_ZF = "1597752569";
// NOTE: fz/hash2/zf/steamToken are used in the ENet protocol|211 login packet but
// MUST NOT be sent in the growid dashboard POST (that made the 5.51 dashboard 500).
inline constexpr std::string_view DEFAULT_STEAM_TOKEN =
    "14 00 00 00 3c 13 04 25 a4 c5 fd 4c 8c 55 93 72 01 00 10 01 38 b9 12 6a 18 00 00 00 01 00 00 00 05 00 00 00 6d 91 7f 14 12 ca 43 3b 27 31 26 00 02 00 00 00 b8 00 00 00 38 00 00 00 04 00 00 00 8c 55 93 72 01 00 10 01 e4 36 0d 00 c0 47 54 b9 06 64 a8 c0 00 00 00 00 6a 3c 10 6a ea eb 2b 6a 01 00 11 2b 04 00 01 00 e6 41 28 00 00 00 00 00 c2 54 51 5d b7 4e 66 c6 a0 52 02 c9 67 d0 56 bd aa d6 43 6d ab 51 3b 68 b9 3c 10 71 21 37 f7 ca ec 78 eb 8b 5d 83 5a 14 91 87 b2 fa b2 c3 db 69 1e 23 11 d2 35 bd 6b a0 1c 41 fe 83 75 43 51 ce b6 71 6f da be ef 1d 09 9e 04 53 46 7c 41 62 f8 f7 e6 51 ac b8 b1 c4 24 84 a6 94 33 36 52 8c dc 3a a6 d3 80 f6 ba 2d 26 d3 eb ac ea 91 5c 52 17 60 55 f2 52 26 24 d4 c6 b9 4b 52 c5 0e ff 2d 5d.240";

// --- Login endpoints ---------------------------------------------------------
inline constexpr std::string_view SERVER_DATA_HOST_1 = "www.growtopia1.com";
inline constexpr std::string_view SERVER_DATA_HOST_2 = "www.growtopia2.com";
inline constexpr std::string_view SERVER_DATA_PATH = "/growtopia/server_data.php";

} // namespace adonai::constants
