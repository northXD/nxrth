/* Bridge between the vendored ENet socket layer (win32.c) and Adonai's SOCKS5
   relay registry (defined in src/net/enet_host.cpp). When a relay is registered
   for a socket, ENet's send/recv wrap/strip the SOCKS5 UDP header so every ENet
   datagram tunnels through the proxy. When no relay is set the socket behaves as
   stock ENet (direct UDP). */
#ifndef ADONAI_SOCKS5_H
#define ADONAI_SOCKS5_H

#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 and fills out_addr/out_len if `socket_handle` has a relay; else 0. */
int adonai_socks5_relay_for(intptr_t socket_handle, struct sockaddr_storage * out_addr,
                            int * out_len);

/* Builds the SOCKS5 UDP request header targeting `target`. Returns header length
   (10 for IPv4) written to `out`, or <0 on error. */
int adonai_socks5_build_header(const struct sockaddr * target, unsigned char * out, int cap);

/* Parses a SOCKS5 UDP reply header at `data`. Returns the payload length (>=0)
   and fills `src` (real source), `src_len`, `payload_off` (offset of payload in
   data). Returns <0 to drop the datagram. */
int adonai_socks5_parse_header(const unsigned char * data, int len,
                               struct sockaddr_storage * src, int * src_len, int * payload_off);

#ifdef __cplusplus
}
#endif

#endif /* ADONAI_SOCKS5_H */
