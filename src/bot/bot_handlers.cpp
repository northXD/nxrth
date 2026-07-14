// Adonai — Bot packet-handler half (port spec 07 §2.2-§2.11).
// service_once ENet pump + Connect/Disconnect state machine, IncomingPacket
// dispatch, ServerHello / login+redirect packet builders, ping reply, and the
// CallFunction dispatch (incl. OnSendToServer redirect capture + OnSpawn self).
// GT wire data is little-endian. NO Lua, NO web server. Only Bot methods declared
// in bot/bot.h are defined here; free helpers below are file-local.
#include "bot/bot.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "core/constants.h"
#include "protocol/crypto.h"
#include "world/inventory.h"
#include "world/world.h"

namespace adonai::bot {

namespace {

using adonai::protocol::GameUpdatePacket;
using adonai::protocol::GamePacketType;
using adonai::protocol::IncomingPacket;
using adonai::protocol::Variant;
using adonai::protocol::VariantList;
using Clock = std::chrono::steady_clock;

constexpr std::uint8_t kPT(GamePacketType t) { return static_cast<std::uint8_t>(t); }

// Trailing-whitespace trim (Rust str::trim_end).
std::string rtrim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}
// Both-ends trim (Rust str::trim).
std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Full-string i64 parse (Rust str::parse::<i64>() — rejects trailing junk).
std::optional<long long> parse_full_i64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try {
        std::size_t p = 0;
        long long v = std::stoll(s, &p);
        if (p != s.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::uint32_t to_u32(const std::string& s) {
    try {
        return static_cast<std::uint32_t>(std::stoul(trim(s)));
    } catch (...) {
        return 0;
    }
}
std::uint64_t to_u64(const std::string& s) {
    try {
        return static_cast<std::uint64_t>(std::stoull(trim(s)));
    } catch (...) {
        return 0;
    }
}
std::int32_t to_i32(const std::string& s) {
    try {
        return static_cast<std::int32_t>(std::stol(trim(s)));
    } catch (...) {
        return 0;
    }
}
float to_f32(const std::string& s) {
    try {
        return std::stof(trim(s));
    } catch (...) {
        return 0.0f;
    }
}

// vl.get(i).as_string() or "" when absent.
std::string arg_str(const VariantList& vl, std::size_t i) {
    const Variant* v = vl.get(i);
    return v ? v->as_string() : std::string();
}

// Debug/Display name of a GamePacketType byte (matches Rust `{:?}`).
std::string gpt_name(std::uint8_t raw) {
    switch (raw) {
        case 0x00: return "State";
        case 0x01: return "CallFunction";
        case 0x02: return "UpdateStatus";
        case 0x03: return "TileChangeRequest";
        case 0x04: return "SendMapData";
        case 0x05: return "SendTileUpdateData";
        case 0x06: return "SendTileUpdateDataMultiple";
        case 0x07: return "TileActivateRequest";
        case 0x08: return "TileApplyDamage";
        case 0x09: return "SendInventoryState";
        case 0x0A: return "ItemActivateRequest";
        case 0x0B: return "ItemActivateObjectRequest";
        case 0x0C: return "SendTileTreeState";
        case 0x0D: return "ModifyItemInventory";
        case 0x0E: return "ItemChangeObject";
        case 0x0F: return "SendLock";
        case 0x10: return "SendItemDatabaseData";
        case 0x11: return "SendParticleEffect";
        case 0x12: return "SetIconState";
        case 0x13: return "ItemEffect";
        case 0x14: return "SetCharacterState";
        case 0x15: return "PingReply";
        case 0x16: return "PingRequest";
        case 0x17: return "GotPunched";
        case 0x18: return "AppCheckResponse";
        case 0x19: return "AppIntegrityFail";
        case 0x1A: return "Disconnect";
        case 0x1B: return "BattleJoin";
        case 0x1C: return "BattleEvent";
        case 0x1D: return "UseDoor";
        case 0x1E: return "SendParental";
        case 0x1F: return "GoneFishin";
        case 0x20: return "Steam";
        case 0x21: return "PetBattle";
        case 0x22: return "Npc";
        case 0x23: return "Special";
        case 0x24: return "SendParticleEffectV2";
        case 0x25: return "ActiveArrowToItem";
        case 0x26: return "SelectTileIndex";
        case 0x27: return "SendPlayerTributeData";
        case 0x28: return "FtueSetItemToQuickInventory";
        case 0x29: return "PveNpc";
        case 0x2A: return "PvpCardBattle";
        case 0x2B: return "PveApplyPlayerDamage";
        case 0x2C: return "PveNpcPositionUpdate";
        case 0x2D: return "SetExtraMods";
        case 0x2E: return "OnStepOnTileMod";
        default: return "Unknown(" + std::to_string(static_cast<int>(raw)) + ")";
    }
}

// Display form of a GameUpdatePacket. MUST begin "GameUpdatePacket" so the console
// high-frequency-noise filter (§2.16) routes it to the file log only.
std::string format_game_packet_detail(const GameUpdatePacket& p) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%08x", p.flags);
    std::string s = "GameUpdatePacket { type=" + gpt_name(p.packet_type);
    s += " object_type=" + std::to_string(static_cast<int>(p.object_type));
    s += " net_id=" + std::to_string(p.net_id);
    s += " target_net_id=" + std::to_string(p.target_net_id);
    s += " flags=" + std::string(buf);
    s += " value=" + std::to_string(p.value);
    s += " float=" + std::to_string(p.float_variable);
    s += " vec=(" + std::to_string(p.vector_x) + ", " + std::to_string(p.vector_y) + ")";
    s += " int=(" + std::to_string(p.int_x) + ", " + std::to_string(p.int_y) + ")";
    s += " extra_len=" + std::to_string(p.extra_data.size()) + " }";
    return s;
}

std::string hex_dump(const std::vector<std::uint8_t>& data) {
    std::string out;
    out.reserve(data.size() * 3);
    char b[4];
    for (std::uint8_t byte : data) {
        std::snprintf(b, sizeof(b), "%02x ", byte);
        out += b;
    }
    return out;
}

}  // namespace

// ===========================================================================
// §2.2 service_once — drain ALL currently-available ENet events.
// ===========================================================================
void Bot::service_once() {
    while (auto ev = host_.next_event()) {
        switch (ev->type) {
            case adonai::net::HostEvent::Type::Connect:
                on_connect(ev->peer);
                break;
            case adonai::net::HostEvent::Type::Disconnect:
                on_disconnect(ev->peer, ev->data);
                break;
            case adonai::net::HostEvent::Type::Receive:
                on_receive(ev->peer, ev->channel_id, ev->packet);
                break;
        }
    }
}

// --- Receive — incoming packet dispatch ------------------------------------
void Bot::on_receive(adonai::net::PeerId id, std::uint8_t channel,
                     const std::vector<std::uint8_t>& data) {
    const std::size_t size = data.size();
    auto parsed = IncomingPacket::parse(data);
    if (!parsed) {
        std::string hex = hex_dump(data);
        emit_traffic("in", "parse_error", size, "channel=" + std::to_string(channel) + "\n" + hex);
        log_console("[Bot] Failed to parse packet (" + std::to_string(size) + " bytes on ch " +
                    std::to_string(channel) + "): " + hex);
        return;
    }

    switch (parsed->type) {
        case IncomingPacket::Type::ServerHello:
            emit_traffic("in", "server_hello", size, "ServerHello");
            on_server_hello();
            break;

        case IncomingPacket::Type::Text:
            emit_traffic("in", "text", size, redact_packet_text(parsed->text));
            log_console("[Bot] Text: " + parsed->text);
            break;

        case IncomingPacket::Type::GameMessage:
            emit_traffic("in", "game_message", size, redact_packet_text(parsed->text));
            log_console("[Bot] GameMessage: " + parsed->text);
            handle_game_message(parsed->text, id);
            break;

        case IncomingPacket::Type::GameUpdate: {
            const GameUpdatePacket& pkt = parsed->game_update;
            emit_traffic("in", "game_update:" + gpt_name(pkt.packet_type), size,
                         format_game_packet_detail(pkt));
            update_geiger_signal(pkt);

            switch (pkt.packet_type) {
                case kPT(GamePacketType::SetCharacterState):
                    local_.hack_type = pkt.value;
                    local_.build_length =
                        pkt.jump_count >= 126 ? static_cast<std::uint8_t>(pkt.jump_count - 126) : 0;
                    local_.punch_length = pkt.animation_type >= 126
                                              ? static_cast<std::uint8_t>(pkt.animation_type - 126)
                                              : 0;
                    local_.gravity = pkt.vector_x2;
                    local_.velocity = pkt.vector_y2;
                    break;

                case kPT(GamePacketType::CallFunction):
                    on_call_function(id, pkt.extra_data);
                    break;

                case kPT(GamePacketType::PingRequest):
                    on_ping_request(pkt.value);
                    break;

                case kPT(GamePacketType::SendInventoryState):
                    try {
                        adonai::world::Inventory inv = adonai::world::Inventory::parse(pkt.extra_data);
                        log_console("[Bot] Inventory: " + std::to_string(inv.items.size()) +
                                    " items");
                        equipped_items_.clear();
                        for (const auto& [item_id, item] : inv.items) {
                            if (item.flag & 1) equipped_items_.insert(item_id);
                        }
                        inventory_ = std::move(inv);
                        emit_inventory_update();
                    } catch (const std::exception& e) {
                        log_console(std::string("[Bot] Inventory parse error: ") + e.what());
                    }
                    break;

                case kPT(GamePacketType::SendMapData):
                    load_world(pkt);
                    break;

                case kPT(GamePacketType::State):
                    on_state(pkt);
                    break;
                case kPT(GamePacketType::TileChangeRequest):
                    on_tile_change(pkt);
                    break;
                case kPT(GamePacketType::SendTileUpdateData):
                    on_send_tile_update_data(pkt);
                    break;
                case kPT(GamePacketType::SendTileUpdateDataMultiple):
                    on_send_tile_update_data_multiple(pkt);
                    break;
                case kPT(GamePacketType::SendTileTreeState):
                    on_send_tile_tree_state(pkt);
                    break;
                case kPT(GamePacketType::ModifyItemInventory):
                    on_modify_item_inventory(pkt);
                    break;
                case kPT(GamePacketType::ItemChangeObject):
                    on_item_change_object(pkt);
                    break;
                case kPT(GamePacketType::SendLock):
                    on_send_lock(pkt);
                    break;

                default:
                    log_console("[Bot] " + format_game_packet_detail(pkt));
                    break;
            }
            break;
        }

        case IncomingPacket::Type::Track:
            emit_traffic("in", "track", size, redact_packet_text(parsed->text));
            log_console("[Bot] Track: " + parsed->text);
            handle_track(parsed->text);
            break;

        case IncomingPacket::Type::ClientLogRequest:
            emit_traffic("in", "client_log_request", size, "ClientLogRequest");
            log_console("[Bot] ClientLogRequest");
            break;

        case IncomingPacket::Type::Unknown:
            emit_traffic("in", "unknown:" + std::to_string(parsed->msg_type), size,
                         "unknown msg_type=" + std::to_string(parsed->msg_type) +
                             " payload_len=" + std::to_string(parsed->data.size()));
            log_console("[Bot] Unknown msg_type=" + std::to_string(parsed->msg_type) +
                        " len=" + std::to_string(parsed->data.size()));
            break;
    }
}

// --- GameMessage substring scans + logon_fail ladder -----------------------
void Bot::handle_game_message(const std::string& s, adonai::net::PeerId id) {
    const auto now = Clock::now();
    auto has = [&](const char* needle) { return s.find(needle) != std::string::npos; };

    if (has("action|request_token")) {
        log_console("[Bot] Server requested a fresh login token - fetching now.");
        redirect_.reset();
        redirect_attempts_ = 0;
        refresh_token_on_reconnect_ = false;
        reconnect_after_.reset();
        reconnect_main(true);
        return;  // restart the event-drain loop; skip remaining scans
    }

    if (has("Advanced Account Protection")) pending_2fa_ = true;
    if (has("action|log") && has("SERVER OVERLOADED")) pending_server_overload_ = true;
    if (has("action|log") && has("Too many people logging in")) pending_too_many_logins_ = true;
    if (has("action|log") && (has("Please try again in") || has("Fail to login")))
        pending_login_throttle_ = true;
    if (has("Server couldn't prepare a place")) pending_place_prepare_ = true;
    if (has("action|log") && has("Server requesting that you re-logon")) {
        log_console("[Bot] Server requested re-logon — clearing redirect data.");
        redirect_.reset();
        pending_relogon_ = true;
    }
    if (has("action|log") && has("UPDATE REQUIRED")) pending_update_required_ = true;
    if (has("action|log") && has("undergoing maintenance")) pending_maintenance_ = true;

    if (has("action|logon_fail")) {
        // Consume the pending flags (first match wins, priority order), then
        // unconditionally disconnect.
        if (pending_2fa_) {
            pending_2fa_ = false;
            std::uint64_t secs = delays_.twofa_secs;
            log_console("[Bot] Logon failed — 2FA (Advanced Account Protection). Retrying in " +
                        std::to_string(secs) + " s.");
            reconnect_after_ = now + std::chrono::seconds(secs);
            emit_status(BotStatus::TwoFactorAuth);
        } else if (pending_server_overload_) {
            pending_server_overload_ = false;
            std::uint64_t secs = delays_.server_overload_secs + (bot_id_ % 7);
            log_console("[Bot] Logon failed — server overloaded. Retrying in " +
                        std::to_string(secs) + " s.");
            reconnect_after_ = now + std::chrono::seconds(secs);
            emit_status(BotStatus::ServerOverloaded);
        } else if (pending_too_many_logins_) {
            pending_too_many_logins_ = false;
            std::uint64_t secs = delays_.too_many_logins_secs + (bot_id_ % 5);
            log_console("[Bot] Logon failed — too many logins at once. Retrying in " +
                        std::to_string(secs) + " s.");
            reconnect_after_ = now + std::chrono::seconds(secs);
            emit_status(BotStatus::TooManyLogins);
        } else if (pending_login_throttle_) {
            pending_login_throttle_ = false;
            if (login_throttle_streak_ < UINT32_MAX) ++login_throttle_streak_;
            SteadyTp slot = reserve_throttle_slot(gateway_logon_gate(), GATEWAY_THROTTLE_COOLDOWN_MS,
                                                  GATEWAY_LOGON_SPACING_MS);
            std::uint64_t secs =
                slot > now
                    ? static_cast<std::uint64_t>(
                          std::chrono::duration_cast<std::chrono::seconds>(slot - now).count())
                    : 0;
            std::uint32_t streak = login_throttle_streak_;  // capture before any reset
            bool rotate = login_proxy_.has_value();
            if (rotate) {
                refresh_token_on_reconnect_ = true;
                login_throttle_streak_ = 0;
                log_console("[Bot] Logon failed — 'Fail to login, try again in 30s' (" +
                            std::to_string(streak) +
                            "x): restarting login from scratch on a FRESH exit IP + token, fleet "
                            "retry in ~" +
                            std::to_string(secs) + " s.");
            } else {
                log_console("[Bot] Logon failed — 'Fail to login, try again in 30s' (" +
                            std::to_string(streak) +
                            "x); reusing token (no rotating login proxy configured), fleet retry "
                            "in ~" +
                            std::to_string(secs) + " s.");
            }
            reconnect_after_ = slot;
            emit_status(BotStatus::TooManyLogins);
        } else if (pending_place_prepare_) {
            pending_place_prepare_ = false;
            std::uint64_t secs = delays_.server_overload_secs + (bot_id_ % 9);
            log_console("[Bot] Logon failed — server could not prepare a place. Retrying in " +
                        std::to_string(secs) + " s.");
            reconnect_after_ = now + std::chrono::seconds(secs);
            emit_status(BotStatus::ServerOverloaded);
        } else if (pending_relogon_) {
            pending_relogon_ = false;
            log_console("[Bot] Logon failed — server requested re-logon. Reconnecting.");
        } else if (pending_update_required_) {
            pending_update_required_ = false;
            log_console("[Bot] Logon failed — client update required. Stopping bot.");
            emit_status(BotStatus::UpdateRequired);
            stop_requested_ = true;
        } else if (pending_maintenance_) {
            pending_maintenance_ = false;
            std::uint64_t secs = delays_.maintenance_secs;
            log_console("[Bot] Logon failed — server maintenance. Retrying in " +
                        std::to_string(secs) + " s.");
            reconnect_after_ = now + std::chrono::seconds(secs);
            emit_status(BotStatus::Maintenance);
        } else {
            log_console("[Bot] Logon failed — clearing redirect and reconnecting");
            redirect_.reset();
            redirect_attempts_ = 0;
            refresh_token_on_reconnect_ = true;
        }
        host_.peer_disconnect(id, 0);
    }
}

// --- Track parse -----------------------------------------------------------
void Bot::handle_track(const std::string& s) {
    if (s.find("Authentication_error|23") != std::string::npos) pending_place_prepare_ = true;
    auto map = parse_pipe_map(s);
    auto get = [&](const char* k) -> std::string {
        auto it = map.find(k);
        return it != map.end() ? it->second : std::string();
    };
    TrackInfo ti;
    ti.level = to_u32(get("Level"));
    ti.grow_id = to_u64(get("GrowId"));
    ti.install_date = to_u64(get("installDate"));
    ti.global_playtime = to_u64(get("Global_Playtime"));
    ti.awesomeness = to_u32(get("Awesomeness"));
    state_->write([&](BotState& st) { st.track_info = ti; });
    notify_dirty();
}

// ===========================================================================
// §2.7 on_server_hello
// ===========================================================================
void Bot::on_server_hello() {
    saw_server_hello_ = true;
    pre_hello_disconnects_ = 0;
    redirect_connect_fails_ = 0;  // this leg's game proxy works
    on_subserver_connect_ = false;  // ServerHello reached: whatever leg this was, it connected

    std::string data;
    if (redirect_.has_value()) {
        subserver_connect_fails_ = 0;  // reached the subserver -> this game proxy reaches the world
        RedirectData r = *redirect_;
        redirect_.reset();  // consume
        if (redirect_attempts_ < 255) ++redirect_attempts_;
        log_console("[Bot] ServerHello (redirect → " + r.door_id + ")");
        data = build_redirect_packet(r);
    } else {
        log_console("[Bot] ServerHello");
        wait_for_global_gate(login_packet_gate(), LOGIN_PACKET_STAGGER_MS, "login packet");
        data = build_login_packet();
    }

    // Developer console dump — redacted (never log the raw token).
    log_console("=== RAW LOGIN PACKET ===\n" + redact_packet_text(data));
    send_text(data);
}

// ===========================================================================
// §2.8 on_ping_request -> PingReply
// ===========================================================================
void Bot::on_ping_request(std::uint32_t challenge) {
    std::uint32_t time_val = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_time_).count());
    float bx = local_.build_length == 0 ? 2.0f : static_cast<float>(local_.build_length);
    float by = local_.punch_length == 0 ? 2.0f : static_cast<float>(local_.punch_length);
    bool in_world = world_.has_value();

    GameUpdatePacket reply{};
    reply.packet_type = kPT(GamePacketType::PingReply);
    reply.target_net_id = adonai::protocol::hash_string(std::to_string(challenge));
    reply.value = time_val;
    reply.vector_x = bx * 32.0f;
    reply.vector_y = by * 32.0f;
    if (in_world) {
        reply.net_id = local_.hack_type;
        reply.vector_x2 = local_.velocity;
        reply.vector_y2 = local_.gravity;
    }
    send_game_packet(reply, true);
}

// ===========================================================================
// §2.9 on_call_function — CallFunction dispatch
// ===========================================================================
void Bot::on_call_function(adonai::net::PeerId id, const std::vector<std::uint8_t>& extra_data) {
    auto vl_opt = VariantList::deserialize(extra_data);
    if (!vl_opt) {
        log_console("[Bot] VariantList parse error");
        return;
    }
    const VariantList& vl = *vl_opt;
    std::string fn_name = arg_str(vl, 0);
    log_console("[Bot] CallFunction: " + fn_name);

    auto rebuild_players_state = [&]() {
        std::vector<PlayerInfo> pv;
        pv.reserve(players_.size());
        for (const auto& [nid, p] : players_) {
            pv.push_back(PlayerInfo{p.net_id, p.name, p.position.first / 32.0f,
                                    p.position.second / 32.0f, p.country});
        }
        state_->write([&](BotState& s) { s.players = std::move(pv); });
    };

    if (fn_name == "OnSendToServer") {
        std::uint16_t port = 0;
        try {
            port = static_cast<std::uint16_t>(std::stoul(trim(arg_str(vl, 1))));
        } catch (...) {
            port = 0;
        }
        const Variant* v2 = vl.get(2);
        std::string raw_token = v2 ? v2->as_string() : std::string("0");
        const Variant* v3 = vl.get(3);
        std::string user_id = v3 ? v3->as_string() : std::string("0");
        std::string server_str = arg_str(vl, 4);
        std::int32_t lmode = to_i32(arg_str(vl, 5));
        std::string tank_id_name = arg_str(vl, 6);

        // splitn(3, '|')
        std::string server, part1, part2;
        {
            std::size_t p0 = server_str.find('|');
            if (p0 == std::string::npos) {
                server = server_str;
            } else {
                server = server_str.substr(0, p0);
                std::size_t p1 = server_str.find('|', p0 + 1);
                if (p1 == std::string::npos) {
                    part1 = server_str.substr(p0 + 1);
                } else {
                    part1 = server_str.substr(p0 + 1, p1 - (p0 + 1));
                    part2 = server_str.substr(p1 + 1);
                }
            }
        }
        server = rtrim(server);
        std::string door_id = rtrim(part1);
        if (door_id.empty()) door_id = "0";
        std::string raw_uuid = rtrim(part2);

        // Token resolution.
        std::string token;
        auto tok_i64 = parse_full_i64(trim(raw_token));
        if (tok_i64 && *tok_i64 < 0) {
            if (last_redirect_token_.has_value()) {
                token = *last_redirect_token_;
                log_console("[Bot] OnSendToServer -> using cached redirect token for marker " +
                            raw_token + " lmode=" + std::to_string(lmode));
            } else {
                log_console("[Bot] OnSendToServer -> no cached redirect token for marker " +
                            raw_token + " lmode=" + std::to_string(lmode));
                token = raw_token;
            }
        } else {
            if (!trim(raw_token).empty()) last_redirect_token_ = raw_token;
            token = raw_token;
        }

        // UUID resolution.
        std::string uuid;
        std::string uuid_marker = trim(raw_uuid);
        if (uuid_marker.empty() || uuid_marker == "-1") {
            if (last_redirect_uuid_.has_value()) {
                uuid = *last_redirect_uuid_;
                log_console(
                    "[Bot] OnSendToServer -> using cached redirect UUIDToken for marker \"" +
                    uuid_marker + "\" lmode=" + std::to_string(lmode));
            } else {
                log_console(
                    "[Bot] OnSendToServer -> no cached redirect UUIDToken for marker \"" +
                    uuid_marker + "\" lmode=" + std::to_string(lmode));
                uuid = raw_uuid;
            }
        } else {
            last_redirect_uuid_ = raw_uuid;
            uuid = raw_uuid;
        }

        log_console("[Bot] OnSendToServer → " + server + ":" + std::to_string(port) +
                    " door=" + door_id);

        RedirectData rd;
        rd.server = server;
        rd.port = port;
        rd.token = token;
        rd.user = user_id;
        rd.door_id = door_id;
        rd.uuid = uuid;
        rd.lmode = std::to_string(lmode);
        rd.tank_id_name = tank_id_name;
        redirect_ = std::move(rd);

        redirect_attempts_ = 0;
        login_reject_streak_ = 0;
        login_throttle_streak_ = 0;  // gateway accepted the logon
        host_.peer_disconnect(id, 0);
        return;
    }

    if (fn_name == "OnSpawn") {
        std::string message = arg_str(vl, 1);
        auto data = parse_pipe_map(message);
        auto get = [&](const char* k) -> std::string {
            auto it = data.find(k);
            return it != data.end() ? it->second : std::string();
        };

        if (data.count("type")) {
            // Local player (self).
            local_.net_id = to_u32(get("netID"));
            local_.user_id = to_u32(get("userID"));
            redirect_.reset();
            redirect_attempts_ = 0;
            refresh_token_on_reconnect_ = false;
            clear_login_state_flags();
            connected_since_.reset();  // login watchdog satisfied
            was_in_world_ = true;
            log_console("[Bot] OnSpawn (self) net_id=" + std::to_string(local_.net_id) +
                        " user_id=" + std::to_string(local_.user_id));
            // Redacted — never log the raw ltoken to the shared console.
            log_console("[Bot] ltoken string: <redacted>");
            emit_status(BotStatus::InGame);
        } else {
            // Another player.
            float px = 0.0f, py = 0.0f;
            std::string pos = get("posXY");
            if (!pos.empty()) {
                std::size_t bar = pos.find('|');
                if (bar == std::string::npos) {
                    px = to_f32(pos);
                } else {
                    px = to_f32(pos.substr(0, bar));
                    py = to_f32(pos.substr(bar + 1));
                }
            }
            Player p;
            p.net_id = to_u32(get("netID"));
            p.user_id = to_u32(get("userID"));
            p.m_state = to_u32(get("mstate"));
            p.invisible = to_u32(get("invis")) != 0;
            p.name = get("name");
            p.country = get("country");
            p.position = {px, py};
            p.avatar = get("avatar");
            p.online_id = get("onlineID");
            p.e_id = get("eid");
            p.ip = get("ip");
            p.col_rect = get("colrect");
            p.title_icon = get("titleIcon");

            char posbuf[64];
            std::snprintf(posbuf, sizeof(posbuf), "(%.0f,%.0f)", px, py);
            log_console("[Bot] OnSpawn player=" + p.name + " net_id=" + std::to_string(p.net_id) +
                        " pos=" + posbuf);

            std::uint32_t nid = p.net_id;
            players_[nid] = std::move(p);
            rebuild_players_state();
            notify_dirty();
        }
        return;
    }

    if (fn_name == "OnSetPos") {
        const Variant* v = vl.get(1);
        adonai::protocol::Vec2 xy = v ? v->as_vec2() : adonai::protocol::Vec2{};
        pos_x_ = xy.x;
        pos_y_ = xy.y;
        if (pathfind_target_.has_value()) pathfind_recalc_ = true;
        state_->write([&](BotState& s) {
            s.pos_x = xy.x / 32.0f;
            s.pos_y = xy.y / 32.0f;
        });
        log_console("[Bot] OnSetPos → (" + std::to_string(xy.x) + ", " + std::to_string(xy.y) + ")");
        notify_dirty();
        return;
    }

    if (fn_name == "OnSuperMainStartAcceptLogonHrdxs47254722215a") {
        emit_status(BotStatus::Connected);
        wait_for_global_gate(enter_game_gate(), ENTER_GAME_STAGGER_MS, "enter_game");
        send_text("action|enter_game\n");
        return;
    }

    if (fn_name == "OnRemove") {
        auto data = parse_pipe_map(arg_str(vl, 1));
        std::uint32_t net_id = 0;
        auto it = data.find("netID");
        if (it != data.end()) net_id = to_u32(it->second);
        players_.erase(net_id);
        rebuild_players_state();
        log_console("[Bot] OnRemove net_id=" + std::to_string(net_id));
        notify_dirty();
        return;
    }

    if (fn_name == "OnSetBux") {
        const Variant* v = vl.get(1);
        std::int32_t gems = v ? v->as_int32() : 0;
        inventory_.add_gems(gems);
        state_->write([&](BotState& s) { s.gems = gems; });
        notify_dirty();
        return;
    }

    if (fn_name == "OnConsoleMessage") {
        std::string message = arg_str(vl, 1);
        sync_geiger_state_from_console(message);
        log_chat(message);  // in-game console/chat -> Console tab (not system Logs)
        return;
    }

    if (fn_name == "OnDialogRequest") {
        std::string message = arg_str(vl, 1);
        log_console("[Bot] Dialog: " + message);
        std::optional<std::function<void(Bot&)>> cb;
        cb.swap(temporary_data_.dialog_callback);
        if (cb) (*cb)(*this);
        return;
    }

    if (fn_name == "SetHasGrowID") {
        const Variant* v = vl.get(2);  // NOTE: index 2, not 1
        if (v) {
            std::string growid = v->as_string();
            username_ = growid;
            state_->write([&](BotState& s) { s.username = growid; });
            notify_dirty();
        }
        return;
    }

    if (fn_name == "OnRequestWorldSelectMenu") {
        world_.reset();
        pathfind_target_.reset();
        pathfind_recalc_ = false;
        bool removed = inventory_.remove_temp_items();
        state_->write([&](BotState& s) {
            s.world_name = "EXIT";
            s.status = BotStatus::InGame;
        });
        if (removed) emit_inventory_update();
        notify_dirty();
        log_console("[Bot] OnRequestWorldSelectMenu → cleared world");
        return;
    }

    // Any other function name: no-op.
}

// ===========================================================================
// §2.11 clear_login_state_flags
// ===========================================================================
void Bot::clear_login_state_flags() {
    pending_2fa_ = false;
    pending_relogon_ = false;
    pending_server_overload_ = false;
    pending_too_many_logins_ = false;
    pending_login_throttle_ = false;
    pending_place_prepare_ = false;
    pending_update_required_ = false;
    pending_maintenance_ = false;
    login_reject_streak_ = 0;
    login_throttle_streak_ = 0;
    pre_hello_disconnects_ = 0;
}

// ===========================================================================
// §2.6 build_login_packet — first (gateway) logon body
// ===========================================================================
std::string Bot::build_login_packet() const {
    const std::string P = std::to_string(adonai::constants::PROTOCOL);
    const std::string F = std::to_string(adonai::constants::FHASH);

    if (login_method_.kind == adonai::login::LoginMethodKind::Newly) {
        // Form 1 — minimal.
        return "protocol|" + P + "\nltoken|" + ltoken_ + "\nplatformID|" + platform_id_ + "\n";
    }

    if (std::string(login_token_field(ltoken_)) == "UbiTicket") {
        // Form 2 — JWT-style UbiTicket token.
        std::string s;
        s += "UbiTicket|" + ltoken_ + "\nrequestedName|\nf|1\nprotocol|" + P + "\n";
        s += "game_version|" + game_version_ + "\nfz|" + fz_ + "\ncbits|" + cbits_ +
             "\nplayer_age|" + player_age_ + "\nGDPR|" + gdpr_ + "\nFCMToken|\n";
        s += "category|" + category_ + "\ntotalPlaytime|" + total_playtime_ + "\nklv|" + klv_ +
             "\nsteamToken|" + steam_token_ + "\nhash2|" + hash2_ + "\nmeta|" + meta_ + "\nfhash|" +
             F + "\n";
        s += "rid|" + rid_ + "\nplatformID|" + platform_id_ + "\ndeviceVersion|0\ncountry|" +
             country_ + "\nhash|" + hash_ + "\nmac|" + mac_ + "\nwk|" + wk_ + "\nzf|" + zf_ + "\n";
        return s;
    }

    // Form 3 — default token/ltoken login.
    std::string s;
    s += "protocol|" + P + "\nltoken|" + ltoken_ + "\nplatformID|" + platform_id_ +
         "\nrequestedName|\nf|1\n";
    s += "game_version|" + game_version_ + "\nfz|" + fz_ + "\ncbits|" + cbits_ + "\nplayer_age|" +
         player_age_ + "\nGDPR|" + gdpr_ + "\nFCMToken|\n";
    s += "category|" + category_ + "\ntotalPlaytime|" + total_playtime_ + "\nklv|" + klv_ +
         "\nsteamToken|" + steam_token_ + "\nhash2|" + hash2_ + "\nmeta|" + meta_ + "\nfhash|" + F +
         "\n";
    s += "rid|" + rid_ + "\ndeviceVersion|0\ncountry|" + country_ + "\nhash|" + hash_ + "\nmac|" +
         mac_ + "\nwk|" + wk_ + "\nzf|" + zf_ + "\n";
    return s;
}

// ===========================================================================
// §2.10 build_redirect_packet — subserver logon body (protocol|211)
// ===========================================================================
std::string Bot::build_redirect_packet(const RedirectData& r) const {
    const std::string F = std::to_string(adonai::constants::FHASH);
    std::string s;
    s += "tankIDName|" + r.tank_id_name + "\n";
    s += "tankIDPass|\n";
    s += "requestedName|\n";
    s += "f|1\n";
    s += "protocol|211\n";  // literal 211, NOT PROTOCOL
    s += "game_version|" + game_version_ + "\n";
    s += "fz|" + fz_ + "\n";
    s += "cbits|" + cbits_ + "\n";
    s += "player_age|" + player_age_ + "\n";
    s += "GDPR|" + gdpr_ + "\n";
    s += "FCMToken|\n";
    s += "category|" + category_ + "\n";
    s += "totalPlaytime|" + total_playtime_ + "\n";
    s += "klv|" + klv_ + "\n";
    s += "hash2|" + hash2_ + "\n";
    s += "meta|" + meta_ + "\n";
    s += "fhash|" + F + "\n";
    s += "rid|" + rid_ + "\n";
    s += "platformID|" + platform_id_ + "\n";
    s += "deviceVersion|0\n";
    s += "country|" + country_ + "\n";
    s += "hash|" + hash_ + "\n";
    s += "mac|" + mac_ + "\n";
    s += "wk|" + wk_ + "\n";
    s += "zf|" + zf_ + "\n";
    s += "lmode|" + r.lmode + "\n";
    s += "user|" + r.user + "\n";
    s += "token|" + r.token + "\n";
    s += "UUIDToken|" + r.uuid + "\n";
    if (!r.door_id.empty()) s += "doorID|" + r.door_id + "\n";
    s += "aat|2\n";  // always last
    return s;
}

// ===========================================================================
// §2.3-§2.5 send primitives (channel 0; no-op without a peer)
// ===========================================================================
void Bot::send_text(const std::string& text) {
    if (!peer_id_.has_value()) return;
    auto raw = adonai::protocol::make_text_packet(text);
    emit_traffic("out", "text", raw.size(), redact_packet_text(text));
    host_.peer_send(*peer_id_, 0, raw.data(), raw.size(), true);
}

void Bot::send_game_message(const std::string& text) {
    if (!peer_id_.has_value()) return;
    auto raw = adonai::protocol::make_game_message_packet(text);
    emit_traffic("out", "game_message", raw.size(), redact_packet_text(text));
    host_.peer_send(*peer_id_, 0, raw.data(), raw.size(), true);
}

void Bot::send_game_packet(const adonai::protocol::GameUpdatePacket& pkt, bool reliable) {
    if (!peer_id_.has_value()) return;
    auto raw = adonai::protocol::make_game_packet(pkt);
    emit_traffic("out", "game_update:" + gpt_name(pkt.packet_type), raw.size(),
                 format_game_packet_detail(pkt));
    host_.peer_send(*peer_id_, 0, raw.data(), raw.size(), reliable);
}

}  // namespace adonai::bot
