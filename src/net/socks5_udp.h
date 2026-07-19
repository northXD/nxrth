// Nxrth — SOCKS5 UDP ASSOCIATE client (ported from Mori/socks5.rs).
//
// Two responsibilities live here:
//   1. The SOCKS5 handshake: method negotiation, RFC-1929 user/pass auth, and
//      the UDP ASSOCIATE command over a persistent TCP control connection.
//   2. Per-datagram header build (create_udp_header) / strip (parse_udp_header)
//      per RFC 1928 §7 — IPv4 header = 10 bytes, IPv6 header = 22 bytes, ports
//      and IPv6 address bytes are BIG-ENDIAN (network order).
//
// The actual send/receive datapath is NOT implemented as a socket wrapper here.
// In Mori the wrapper implemented rusty_enet::Socket; in Nxrth this becomes a
// patch to the vendored C ENet socket layer (enet_socket_send /
// enet_socket_receive). That patch (net/enet_socks5_patch.cpp) reuses
// create_udp_header / parse_udp_header from this module and holds a
// Socks5UdpSocket per ENetHost. See the class-level comment below.
//
// GT payloads are little-endian, but that is INSIDE the opaque ENet payload;
// every multi-byte field this module touches on the wire is network order.
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace nxrth::net {

// Shared proxy descriptor. This is the type proxy_pool + enet_host consume when
// pinning one egress proxy per bot. host may be a hostname or a literal IP;
// username/password are both required for RFC-1929 auth (either both or neither).
struct Socks5Config {
    std::string host;
    std::uint16_t port = 0;
    std::optional<std::string> username;
    std::optional<std::string> password;
};

// Handshake error taxonomy (mirrors Rust Socks5Error). to_string() reproduces
// the verbatim io::Error messages the Rust used in logs / error strings.
enum class Socks5Error {
    Io,
    InvalidResponse,
    AuthenticationFailed,
    UnsupportedVersion,
    ConnectionRefused,
    NetworkUnreachable,
    HostUnreachable,
    ConnectionReset,
    CommandNotSupported,
    AddressTypeNotSupported,
    GeneralFailure,
};

// Verbatim message for each variant (matches the From<Socks5Error> for io::Error
// message column in the spec). Io carries the underlying OS error text instead.
const char* to_string(Socks5Error e);

// Thrown by bind_through_proxy on any handshake / socket failure. Callers that
// distinguish causes can read .code; the connect-retry path only needs to know
// it failed so it can pick a fresh proxy.
class Socks5Exception : public std::runtime_error {
public:
    Socks5Exception(Socks5Error code, std::string msg, int os_errno = 0)
        : std::runtime_error(std::move(msg)), code(code), os_errno(os_errno) {}
    Socks5Error code;
    int os_errno;
};

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

// ENET_PROTOCOL_MAXIMUM_MTU — the receive buffer is exactly this size.
inline constexpr std::size_t MTU_MAX = 4096;

// Per-bot / per-ENet-host SOCKS5 relay context.
//
// Owns three resources whose lifetimes are coupled:
//   * control_stream_ — the SOCKS5 TCP control connection. MUST stay open for
//     the whole life of the UDP association; the relay tears the association
//     down if it closes. Never close it right after the handshake.
//   * udp_socket_     — the bound, non-blocking local UDP socket. All datagrams
//     are physically sent to relay_addr_ (NOT to the game server); the real
//     target is encoded in the per-datagram SOCKS5 header.
//   * relay_addr_     — the UDP relay endpoint returned by UDP ASSOCIATE.
//
// Single-owner, single-thread: it lives on the bot's worker thread and its
// send/receive helpers run inside that thread's ENet service tick. No locking.
// Move-only; the destructor closes both sockets.
class Socks5UdpSocket {
public:
    // Performs the full handshake synchronously (10s TCP connect, 15s control
    // r/w timeouts) then binds + non-blocks a local UDP socket. Throws
    // Socks5Exception on any failure. local_addr==nullptr binds 0.0.0.0:0.
    static Socks5UdpSocket bind_through_proxy(const Socks5Config& proxy,
                                              const sockaddr* local_addr = nullptr,
                                              socklen_t local_len = 0);

    Socks5UdpSocket() = default;
    ~Socks5UdpSocket();
    Socks5UdpSocket(Socks5UdpSocket&&) noexcept;
    Socks5UdpSocket& operator=(Socks5UdpSocket&&) noexcept;
    Socks5UdpSocket(const Socks5UdpSocket&) = delete;
    Socks5UdpSocket& operator=(const Socks5UdpSocket&) = delete;

    bool valid() const { return udp_socket_ != kInvalidSocket; }
    socket_t udp_socket() const { return udp_socket_; }
    socket_t control_stream() const { return control_stream_; }
    const sockaddr* relay_addr() const {
        return reinterpret_cast<const sockaddr*>(&relay_addr_);
    }
    socklen_t relay_addr_len() const { return relay_addr_len_; }

    // --- Per-datagram header codec (used by the ENet socket-layer patch) ------

    // Builds the RFC 1928 §7 UDP request header targeting the REAL game server
    // `target_addr` (NOT the relay). IPv4 -> 10 bytes, IPv6 -> 22 bytes. The
    // caller appends the ENet payload after this header, then sends the whole
    // thing to relay_addr(). Does not depend on relay_addr_.
    static std::vector<std::uint8_t> create_udp_header(const sockaddr* target_addr);

    // Validates + splits an incoming relayed datagram. On success writes the
    // decapsulated real source address into out_src (out_src_len set) and points
    // (payload, payload_len) at the ENet payload inside `data`, then returns
    // true. On any malformation returns false and (if err!=nullptr) sets the
    // verbatim reason string. The receive path drops false silently (Ok(None)).
    static bool parse_udp_header(const std::uint8_t* data, std::size_t len,
                                 sockaddr_storage& out_src, socklen_t& out_src_len,
                                 const std::uint8_t** payload, std::size_t* payload_len,
                                 std::string* err = nullptr);

    // NOTE: send() / receive() are intentionally absent. The datapath lives in
    // the ENet patch (net/enet_socks5_patch.cpp), which must preserve:
    //   * send returns ENet-PAYLOAD byte count on full send, 0 on partial /
    //     WouldBlock (ENet reads 0 as "retry", not an error); only hard errors
    //     propagate. Physical sendto() targets relay_addr(); header encodes the
    //     real remote passed by ENet.
    //   * receive returns the decapsulated real source + payload moved to the
    //     front of ENet's buffer; WouldBlock and ANY other recv error (notably
    //     Windows WSAECONNRESET / 10054 from a prior send's ICMP port-unreach)
    //     become "no packet this tick", never propagated.

private:
    socket_t udp_socket_ = kInvalidSocket;
    socket_t control_stream_ = kInvalidSocket;
    sockaddr_storage relay_addr_{};
    socklen_t relay_addr_len_ = 0;

    void close_all();
};

}  // namespace nxrth::net
