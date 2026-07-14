#include "protocol/packet.h"

#include <cstring>

#include "core/cursor.h"

namespace adonai::protocol {

namespace {

void put_u32_le(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    b[off + 0] = static_cast<std::uint8_t>(v & 0xFF);
    b[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    b[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    b[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

void put_i32_le(std::vector<std::uint8_t>& b, std::size_t off, std::int32_t v) {
    put_u32_le(b, off, static_cast<std::uint32_t>(v));
}

void put_f32_le(std::vector<std::uint8_t>& b, std::size_t off, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32_le(b, off, bits);
}

void append_u32_le(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

// §6.1 — segment is the payload up to (not incl.) the first byte that is
// 0x00 OR >= 0x80. The resulting prefix is pure ASCII, hence always valid UTF-8.
std::string read_terminated_string(std::span<const std::uint8_t> payload) {
    std::size_t end = 0;
    while (end < payload.size()) {
        const std::uint8_t bb = payload[end];
        if (bb == 0x00 || bb >= 0x80) break;
        ++end;
    }
    return std::string(reinterpret_cast<const char*>(payload.data()), end);
}

}  // namespace

std::optional<GameUpdatePacket> GameUpdatePacket::from_bytes(std::span<const std::uint8_t> data) {
    if (data.size() < GAME_PACKET_SIZE) return std::nullopt;

    // The 56-byte header is contiguous and in field order; read sequentially.
    Cursor c(data.data(), data.size(), "gameupdatepacket");
    GameUpdatePacket p;
    p.packet_type = c.u8();
    p.object_type = c.u8();
    p.jump_count = c.u8();
    p.animation_type = c.u8();
    p.net_id = c.u32();
    p.target_net_id = c.i32();
    p.flags = c.u32();
    p.float_variable = c.f32();
    p.value = c.u32();
    p.vector_x = c.f32();
    p.vector_y = c.f32();
    p.vector_x2 = c.f32();
    p.vector_y2 = c.f32();
    p.particle_rotation = c.f32();
    p.int_x = c.i32();
    p.int_y = c.i32();
    const std::uint32_t extra_data_size = c.u32();  // [52..56] — always read

    if (p.flags & FLAG_EXTENDED) {
        const std::size_t end = GAME_PACKET_SIZE + static_cast<std::size_t>(extra_data_size);
        if (data.size() < end) return std::nullopt;
        p.extra_data.assign(data.begin() + GAME_PACKET_SIZE, data.begin() + end);
    }
    return p;
}

std::vector<std::uint8_t> GameUpdatePacket::to_bytes() const {
    const std::uint32_t extra_data_size =
        (flags & FLAG_EXTENDED) ? static_cast<std::uint32_t>(extra_data.size()) : 0u;

    std::vector<std::uint8_t> out(GAME_PACKET_SIZE + static_cast<std::size_t>(extra_data_size), 0);
    out[0] = packet_type;
    out[1] = object_type;
    out[2] = jump_count;
    out[3] = animation_type;
    put_u32_le(out, 4, net_id);
    put_i32_le(out, 8, target_net_id);
    put_u32_le(out, 12, flags);
    put_f32_le(out, 16, float_variable);
    put_u32_le(out, 20, value);
    put_f32_le(out, 24, vector_x);
    put_f32_le(out, 28, vector_y);
    put_f32_le(out, 32, vector_x2);
    put_f32_le(out, 36, vector_y2);
    put_f32_le(out, 40, particle_rotation);
    put_i32_le(out, 44, int_x);
    put_i32_le(out, 48, int_y);
    put_u32_le(out, 52, extra_data_size);
    if (extra_data_size > 0) {
        std::memcpy(out.data() + GAME_PACKET_SIZE, extra_data.data(), extra_data_size);
    }
    return out;
}

std::vector<std::uint8_t> make_text_packet(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + text.size() + 1);
    append_u32_le(out, MSG_TEXT);
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0x00);
    return out;
}

std::vector<std::uint8_t> make_game_message_packet(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + text.size() + 1);
    append_u32_le(out, MSG_GAME_MESSAGE);
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0x00);
    return out;
}

std::vector<std::uint8_t> make_game_packet(const GameUpdatePacket& pkt) {
    std::vector<std::uint8_t> body = pkt.to_bytes();
    std::vector<std::uint8_t> out;
    out.reserve(4 + body.size());
    append_u32_le(out, MSG_GAME_PACKET);  // no trailing NUL (binary)
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::optional<IncomingPacket> IncomingPacket::parse(std::span<const std::uint8_t> data) {
    if (data.size() < 4) return std::nullopt;

    const std::uint32_t msg_type = static_cast<std::uint32_t>(data[0]) |
                                   (static_cast<std::uint32_t>(data[1]) << 8) |
                                   (static_cast<std::uint32_t>(data[2]) << 16) |
                                   (static_cast<std::uint32_t>(data[3]) << 24);
    std::span<const std::uint8_t> payload = data.subspan(4);

    IncomingPacket out;
    switch (msg_type) {
        case MSG_SERVER_HELLO:
            out.type = Type::ServerHello;
            return out;
        case MSG_TEXT:
            out.type = Type::Text;
            out.text = read_terminated_string(payload);
            return out;
        case MSG_GAME_MESSAGE:
            out.type = Type::GameMessage;
            out.text = read_terminated_string(payload);
            return out;
        case MSG_GAME_PACKET: {
            auto gp = GameUpdatePacket::from_bytes(payload);
            if (!gp) return std::nullopt;  // propagate parse failure
            out.type = Type::GameUpdate;
            out.game_update = std::move(*gp);
            return out;
        }
        case MSG_TRACK:
            out.type = Type::Track;
            out.text = read_terminated_string(payload);
            return out;
        case MSG_CLIENT_LOG_REQUEST:
            out.type = Type::ClientLogRequest;
            return out;
        default:
            out.type = Type::Unknown;
            out.msg_type = msg_type;
            out.data.assign(payload.begin(), payload.end());
            return out;
    }
}

}  // namespace adonai::protocol
