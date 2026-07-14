# Vendored ENet (SOCKS5-UDP patched)

Adonai vendors the C ENet library here and patches its socket layer so every
datagram can be relayed through a SOCKS5 UDP ASSOCIATE proxy (the same thing
Mori's `rusty_enet` `Socket` trait impl did).

## Drop-in steps

1. Copy the ENet source (`enet.h`, `callbacks.c`, `host.c`, `list.c`, `packet.c`,
   `peer.c`, `protocol.c`, `unix.c`, `win32.c`, and `include/`) into this folder.
2. Add a `CMakeLists.txt` here that builds a static `enet` target. Adonai's root
   CMake auto-detects it (`ADONAI_HAVE_ENET`) and links it.
3. Apply the SOCKS5 patch: in `win32.c`, `enet_socket_send`/`enet_socket_receive`
   consult a per-host relay config. When a relay is set:
   - **send**: prepend the 10-byte SOCKS5 UDP header (`00 00 00` RSV/FRAG, ATYP,
     dest IP, dest port) and `sendto` the relay address instead of the peer.
   - **receive**: `recvfrom` the relay, strip the SOCKS5 UDP header, and report the
     real source address parsed from that header to ENet.
   - Ignore transient recv errors (Windows `WSAECONNRESET`) — return "no packet",
     never propagate (Mori bug: it crashed the bot / dropped the peer).
4. The relay config + handshake (UDP ASSOCIATE over TCP) lives in
   `src/net/socks5_udp.*`; `enet_host.*` wires a created host to a relay.

See `docs/port-specs/03-net-socks5.md` for the exact header format and handshake.
