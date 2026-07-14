// Adonai — BotHost (thin wrapper over vendored C ENet) + SOCKS5 relay registry.
#include "net/enet_host.h"

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "core/logger.h"

#ifdef ADONAI_HAVE_ENET
#include <enet/enet.h>
#endif

namespace adonai::net {

// ---------------------------------------------------------------------------
// SOCKS5 relay registry (always compiled — the ENet patch links against it)
// ---------------------------------------------------------------------------

namespace {

struct RelayEntry {
    sockaddr_storage addr{};
    int len = 0;
};

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::map<std::intptr_t, RelayEntry>& registry() {
    static std::map<std::intptr_t, RelayEntry> r;
    return r;
}

// Build "socks5://[user:pass@]host:port" for the operator log (matches Mori's
// Socks5Config::to_url — unredacted, this is a local log).
std::string proxy_to_url(const Socks5Config& p) {
    std::string s = "socks5://";
    if (p.username && p.password) {
        s += *p.username + ":" + *p.password + "@";
    }
    s += p.host + ":" + std::to_string(p.port);
    return s;
}

}  // namespace

void socks5_relay_register(std::intptr_t socket_handle,
                           const sockaddr* relay_addr, socklen_t relay_len) {
    if (!relay_addr || relay_len <= 0 ||
        relay_len > static_cast<socklen_t>(sizeof(sockaddr_storage))) {
        return;
    }
    RelayEntry e;
    std::memcpy(&e.addr, relay_addr, static_cast<std::size_t>(relay_len));
    e.len = static_cast<int>(relay_len);
    std::lock_guard<std::mutex> lk(registry_mutex());
    registry()[socket_handle] = e;
}

void socks5_relay_unregister(std::intptr_t socket_handle) {
    std::lock_guard<std::mutex> lk(registry_mutex());
    registry().erase(socket_handle);
}

extern "C" int adonai_socks5_relay_for(std::intptr_t socket_handle,
                                       struct sockaddr_storage* out_addr,
                                       int* out_len) {
    std::lock_guard<std::mutex> lk(registry_mutex());
    auto it = registry().find(socket_handle);
    if (it == registry().end()) return 0;
    if (out_addr) std::memcpy(out_addr, &it->second.addr, sizeof(sockaddr_storage));
    if (out_len) *out_len = it->second.len;
    return 1;
}

extern "C" int adonai_socks5_build_header(const struct sockaddr* target,
                                          unsigned char* out, int cap) {
    if (!target || !out || cap < 0) return -1;
    std::vector<std::uint8_t> hdr = Socks5UdpSocket::create_udp_header(target);
    if (static_cast<int>(hdr.size()) > cap) return -1;
    std::memcpy(out, hdr.data(), hdr.size());
    return static_cast<int>(hdr.size());
}

extern "C" int adonai_socks5_parse_header(const unsigned char* data, int len,
                                          struct sockaddr_storage* src,
                                          int* src_len, int* payload_off) {
    if (!data || len < 0) return -1;
    sockaddr_storage local_src{};
    socklen_t local_src_len = 0;
    const std::uint8_t* payload = nullptr;
    std::size_t payload_len = 0;
    bool ok = Socks5UdpSocket::parse_udp_header(
        reinterpret_cast<const std::uint8_t*>(data),
        static_cast<std::size_t>(len), local_src, local_src_len, &payload,
        &payload_len, nullptr);
    if (!ok || payload == nullptr) return -1;
    if (src) std::memcpy(src, &local_src, sizeof(sockaddr_storage));
    if (src_len) *src_len = static_cast<int>(local_src_len);
    if (payload_off) {
        *payload_off = static_cast<int>(payload -
                                        reinterpret_cast<const std::uint8_t*>(data));
    }
    return static_cast<int>(payload_len);
}

// ---------------------------------------------------------------------------
// BotHost
// ---------------------------------------------------------------------------

BotHost::BotHost(BotHost&& o) noexcept
    : host_(o.host_), peer_(o.peer_), relay_(std::move(o.relay_)),
      relayed_(o.relayed_) {
    o.host_ = nullptr;
    o.peer_ = nullptr;
    o.relayed_ = false;
}

BotHost& BotHost::operator=(BotHost&& o) noexcept {
    if (this != &o) {
        destroy();
        host_ = o.host_;
        peer_ = o.peer_;
        relay_ = std::move(o.relay_);
        relayed_ = o.relayed_;
        o.host_ = nullptr;
        o.peer_ = nullptr;
        o.relayed_ = false;
    }
    return *this;
}

BotHost::~BotHost() { destroy(); }

#ifdef ADONAI_HAVE_ENET

namespace {

std::once_flag g_enet_init_once;
void ensure_enet() {
    std::call_once(g_enet_init_once, [] { enet_initialize(); });
}

// Configure the shared HostSettings (spec 06 §2.3): RangeCoder compressor,
// crc32 checksum, GT "new packet" framing.
void configure_host(ENetHost* host) {
    enet_host_compress_with_range_coder(host);
    host->checksum = enet_crc32;
#if defined(ADONAI_ENET_HAS_NEW_PACKET)
    host->usingNewPacket = 1;  // GT 5.x framing; provided by the vendored fork
#endif
}

// Fill an ENetAddress from a bind interface (IPv4). host in network order.
ENetAddress make_bind_addr(std::uint32_t host_net, std::uint16_t port_host) {
    ENetAddress a{};
    a.host = host_net;   // ENET_HOST_ANY (0) or loopback, already network order
    a.port = port_host;  // ENet ports are host order
    return a;
}

}  // namespace

BotHost BotHost::create(const Socks5Config* proxy) {
    ensure_enet();
    BotHost self;

    if (proxy) {
        adonai::log("[Bot] Connecting via proxy " + proxy_to_url(*proxy));

        // 0..4: up to 4 SOCKS5 UDP-ASSOCIATE attempts, 300 ms apart.
        for (int attempt = 0; attempt < kSocks5BindMaxAttempts; ++attempt) {
            try {
                Socks5UdpSocket relay = Socks5UdpSocket::bind_through_proxy(*proxy);
                ENetAddress bind = make_bind_addr(ENET_HOST_ANY, 0);
                ENetHost* host = enet_host_create(&bind, kHostPeerLimit,
                                                  kHostChannelLimit, 0, 0);
                if (host) {
                    configure_host(host);
                    // Route this host's socket through the relay. ENet's own UDP
                    // socket sends to relay_addr; the association's TCP control fd
                    // is kept alive inside `relay` (moved into the BotHost) for the
                    // life of the host.
                    socks5_relay_register(static_cast<std::intptr_t>(host->socket),
                                          relay.relay_addr(), relay.relay_addr_len());
                    self.host_ = reinterpret_cast<_ENetHost*>(host);
                    self.relay_ = std::move(relay);
                    self.relayed_ = true;
                    return self;
                }
            } catch (const std::exception&) {
                // fall through to retry / fallback
            }
            if (attempt < kSocks5BindMaxAttempts - 1) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kSocks5BindRetryDelayMs));
            }
        }

        // SAFE fallback: all attempts failed. Do NOT connect direct (would leak
        // the operator's real IP). Bind a dead 127.0.0.1:0 socket — the follow-up
        // connect to a public game server physically cannot leave a loopback-bound
        // socket, so it fails and the run loop retries with a fresh proxy.
        adonai::log("[Bot] Proxy UDP bind failed after retries — NOT falling back "
                    "to direct (would leak real IP); will retry with a fresh proxy");
        ENetAddress bind = make_bind_addr(htonl(INADDR_LOOPBACK), 0);
        ENetHost* host = enet_host_create(&bind, kHostPeerLimit,
                                          kHostChannelLimit, 0, 0);
        if (host) {
            configure_host(host);
            self.host_ = reinterpret_cast<_ENetHost*>(host);
        }
        self.relayed_ = false;
        return self;
    }

    // No proxy configured: legitimate direct host bound to 0.0.0.0:0.
    ENetAddress bind = make_bind_addr(ENET_HOST_ANY, 0);
    ENetHost* host = enet_host_create(&bind, kHostPeerLimit, kHostChannelLimit, 0, 0);
    if (host) {
        configure_host(host);
        self.host_ = reinterpret_cast<_ENetHost*>(host);
    }
    self.relayed_ = false;
    return self;
}

void BotHost::destroy() {
    if (host_) {
        ENetHost* host = reinterpret_cast<ENetHost*>(host_);
        socks5_relay_unregister(static_cast<std::intptr_t>(host->socket));
        enet_host_destroy(host);
        host_ = nullptr;
    }
    peer_ = nullptr;
    relayed_ = false;
    // relay_'s destructor closes the control TCP fd (association teardown).
}

_ENetPeer* BotHost::resolve(PeerId peer) const {
    if (!host_) return nullptr;
    ENetHost* host = reinterpret_cast<ENetHost*>(host_);
    if (peer.index >= host->peerCount) return nullptr;
    return reinterpret_cast<_ENetPeer*>(&host->peers[peer.index]);
}

std::optional<HostEvent> BotHost::next_event() {
    if (!host_) return std::nullopt;
    ENetHost* host = reinterpret_cast<ENetHost*>(host_);
    ENetEvent ev{};
    int r = enet_host_service(host, &ev, 0);
    if (r <= 0) return std::nullopt;  // 0 = no event; <0 = error (swallowed)

    HostEvent out{};
    PeerId pid{};
    if (ev.peer) {
        pid.index = static_cast<std::uint16_t>(ev.peer - host->peers);
    }
    out.peer = pid;

    switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            peer_ = reinterpret_cast<_ENetPeer*>(ev.peer);
            out.type = HostEvent::Type::Connect;
            out.data = ev.data;
            // Raise the peer timeout to (0, 12s, 30s) so a brief UDP relay gap
            // can't drop an in-world bot (spec 06 §2.8).
            enet_peer_timeout(ev.peer, kPeerTimeoutLimit, kPeerTimeoutMinMs,
                              kPeerTimeoutMaxMs);
            return out;
        case ENET_EVENT_TYPE_DISCONNECT:
            if (peer_ == reinterpret_cast<_ENetPeer*>(ev.peer)) peer_ = nullptr;
            out.type = HostEvent::Type::Disconnect;
            out.data = ev.data;  // 0 = local/transport timeout; else server code
            return out;
        case ENET_EVENT_TYPE_RECEIVE: {
            out.type = HostEvent::Type::Receive;
            out.channel_id = ev.channelID;
            if (ev.packet) {
                out.packet.assign(ev.packet->data,
                                  ev.packet->data + ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
            }
            return out;
        }
        default:
            return std::nullopt;
    }
}

bool BotHost::connect(const sockaddr* addr, socklen_t addr_len,
                      std::size_t channel_count, std::uint32_t data) {
    if (!host_ || !addr) return false;
    ENetHost* host = reinterpret_cast<ENetHost*>(host_);

    ENetAddress ea{};
    if (addr->sa_family == AF_INET && addr_len >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
        ea.host = in->sin_addr.s_addr;        // network order
        ea.port = ntohs(in->sin_port);        // ENet wants host order
    } else {
        adonai::log("[Bot] connect: unsupported address family — attempt failed");
        return false;
    }

    ENetPeer* peer = enet_host_connect(host, &ea, channel_count, data);
    if (!peer) {
        // Mori `.expect("connect failed")` panics; Adonai treats it as a failed
        // attempt so the run loop can retry (never abort the process).
        adonai::log("[Bot] connect failed (no free peer) — will retry");
        return false;
    }
    peer_ = reinterpret_cast<_ENetPeer*>(peer);
    return true;
}

std::uint32_t BotHost::peer_rtt(PeerId peer) const {
    _ENetPeer* p = resolve(peer);
    if (!p) return 0;
    return reinterpret_cast<ENetPeer*>(p)->roundTripTime;
}

void BotHost::peer_send(PeerId peer, std::uint8_t channel,
                        const std::uint8_t* data, std::size_t len, bool reliable) {
    _ENetPeer* p = resolve(peer);
    if (!p) return;
    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(data, len, flags);
    if (!packet) return;
    if (enet_peer_send(reinterpret_cast<ENetPeer*>(p), channel, packet) < 0) {
        // enet_peer_send does not free on failure; Mori ignores the Result (.ok()).
        enet_packet_destroy(packet);
    }
}

void BotHost::peer_disconnect(PeerId peer, std::uint32_t data) {
    _ENetPeer* p = resolve(peer);
    if (!p) return;
    enet_peer_disconnect(reinterpret_cast<ENetPeer*>(p), data);
}

void BotHost::peer_set_timeout(PeerId peer, std::uint32_t limit,
                               std::uint32_t minimum, std::uint32_t maximum) {
    _ENetPeer* p = resolve(peer);
    if (!p) return;
    enet_peer_timeout(reinterpret_cast<ENetPeer*>(p), limit, minimum, maximum);
}

#else  // !ADONAI_HAVE_ENET — inert build so this unit compiles before vendoring.

BotHost BotHost::create(const Socks5Config* proxy) {
    if (proxy) {
        adonai::log("[Bot] Connecting via proxy " + proxy_to_url(*proxy));
    }
    adonai::log("[Bot] ENet not vendored (ADONAI_HAVE_ENET undefined) — "
                "host is inert");
    return BotHost{};
}

void BotHost::destroy() {
    host_ = nullptr;
    peer_ = nullptr;
    relayed_ = false;
}

_ENetPeer* BotHost::resolve(PeerId) const { return nullptr; }
std::optional<HostEvent> BotHost::next_event() { return std::nullopt; }
bool BotHost::connect(const sockaddr*, socklen_t, std::size_t, std::uint32_t) {
    return false;
}
std::uint32_t BotHost::peer_rtt(PeerId) const { return 0; }
void BotHost::peer_send(PeerId, std::uint8_t, const std::uint8_t*, std::size_t, bool) {}
void BotHost::peer_disconnect(PeerId, std::uint32_t) {}
void BotHost::peer_set_timeout(PeerId, std::uint32_t, std::uint32_t, std::uint32_t) {}

#endif  // ADONAI_HAVE_ENET

}  // namespace adonai::net
