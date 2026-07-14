// Adonai — SOCKS5 UDP ASSOCIATE client implementation (from Mori/socks5.rs).
#include "net/socks5_udp.h"

#ifdef _WIN32
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cstring>
#include <mutex>

namespace adonai::net {

// ---------------------------------------------------------------------------
// Platform shims
// ---------------------------------------------------------------------------

#ifdef _WIN32
static int last_sock_error() { return WSAGetLastError(); }
static void close_socket(socket_t s) { closesocket(s); }
static bool would_block(int e) { return e == WSAEWOULDBLOCK; }

static void ensure_winsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}
#else
static int last_sock_error() { return errno; }
static void close_socket(socket_t s) { ::close(s); }
static bool would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN; }
static void ensure_winsock() {}
#endif

// ---------------------------------------------------------------------------
// Error text (verbatim io::Error messages from the spec)
// ---------------------------------------------------------------------------

const char* to_string(Socks5Error e) {
    switch (e) {
        case Socks5Error::Io: return "SOCKS5 I/O error";
        case Socks5Error::InvalidResponse: return "Invalid SOCKS5 response";
        case Socks5Error::AuthenticationFailed: return "SOCKS5 authentication failed";
        case Socks5Error::UnsupportedVersion: return "Unsupported SOCKS5 version";
        case Socks5Error::ConnectionRefused: return "SOCKS5 connection refused";
        case Socks5Error::NetworkUnreachable: return "Network unreachable";
        case Socks5Error::HostUnreachable: return "Host unreachable";
        case Socks5Error::ConnectionReset: return "Connection reset";
        case Socks5Error::CommandNotSupported: return "Command not supported";
        case Socks5Error::AddressTypeNotSupported: return "Address type not supported";
        case Socks5Error::GeneralFailure: return "General SOCKS5 failure";
    }
    return "Unknown SOCKS5 error";
}

[[noreturn]] static void fail(Socks5Error code) {
    throw Socks5Exception(code, to_string(code));
}
[[noreturn]] static void fail_io(const char* what) {
    throw Socks5Exception(Socks5Error::Io, std::string(what), last_sock_error());
}

// ---------------------------------------------------------------------------
// sockaddr helpers (ports/IPv6 are network order == wire order)
// ---------------------------------------------------------------------------

static std::uint16_t addr_port_net(const sockaddr_storage& ss) {
    if (ss.ss_family == AF_INET)
        return reinterpret_cast<const sockaddr_in&>(ss).sin_port;
    return reinterpret_cast<const sockaddr_in6&>(ss).sin6_port;
}
static void addr_set_port_net(sockaddr_storage& ss, std::uint16_t net_port) {
    if (ss.ss_family == AF_INET)
        reinterpret_cast<sockaddr_in&>(ss).sin_port = net_port;
    else
        reinterpret_cast<sockaddr_in6&>(ss).sin6_port = net_port;
}
static socklen_t addr_len(const sockaddr_storage& ss) {
    return ss.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
}
static bool addr_is_unspecified(const sockaddr_storage& ss) {
    if (ss.ss_family == AF_INET)
        return reinterpret_cast<const sockaddr_in&>(ss).sin_addr.s_addr == 0;
    if (ss.ss_family == AF_INET6) {
        const auto& a = reinterpret_cast<const sockaddr_in6&>(ss).sin6_addr;
        return IN6_IS_ADDR_UNSPECIFIED(&a);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Blocking read_exact / write_all over the control TCP socket
// ---------------------------------------------------------------------------

static void write_all(socket_t s, const std::uint8_t* buf, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        int sent = ::send(s, reinterpret_cast<const char*>(buf + off),
                          static_cast<int>(n - off), 0);
        if (sent <= 0) fail_io("SOCKS5 control send failed");
        off += static_cast<std::size_t>(sent);
    }
}
static void read_exact(socket_t s, std::uint8_t* buf, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        int got = ::recv(s, reinterpret_cast<char*>(buf + off),
                         static_cast<int>(n - off), 0);
        if (got == 0) fail(Socks5Error::InvalidResponse);  // peer closed mid-reply
        if (got < 0) fail_io("SOCKS5 control recv failed/timeout");
        off += static_cast<std::size_t>(got);
    }
}

// ---------------------------------------------------------------------------
// Handshake stages
// ---------------------------------------------------------------------------

// §2.3 method negotiation.
static void negotiate_auth_method(socket_t s, bool use_auth) {
    if (use_auth) {
        const std::uint8_t req[] = {0x05, 0x02, 0x00, 0x02};
        write_all(s, req, sizeof(req));
    } else {
        const std::uint8_t req[] = {0x05, 0x01, 0x00};
        write_all(s, req, sizeof(req));
    }
    std::uint8_t resp[2];
    read_exact(s, resp, 2);
    if (resp[0] != 0x05) fail(Socks5Error::UnsupportedVersion);
    switch (resp[1]) {
        case 0x00: return;
        case 0x02: return;
        case 0xFF: fail(Socks5Error::AuthenticationFailed);
        default: fail(Socks5Error::InvalidResponse);
    }
}

// §2.4 RFC-1929 user/pass auth. ULEN/PLEN are single bytes -> truncate via (u8).
static void authenticate(socket_t s, const std::string& user, const std::string& pass) {
    std::vector<std::uint8_t> req;
    req.push_back(0x01);
    req.push_back(static_cast<std::uint8_t>(user.size()));
    req.insert(req.end(), user.begin(), user.end());
    req.push_back(static_cast<std::uint8_t>(pass.size()));
    req.insert(req.end(), pass.begin(), pass.end());
    write_all(s, req.data(), req.size());

    std::uint8_t resp[2];
    read_exact(s, resp, 2);
    if (!(resp[0] == 0x01 && resp[1] == 0x00)) fail(Socks5Error::AuthenticationFailed);
}

// §2.5 UDP ASSOCIATE. Writes the hardcoded 10-byte request, parses the bound
// relay endpoint into out.
static void udp_associate(socket_t s, sockaddr_storage& out, socklen_t& out_len) {
    const std::uint8_t req[] = {0x05, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    write_all(s, req, sizeof(req));

    std::uint8_t head[4];
    read_exact(s, head, 4);  // VER REP RSV ATYP
    if (head[0] != 0x05) fail(Socks5Error::UnsupportedVersion);
    switch (head[1]) {  // REP
        case 0x00: break;
        case 0x01: fail(Socks5Error::GeneralFailure);
        case 0x02: fail(Socks5Error::ConnectionRefused);
        case 0x03: fail(Socks5Error::NetworkUnreachable);
        case 0x04: fail(Socks5Error::HostUnreachable);
        case 0x05: fail(Socks5Error::ConnectionRefused);
        case 0x06: fail(Socks5Error::ConnectionReset);
        case 0x07: fail(Socks5Error::CommandNotSupported);
        case 0x08: fail(Socks5Error::AddressTypeNotSupported);
        default: fail(Socks5Error::InvalidResponse);
    }

    std::memset(&out, 0, sizeof(out));
    switch (head[3]) {  // ATYP
        case 0x01: {  // IPv4: 4 addr bytes + 2 BE port
            std::uint8_t body[6];
            read_exact(s, body, 6);
            auto& v4 = reinterpret_cast<sockaddr_in&>(out);
            v4.sin_family = AF_INET;
            std::memcpy(&v4.sin_addr, body, 4);       // network order == wire order
            std::memcpy(&v4.sin_port, body + 4, 2);   // BE on wire -> sin_port is BE
            out_len = sizeof(sockaddr_in);
            return;
        }
        case 0x04: {  // IPv6: 16 addr bytes + 2 BE port
            std::uint8_t body[18];
            read_exact(s, body, 18);
            auto& v6 = reinterpret_cast<sockaddr_in6&>(out);
            v6.sin6_family = AF_INET6;
            std::memcpy(&v6.sin6_addr, body, 16);
            std::memcpy(&v6.sin6_port, body + 16, 2);
            out_len = sizeof(sockaddr_in6);
            return;
        }
        case 0x03:  // domain in bind reply -> refused
            fail(Socks5Error::InvalidResponse);
        default:
            fail(Socks5Error::AddressTypeNotSupported);
    }
}

// §2.2 driver.
static void socks5_handshake(socket_t s, const Socks5Config& cfg,
                             sockaddr_storage& relay, socklen_t& relay_len) {
    const bool use_auth = cfg.username.has_value() && cfg.password.has_value();
    negotiate_auth_method(s, use_auth);
    if (use_auth) authenticate(s, *cfg.username, *cfg.password);
    udp_associate(s, relay, relay_len);
}

// ---------------------------------------------------------------------------
// TCP connect with a 10s timeout
// ---------------------------------------------------------------------------

static socket_t connect_with_timeout(const sockaddr* addr, socklen_t addr_len_,
                                     int timeout_secs) {
    socket_t s = ::socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) fail_io("failed to create SOCKS5 control socket");

    // Non-blocking connect + select(writable) so a half-open proxy cannot block
    // the worker thread forever (the handshake runs inside the bot service loop).
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif

    int rc = ::connect(s, addr, addr_len_);
    if (rc != 0 && !would_block(last_sock_error())
#ifndef _WIN32
        && last_sock_error() != EINPROGRESS
#endif
    ) {
        close_socket(s);
        fail(Socks5Error::ConnectionRefused);
    }
    if (rc != 0) {
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        timeval tv{timeout_secs, 0};
        int sel = ::select(static_cast<int>(s) + 1, nullptr, &wr, nullptr, &tv);
        if (sel <= 0) {  // 0 = timed out, <0 = error
            close_socket(s);
            fail(Socks5Error::ConnectionRefused);
        }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &sl);
        if (soerr != 0) {
            close_socket(s);
            fail(Socks5Error::ConnectionRefused);
        }
    }

    // Back to blocking for the synchronous handshake (guarded by r/w timeouts).
#ifdef _WIN32
    u_long bl = 0;
    ioctlsocket(s, FIONBIO, &bl);
#else
    fcntl(s, F_SETFL, fl);
#endif
    return s;
}

static void set_rw_timeout(socket_t s, int secs) {
#ifdef _WIN32
    DWORD ms = static_cast<DWORD>(secs) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv{secs, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// ---------------------------------------------------------------------------
// bind_through_proxy (§2.1)
// ---------------------------------------------------------------------------

Socks5UdpSocket Socks5UdpSocket::bind_through_proxy(const Socks5Config& proxy,
                                                    const sockaddr* local_addr,
                                                    socklen_t local_len) {
    ensure_winsock();

    // Resolve proxy host:port -> proxy_addr.
    sockaddr_storage proxy_addr{};
    socklen_t proxy_len = 0;
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        char port_s[16];
        std::snprintf(port_s, sizeof(port_s), "%u", static_cast<unsigned>(proxy.port));
        addrinfo* res = nullptr;
        if (getaddrinfo(proxy.host.c_str(), port_s, &hints, &res) != 0 || !res)
            fail(Socks5Error::HostUnreachable);
        std::memcpy(&proxy_addr, res->ai_addr, res->ai_addrlen);
        proxy_len = static_cast<socklen_t>(res->ai_addrlen);
        freeaddrinfo(res);
    }

    // 1-2. Connect (10s) + control r/w timeouts (15s).
    socket_t control = connect_with_timeout(reinterpret_cast<const sockaddr*>(&proxy_addr),
                                            proxy_len, 10);
    set_rw_timeout(control, 15);

    // 3. Handshake -> relay endpoint.
    sockaddr_storage relay{};
    socklen_t relay_len = 0;
    try {
        socks5_handshake(control, proxy, relay, relay_len);
    } catch (...) {
        close_socket(control);
        throw;
    }

    // 4. Relay returned 0.0.0.0 / :: -> the real relay lives at the proxy IP.
    //    Keep the relay port, adopt the proxy's address (and family).
    if (addr_is_unspecified(relay)) {
        std::uint16_t relay_port = addr_port_net(relay);
        relay = proxy_addr;
        relay_len = proxy_len;
        addr_set_port_net(relay, relay_port);
    }

    // 5. Bind local UDP socket + non-blocking.
    sockaddr_storage default_local{};
    if (!local_addr) {
        auto& v4 = reinterpret_cast<sockaddr_in&>(default_local);
        v4.sin_family = AF_INET;
        v4.sin_addr.s_addr = INADDR_ANY;
        v4.sin_port = 0;
        local_addr = reinterpret_cast<const sockaddr*>(&default_local);
        local_len = sizeof(sockaddr_in);
    }
    socket_t udp = ::socket(local_addr->sa_family, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == kInvalidSocket) {
        close_socket(control);
        fail_io("failed to create SOCKS5 UDP socket");
    }
    if (::bind(udp, local_addr, local_len) != 0) {
        close_socket(udp);
        close_socket(control);
        fail_io("failed to bind SOCKS5 UDP socket");
    }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(udp, FIONBIO, &nb);
#else
    int fl = fcntl(udp, F_GETFL, 0);
    fcntl(udp, F_SETFL, fl | O_NONBLOCK);
#endif

    Socks5UdpSocket out;
    out.udp_socket_ = udp;
    out.control_stream_ = control;
    out.relay_addr_ = relay;
    out.relay_addr_len_ = relay_len;
    return out;
}

// ---------------------------------------------------------------------------
// Per-datagram header codec (§2.6 / §2.7)
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> Socks5UdpSocket::create_udp_header(const sockaddr* target_addr) {
    std::vector<std::uint8_t> h;
    h.push_back(0x00);  // RSV
    h.push_back(0x00);  // RSV
    h.push_back(0x00);  // FRAG
    if (target_addr->sa_family == AF_INET) {
        const auto& v4 = *reinterpret_cast<const sockaddr_in*>(target_addr);
        h.push_back(0x01);  // ATYP IPv4
        const auto* ip = reinterpret_cast<const std::uint8_t*>(&v4.sin_addr);
        h.insert(h.end(), ip, ip + 4);
        const auto* port = reinterpret_cast<const std::uint8_t*>(&v4.sin_port);
        h.insert(h.end(), port, port + 2);  // sin_port already BE
    } else {
        const auto& v6 = *reinterpret_cast<const sockaddr_in6*>(target_addr);
        h.push_back(0x04);  // ATYP IPv6
        const auto* ip = reinterpret_cast<const std::uint8_t*>(&v6.sin6_addr);
        h.insert(h.end(), ip, ip + 16);
        const auto* port = reinterpret_cast<const std::uint8_t*>(&v6.sin6_port);
        h.insert(h.end(), port, port + 2);
    }
    return h;
}

bool Socks5UdpSocket::parse_udp_header(const std::uint8_t* data, std::size_t len,
                                       sockaddr_storage& out_src, socklen_t& out_src_len,
                                       const std::uint8_t** payload, std::size_t* payload_len,
                                       std::string* err) {
    auto reject = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    if (len < 10) return reject("UDP header too short");
    if (data[0] != 0 || data[1] != 0) return reject("Invalid RSV field");
    if (data[2] != 0) return reject("Fragmentation not supported");

    std::memset(&out_src, 0, sizeof(out_src));
    switch (data[3]) {  // ATYP
        case 0x01: {  // IPv4 -> header 10 bytes
            auto& v4 = reinterpret_cast<sockaddr_in&>(out_src);
            v4.sin_family = AF_INET;
            std::memcpy(&v4.sin_addr, data + 4, 4);
            std::memcpy(&v4.sin_port, data + 8, 2);  // BE
            out_src_len = sizeof(sockaddr_in);
            *payload = data + 10;
            *payload_len = len - 10;
            return true;
        }
        case 0x04: {  // IPv6 -> header 22 bytes
            if (len < 22) return reject("IPv6 header too short");
            auto& v6 = reinterpret_cast<sockaddr_in6&>(out_src);
            v6.sin6_family = AF_INET6;
            std::memcpy(&v6.sin6_addr, data + 4, 16);
            std::memcpy(&v6.sin6_port, data + 20, 2);  // BE
            out_src_len = sizeof(sockaddr_in6);
            *payload = data + 22;
            *payload_len = len - 22;
            return true;
        }
        case 0x03:
            return reject("Domain name addresses not supported");
        default:
            return reject("Unsupported address type");
    }
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

void Socks5UdpSocket::close_all() {
    if (udp_socket_ != kInvalidSocket) {
        close_socket(udp_socket_);
        udp_socket_ = kInvalidSocket;
    }
    if (control_stream_ != kInvalidSocket) {
        close_socket(control_stream_);
        control_stream_ = kInvalidSocket;
    }
}

Socks5UdpSocket::~Socks5UdpSocket() { close_all(); }

Socks5UdpSocket::Socks5UdpSocket(Socks5UdpSocket&& o) noexcept
    : udp_socket_(o.udp_socket_),
      control_stream_(o.control_stream_),
      relay_addr_(o.relay_addr_),
      relay_addr_len_(o.relay_addr_len_) {
    o.udp_socket_ = kInvalidSocket;
    o.control_stream_ = kInvalidSocket;
}

Socks5UdpSocket& Socks5UdpSocket::operator=(Socks5UdpSocket&& o) noexcept {
    if (this != &o) {
        close_all();
        udp_socket_ = o.udp_socket_;
        control_stream_ = o.control_stream_;
        relay_addr_ = o.relay_addr_;
        relay_addr_len_ = o.relay_addr_len_;
        o.udp_socket_ = kInvalidSocket;
        o.control_stream_ = kInvalidSocket;
    }
    return *this;
}

// Silence unused-helper warnings for platform builds that don't touch them all.
static void adonai_socks5_touch() {
    (void)&addr_len;
    (void)&would_block;
}

}  // namespace adonai::net
