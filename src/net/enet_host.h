// Adonai — thin C++ wrapper over the vendored C ENet (ported from Mori's
// `BotHost` enum + `Bot::create_host`, port spec 06 §1.3 / §2.2 / §2.3).
//
// One BotHost owns one ENetHost. Its underlying UDP socket may be either a plain
// direct socket or — when a Socks5Config relay is supplied — routed through a
// SOCKS5 UDP-ASSOCIATE proxy. Mori modelled this as a compile-time `enum
// BotHost { Direct, Socks5 }`; in C++ a single class suffices because the SOCKS5
// behaviour lives in the vendored ENet's *patched socket layer*, not in the host
// type (see the patch contract at the bottom of this file + third_party/enet/README.md).
//
// The ENet dependency is guarded by ADONAI_HAVE_ENET so this unit compiles before
// ENet is vendored: without it, create() logs and yields an inert host and every
// method is a safe no-op. The SOCKS5 relay registry + its C-ABI (used by the
// patched enet_socket_send/receive in win32.c) are always compiled.
//
// GT is little-endian, but ENet/SOCKS5 wire ports + IPv6 bytes are network order;
// that is handled inside socks5_udp.* — this wrapper only moves opaque payloads.
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include <cstdint>
#include <optional>
#include <vector>

#include "net/socks5_udp.h"

// Forward-declare the ENet tag types so the header needs no <enet.h>. These match
// classic ENet's `typedef struct _ENetHost ENetHost;` / `_ENetPeer`.
struct _ENetHost;
struct _ENetPeer;

namespace adonai::net {

// ---------------------------------------------------------------------------
// Tuning constants (load-bearing; do not change — port spec 06 §2.3 / §2.8)
// ---------------------------------------------------------------------------

// ENet HostSettings applied to every host (spec 06 §2.3).
inline constexpr std::size_t kHostPeerLimit = 1;
inline constexpr std::size_t kHostChannelLimit = 2;

// SOCKS5 UDP-bind retry policy: `for attempt in 0..4` with 300 ms between tries,
// then the SAFE dead-loopback fallback (NEVER a direct connection — that would
// leak the operator's real IP to Growtopia).
inline constexpr int kSocks5BindMaxAttempts = 4;
inline constexpr int kSocks5BindRetryDelayMs = 300;

// peer_set_timeout applied on Connect (spec 06 §2.8): limit=0 keeps ENet's
// default (32); minimum raised from ENet's aggressive 5 s to 12 s so a brief
// SOCKS5/game-proxy UDP gap can't drop an in-world bot; maximum 30 s so a
// sustained outage still times out cleanly.
inline constexpr std::uint32_t kPeerTimeoutLimit = 0;
inline constexpr std::uint32_t kPeerTimeoutMinMs = 12000;
inline constexpr std::uint32_t kPeerTimeoutMaxMs = 30000;

// Lightweight peer handle (mirrors enet::PeerID — an index into host->peers).
struct PeerId {
    std::uint16_t index = 0;
    bool operator==(const PeerId& o) const { return index == o.index; }
};

// One serviced ENet event, decoupled from ENet types (mirrors Mori's EventNoRef).
struct HostEvent {
    enum class Type { Connect, Disconnect, Receive };
    Type type;
    PeerId peer;
    std::uint32_t data = 0;            // Connect: connect-data; Disconnect: reason code
    std::uint8_t channel_id = 0;       // Receive only
    std::vector<std::uint8_t> packet;  // Receive: owned copy of the payload
};

// ENet host + optional SOCKS5 UDP relay. Move-only; single-owner, single-thread
// (lives on the bot's worker thread — no internal locking). The destructor
// destroys the host and closes the relay's control TCP socket.
class BotHost {
public:
    // Build a host. If `proxy` is non-null: log the proxy URL, then try
    // Socks5UdpSocket::bind_through_proxy up to 4 times (300 ms apart). On success
    // the host's socket is registered with the SOCKS5 relay so the patched ENet
    // socket layer tunnels every datagram. If all 4 attempts fail, DO NOT connect
    // direct — bind a dead 127.0.0.1:0 socket instead so the follow-up connect
    // simply fails and the run loop retries with a fresh proxy (anti-IP-leak).
    // If `proxy` is null: bind 0.0.0.0:0 directly. Never returns a leaking host.
    static BotHost create(const Socks5Config* proxy);

    BotHost() = default;
    ~BotHost();
    BotHost(BotHost&&) noexcept;
    BotHost& operator=(BotHost&&) noexcept;
    BotHost(const BotHost&) = delete;
    BotHost& operator=(const BotHost&) = delete;

    // True when this host tunnels through a SOCKS5 UDP relay.
    bool relayed() const { return relayed_; }
    // True when a real ENet host exists (false only if ENet isn't vendored).
    bool valid() const { return host_ != nullptr; }

    // Service the host once (enet_host_service, timeout 0) and return one event.
    // Returns std::nullopt on "no event" AND on any socket-level error — a
    // transient Windows WSAECONNRESET from a SOCKS5 relay's ICMP port-unreachable
    // must NEVER crash the bot thread; a truly dead relay is still caught by
    // ENet's own timeout → clean Disconnect. On a Connect event the peer's timeout
    // is set to (0, 12s, 30s) before the event is returned (spec 06 §2.8).
    std::optional<HostEvent> next_event();

    // Begin connecting to `addr`. On failure logs and returns false (Mori
    // `.expect` panics; Adonai must not abort the process — the run loop retries).
    bool connect(const sockaddr* addr, socklen_t addr_len,
                 std::size_t channel_count = kHostChannelLimit,
                 std::uint32_t data = 0);

    // Round-trip time in ms (0 if the peer is gone).
    std::uint32_t peer_rtt(PeerId peer) const;

    // Queue a packet to the peer; the send Result is ignored (Mori `.ok()`).
    // GT login/game traffic is reliable on channel 0.
    void peer_send(PeerId peer, std::uint8_t channel,
                   const std::uint8_t* data, std::size_t len, bool reliable = true);

    // Request a graceful disconnect carrying `data` as the reason code.
    void peer_disconnect(PeerId peer, std::uint32_t data);

    // enet_peer_timeout passthrough. The wrapper's own Connect handler already
    // applies (0, 12s, 30s); callers may re-apply explicitly.
    void peer_set_timeout(PeerId peer, std::uint32_t limit,
                          std::uint32_t minimum, std::uint32_t maximum);

private:
    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr;   // the single peer (kHostPeerLimit == 1)
    Socks5UdpSocket relay_;       // owns the control TCP fd for the association
    bool relayed_ = false;

    void destroy();
    _ENetPeer* resolve(PeerId peer) const;
};

// ---------------------------------------------------------------------------
// SOCKS5 relay registry — C-ABI consumed by the vendored ENet socket patch.
//
// The patched win32.c enet_socket_send / enet_socket_receive (see
// third_party/enet/README.md) call these on the host's own socket handle. When a
// relay is registered for that socket:
//   * send    — build the RFC 1928 §7 UDP header for the REAL target address with
//               adonai_socks5_build_header, prepend it to the ENet payload, and
//               sendto the returned relay address instead of the peer. Return the
//               PAYLOAD byte count (not the on-wire length); 0 on WouldBlock /
//               partial; swallow other errors as 0.
//   * receive — recvfrom the socket, adonai_socks5_parse_header to strip the
//               header, report the decapsulated real source to ENet, memmove the
//               payload to the buffer front. On WouldBlock AND any other error
//               (esp. WSAECONNRESET / 10054) report "no packet", never propagate.
// When no relay is registered the patch behaves as stock ENet (direct UDP).
// ---------------------------------------------------------------------------
extern "C" {

// Returns 1 and fills *out_addr (with *out_len) if a SOCKS5 relay is bound to the
// given socket handle ((intptr_t)host->socket); returns 0 otherwise.
int adonai_socks5_relay_for(std::intptr_t socket_handle,
                            struct sockaddr_storage* out_addr, int* out_len);

// Build the per-datagram SOCKS5 UDP request header for `target` into `out`
// (capacity `cap`). Returns the header length (10 IPv4 / 22 IPv6) or -1 on error.
int adonai_socks5_build_header(const struct sockaddr* target,
                               unsigned char* out, int cap);

// Validate + locate the payload inside a relayed datagram. On success returns the
// payload length, sets *payload_off to the header length (payload begins there)
// and fills *src (*src_len) with the real source. Returns -1 on any malformation
// (RSV/FRAG/ATYP/short) — the caller drops the datagram silently.
int adonai_socks5_parse_header(const unsigned char* data, int len,
                               struct sockaddr_storage* src, int* src_len,
                               int* payload_off);

}  // extern "C"

// Register / unregister a relay endpoint for an ENet socket handle. create()
// registers after the host is built; destroy() unregisters before teardown.
void socks5_relay_register(std::intptr_t socket_handle,
                           const sockaddr* relay_addr, socklen_t relay_len);
void socks5_relay_unregister(std::intptr_t socket_handle);

}  // namespace adonai::net
