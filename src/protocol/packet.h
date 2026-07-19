// Nxrth — GT wire framing + tank/GameUpdatePacket codec
// (ported from Mori/protocol/packet.rs). GT is little-endian throughout.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nxrth::protocol {

// --- §1 Outer ENet message types (4-byte LE u32 prefix on every payload) ------
inline constexpr std::uint32_t MSG_SERVER_HELLO = 1;
inline constexpr std::uint32_t MSG_TEXT = 2;              // NUL-terminated text
inline constexpr std::uint32_t MSG_GAME_MESSAGE = 3;      // NUL-terminated text
inline constexpr std::uint32_t MSG_GAME_PACKET = 4;       // 56-byte GameUpdatePacket
inline constexpr std::uint32_t MSG_TRACK = 6;             // telemetry string
inline constexpr std::uint32_t MSG_CLIENT_LOG_REQUEST = 7;
// Value 5 (NET_MESSAGE_ERROR) is intentionally undefined -> Unknown.

// --- §2 GamePacketType (byte [0] of the 56-byte struct) -----------------------
enum class GamePacketType : std::uint8_t {
    State = 0x00,  // default
    CallFunction = 0x01,
    UpdateStatus = 0x02,
    TileChangeRequest = 0x03,
    SendMapData = 0x04,
    SendTileUpdateData = 0x05,
    SendTileUpdateDataMultiple = 0x06,
    TileActivateRequest = 0x07,
    TileApplyDamage = 0x08,
    SendInventoryState = 0x09,
    ItemActivateRequest = 0x0A,
    ItemActivateObjectRequest = 0x0B,
    SendTileTreeState = 0x0C,
    ModifyItemInventory = 0x0D,
    ItemChangeObject = 0x0E,
    SendLock = 0x0F,
    SendItemDatabaseData = 0x10,
    SendParticleEffect = 0x11,
    SetIconState = 0x12,
    ItemEffect = 0x13,
    SetCharacterState = 0x14,
    PingReply = 0x15,
    PingRequest = 0x16,
    GotPunched = 0x17,
    AppCheckResponse = 0x18,
    AppIntegrityFail = 0x19,
    Disconnect = 0x1A,
    BattleJoin = 0x1B,
    BattleEvent = 0x1C,
    UseDoor = 0x1D,
    SendParental = 0x1E,
    GoneFishin = 0x1F,
    Steam = 0x20,
    PetBattle = 0x21,
    Npc = 0x22,
    Special = 0x23,
    SendParticleEffectV2 = 0x24,
    ActiveArrowToItem = 0x25,
    SelectTileIndex = 0x26,
    SendPlayerTributeData = 0x27,
    FtueSetItemToQuickInventory = 0x28,
    PveNpc = 0x29,
    PvpCardBattle = 0x2A,
    PveApplyPlayerDamage = 0x2B,
    PveNpcPositionUpdate = 0x2C,
    SetExtraMods = 0x2D,
    OnStepOnTileMod = 0x2E,
};

// True when raw is a known 0x00..0x2E subtype (else it is an Unknown byte,
// preserved verbatim in GameUpdatePacket::packet_type for round-trip fidelity).
inline constexpr bool is_known_packet_type(std::uint8_t raw) { return raw <= 0x2E; }

// --- §3 PacketFlags (raw u32 at offset [12]; retain unknown bits) -------------
namespace PacketFlags {
inline constexpr std::uint32_t WALK = 0x0000'0001u;
inline constexpr std::uint32_t UNK_2 = 0x0000'0002u;
inline constexpr std::uint32_t SPAWN_RELATED = 0x0000'0004u;
inline constexpr std::uint32_t EXTENDED = 0x0000'0008u;
inline constexpr std::uint32_t FACING_LEFT = 0x0000'0010u;
inline constexpr std::uint32_t STANDING = 0x0000'0020u;
inline constexpr std::uint32_t FIRE_DAMAGE = 0x0000'0040u;
inline constexpr std::uint32_t JUMP = 0x0000'0080u;
inline constexpr std::uint32_t GOT_KILLED = 0x0000'0100u;
inline constexpr std::uint32_t PUNCH = 0x0000'0200u;
inline constexpr std::uint32_t PLACE = 0x0000'0400u;
inline constexpr std::uint32_t TILE_CHANGE = 0x0000'0800u;
inline constexpr std::uint32_t GOT_PUNCHED = 0x0000'1000u;
inline constexpr std::uint32_t RESPAWN = 0x0000'2000u;
inline constexpr std::uint32_t OBJECT_COLLECT = 0x0000'4000u;
inline constexpr std::uint32_t TRAMPOLINE = 0x0000'8000u;
inline constexpr std::uint32_t DAMAGE = 0x0001'0000u;
inline constexpr std::uint32_t SLIDE = 0x0002'0000u;
inline constexpr std::uint32_t PARASOL = 0x0004'0000u;
inline constexpr std::uint32_t UNK_GRAVITY_RELATED = 0x0008'0000u;
inline constexpr std::uint32_t SWIM = 0x0010'0000u;
inline constexpr std::uint32_t WALL_HANG = 0x0020'0000u;
inline constexpr std::uint32_t POWER_UP_PUNCH_START = 0x0040'0000u;
inline constexpr std::uint32_t POWER_UP_PUNCH_END = 0x0080'0000u;
inline constexpr std::uint32_t UNK_TILE_CHANGE = 0x0100'0000u;
inline constexpr std::uint32_t HAY_CART_RELATED = 0x0200'0000u;
inline constexpr std::uint32_t ACID_RELATED_DAMAGE = 0x0400'0000u;
inline constexpr std::uint32_t UNK_3 = 0x0800'0000u;
inline constexpr std::uint32_t ACID_DAMAGE = 0x1000'0000u;
}  // namespace PacketFlags

// Backwards-friendly alias for the only codec-relevant flag.
inline constexpr std::uint32_t FLAG_EXTENDED = PacketFlags::EXTENDED;

// --- §4 GameUpdatePacket — the 56-byte tank struct ----------------------------
inline constexpr std::size_t GAME_PACKET_SIZE = 56;

struct GameUpdatePacket {
    std::uint8_t packet_type{};    // [0]  GamePacketType byte (raw; preserved)
    std::uint8_t object_type{};    // [1]
    std::uint8_t jump_count{};     // [2]
    std::uint8_t animation_type{}; // [3]
    std::uint32_t net_id{};        // [4]
    std::int32_t target_net_id{};  // [8]
    std::uint32_t flags{};         // [12] raw bits, unknown bits retained
    float float_variable{};        // [16]
    std::uint32_t value{};         // [20]
    float vector_x{};              // [24]
    float vector_y{};              // [28]
    float vector_x2{};             // [32]
    float vector_y2{};             // [36]
    float particle_rotation{};     // [40]
    std::int32_t int_x{};          // [44]
    std::int32_t int_y{};          // [48]
    // [52] extra_data_size (u32 LE) is derived on serialize, not stored.
    std::vector<std::uint8_t> extra_data;  // [56..] only when flags & EXTENDED

    // Input = ENet payload AFTER the 4-byte MSG_GAME_PACKET prefix.
    // nullopt if data < 56 bytes, or (EXTENDED set) the extended blob is truncated.
    static std::optional<GameUpdatePacket> from_bytes(std::span<const std::uint8_t> data);

    // 56 + extra_data_size bytes. extra_data is written only when EXTENDED is set.
    std::vector<std::uint8_t> to_bytes() const;
};

// --- §5 Packet builders (payload incl. the 4-byte type prefix) ----------------
std::vector<std::uint8_t> make_text_packet(std::string_view text);          // +trailing 0x00
std::vector<std::uint8_t> make_game_message_packet(std::string_view text);  // +trailing 0x00
std::vector<std::uint8_t> make_game_packet(const GameUpdatePacket& pkt);     // no trailing NUL

// --- §6 IncomingPacket dispatch (owned copies — safe across threads) ----------
struct IncomingPacket {
    enum class Type {
        ServerHello,
        Text,
        GameMessage,
        GameUpdate,
        Track,
        ClientLogRequest,
        Unknown,
    };

    Type type{Type::Unknown};
    std::string text;               // Text / GameMessage / Track
    GameUpdatePacket game_update;   // GameUpdate
    std::uint32_t msg_type{0};      // Unknown (raw outer type)
    std::vector<std::uint8_t> data; // Unknown payload (after the 4-byte prefix)

    // nullopt if data < 4 bytes, a game packet fails to parse, or (unreachable in
    // practice) a text segment is invalid UTF-8.
    static std::optional<IncomingPacket> parse(std::span<const std::uint8_t> data);
};

}  // namespace nxrth::protocol
