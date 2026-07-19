# Port Spec 03 — `net-socks5` (SOCKS5 UDP relay + `server_data.php` fetch)

**Source Rust module(s):** `socks5.rs`, `server_data.rs` (Mori 2.0.0, `src/`)
**Target:** Nxrth (C++). This document is the single source of truth. An engineer must be able to
reimplement this module in C++ **without** reading the Rust.

This module has two independent concerns:

1. **`socks5.rs`** — a SOCKS5 client that performs a **UDP ASSOCIATE** and wraps a `UdpSocket` so that
   every datagram sent gets a SOCKS5 UDP request header prepended, and every datagram received gets it
   stripped. In Mori this wrapper implements the `rusty_enet::Socket` trait so ENet talks UDP through the
   proxy. **In Nxrth this becomes a patch to the vendored C ENet socket layer** (there is no `rusty_enet`).
2. **`server_data.rs`** — an HTTPS `POST` to `server_data.php` (growtopia1 primary / growtopia2 alternate)
   that returns a pipe-delimited key/value blob describing the game server to connect to. Optionally routed
   through an HTTP/SOCKS proxy.

> **CRITICAL — do NOT rename these literals.** They are Growtopia wire constants, not Mori branding:
> the User-Agent `UbiServices_SDK_2022.Release.9_PC64_ansi_static`, the hostnames
> `www.growtopia1.com` / `www.growtopia2.com`, the URL path `/growtopia/server_data.php`, the end marker
> `RTENDMARKERBS1001`, the form keys (`platform`, `protocol`, `version`), and all SOCKS5 protocol bytes.
> Renaming any of these breaks login. See §7 (Rename Rules) for what *does* get renamed.

---

## 1. TYPES

### 1.1 `Socks5Error` (enum) — `socks5.rs`

Error type for the handshake. Variants (no payload except `Io`):

| Variant | Payload | Meaning |
|---|---|---|
| `Io` | `io::Error` | wrapped OS/socket error |
| `InvalidResponse` | — | malformed SOCKS5 reply |
| `AuthenticationFailed` | — | auth rejected / no acceptable method (`0xFF`) |
| `UnsupportedVersion` | — | reply first byte ≠ `0x05` |
| `ConnectionRefused` | — | REP `0x02` or `0x05` |
| `NetworkUnreachable` | — | REP `0x03` |
| `HostUnreachable` | — | REP `0x04` |
| `ConnectionReset` | — | REP `0x06` |
| `CommandNotSupported` | — | REP `0x07` |
| `AddressTypeNotSupported` | — | REP `0x08` or unknown ATYP |
| `GeneralFailure` | — | REP `0x01` |

**Two `From` conversions (preserve exactly):**

- `From<io::Error> for Socks5Error` → wraps as `Socks5Error::Io(err)`.
- `From<Socks5Error> for io::Error` — maps each variant to an `(ErrorKind, message)` pair. Reproduce the
  messages verbatim (used in logs / error strings):

| Variant | `ErrorKind` | Message string |
|---|---|---|
| `Io(e)` | (passthrough `e`) | — |
| `InvalidResponse` | `InvalidData` | `"Invalid SOCKS5 response"` |
| `AuthenticationFailed` | `PermissionDenied` | `"SOCKS5 authentication failed"` |
| `UnsupportedVersion` | `Unsupported` | `"Unsupported SOCKS5 version"` |
| `ConnectionRefused` | `ConnectionRefused` | `"SOCKS5 connection refused"` |
| `NetworkUnreachable` | `NetworkUnreachable` | `"Network unreachable"` |
| `HostUnreachable` | `Other` | `"Host unreachable"` |
| `ConnectionReset` | `ConnectionReset` | `"Connection reset"` |
| `CommandNotSupported` | `Unsupported` | `"Command not supported"` |
| `AddressTypeNotSupported` | `Unsupported` | `"Address type not supported"` |
| `GeneralFailure` | `Other` | `"General SOCKS5 failure"` |

**C++ mapping:** an `enum class Socks5Error { Io, InvalidResponse, ... }` plus a `to_string()`/`what()`
helper reproducing the messages. There is no `io::Error`/`ErrorKind` in C++; represent failures with a
small `struct { Socks5Error code; std::string msg; int os_errno; }` or throw a `std::runtime_error(msg)`.
The `ErrorKind` values only matter where Nxrth's caller distinguishes them; if it doesn't, keep the
message strings and drop the kind mapping.

### 1.2 `Socks5UdpSocket` (struct) — `socks5.rs`

```
struct Socks5UdpSocket {
    udp_socket:      UdpSocket,   // the bound local UDP socket, set non-blocking
    _control_stream: TcpStream,   // SOCKS5 TCP control connection — MUST stay open for the
                                  // lifetime of the UDP association (relay tears it down otherwise).
                                  // Underscore = never read again, but ownership keeps it alive.
    relay_addr:      SocketAddr,  // the UDP relay endpoint returned by UDP ASSOCIATE; all datagrams
                                  // are sent here (NOT to the game server directly).
}
```

**C++ shape:** a struct/class holding `int udp_fd`, `int control_tcp_fd` (SOCKET on Windows), and a
`sockaddr_storage relay_addr` (+ its length). **The control TCP socket must not be closed until the
whole socket wrapper is destroyed.** Closing it drops the UDP association and the bot silently stops
receiving datagrams.

### 1.3 SOCKS5 wire structures (byte-exact)

GT/Growtopia is little-endian, **but SOCKS5 multi-byte fields (ports, and IPv6 addresses) are
big-endian / network byte order** — see the `to_be_bytes` / `from_be_bytes` calls. Ports are always BE.

#### Method-negotiation request (client → proxy)

| Bytes | Field | Value |
|---|---|---|
| with auth | | `05 02 00 02` — VER=5, NMETHODS=2, methods {`00` no-auth, `02` user/pass} |
| no auth | | `05 01 00` — VER=5, NMETHODS=1, method {`00` no-auth} |

Method-negotiation reply (proxy → client), **exactly 2 bytes**: `VER METHOD`.

#### Username/password auth request (RFC 1929, client → proxy)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `0x01` (auth subnegotiation version) |
| 1 | 1 | ULEN = `username.len()` (u8) |
| 2 | ULEN | username bytes (UTF-8/raw) |
| 2+ULEN | 1 | PLEN = `password.len()` (u8) |
| 3+ULEN | PLEN | password bytes |

Auth reply, **2 bytes**: `VER STATUS`. Success ⇔ `VER==0x01 && STATUS==0x00`.

> Note: ULEN/PLEN are single bytes → username/password must be ≤ 255 bytes. No validation is done in Mori;
> a longer string truncates via `as u8`. Preserve behavior (or clamp defensively).

#### UDP ASSOCIATE request (client → proxy), fixed 10 bytes

```
05 03 00 01 00 00 00 00 00 00
VER CMD RSV ATYP  --DST.ADDR--  DST.PORT
 5   3   0   1   0.0.0.0        0
```
CMD=`0x03` (UDP ASSOCIATE), ATYP=`0x01` (IPv4), DST.ADDR=`0.0.0.0`, DST.PORT=`0` (client requests the
relay to accept from any source). This request is **hardcoded** — reproduce byte-for-byte.

#### UDP ASSOCIATE reply (proxy → client), variable length

Read the **first 4 bytes** first: `VER REP RSV ATYP`. Then read the bound address by ATYP:

| ATYP | Extra bytes | Layout |
|---|---|---|
| `0x01` IPv4 | 6 | 4-byte IPv4 (BND.ADDR) + 2-byte BE port |
| `0x04` IPv6 | 18 | 16-byte IPv6 + 2-byte BE port |
| `0x03` domain | — | **error** `InvalidResponse` (Mori refuses a domain in the bind reply) |
| other | — | `AddressTypeNotSupported` |

The returned `(BND.ADDR, BND.PORT)` becomes `relay_addr`.

#### Per-datagram SOCKS5 UDP request header (RFC 1928 §7) — prepended on send, stripped on recv

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 2 | RSV | `00 00` |
| 2 | 1 | FRAG | `00` (fragmentation unsupported) |
| 3 | 1 | ATYP | `01`=IPv4, `04`=IPv6 |
| 4 | 4 or 16 | DST.ADDR | target IP octets |
| 4+len | 2 | DST.PORT | BE port |

**Header length:** IPv4 → **10 bytes**, IPv6 → **22 bytes**. Payload (the ENet datagram) follows.

### 1.4 `LoginInfo` (struct) — `server_data.rs`

```
struct LoginInfo {
    protocol:     u32,
    game_version: String,
}
```
Method `to_form_data(&self) -> String` returns `"protocol={protocol}&version={game_version}"`.
**Caveat:** `to_form_data` is defined but **NOT used** by the fetch path — the actual POST body is built
inline with a `platform=0&` prefix (see §2.9). Port `to_form_data` for completeness but know the live body
differs.

### 1.5 `ServerData` (struct) — `server_data.rs`

Derives `Clone, Debug, Default`. All fields `pub`. Field name ← parsed key (note the two renames):

| Field | Type | Parsed from key | Notes |
|---|---|---|---|
| `server` | `String` | `server` | |
| `port` | `u16` | `port` | `str::parse` — parse error aborts the whole parse |
| `loginurl` | `String` | `loginurl` | |
| `server_type` | `u8` | **`type`** | field renamed (`type` is a Rust keyword) |
| `beta_server` | `String` | `beta_server` | |
| `beta_loginurl` | `String` | `beta_loginurl` | |
| `beta_port` | `u16` | `beta_port` | |
| `beta_type` | `u8` | `beta_type` | |
| `beta2_server` | `String` | `beta2_server` | |
| `beta2_loginurl` | `String` | `beta2_loginurl` | |
| `beta2_port` | `u16` | `beta2_port` | |
| `beta2_type` | `u8` | `beta2_type` | |
| `beta3_server` | `String` | `beta3_server` | |
| `beta3_loginurl` | `String` | `beta3_loginurl` | |
| `beta3_port` | `u16` | `beta3_port` | |
| `beta3_type` | `u8` | `beta3_type` | |
| `type2` | `u8` | `type2` | |
| `maint` | `Option<String>` | **`#maint`** | present only if server is in maintenance |
| `meta` | `String` | `meta` | **required** login field; anti-bot token echoed back at login |

**C++ mapping:** a plain struct; `Option<String> maint` → `std::optional<std::string>` (or a `bool has_maint`
+ `std::string maint`). `Default` → zero-init all numerics, empty strings.

---

## 2. FUNCTIONS

### 2.1 `Socks5UdpSocket::bind_through_proxy` (public)

```
fn bind_through_proxy(
    local_addr: SocketAddr,
    proxy_addr: SocketAddr,
    username: Option<&str>,
    password: Option<&str>,
) -> io::Result<Self>
```

Steps:
1. `TcpStream::connect_timeout(proxy_addr, 10s)` — connect to the SOCKS5 proxy's TCP port.
2. `set_read_timeout(15s)`, `set_write_timeout(15s)` on the control stream.
   **Why (preserve this):** without timeouts a half-open proxy (TCP connects but the SOCKS5 reply never
   arrives) makes `read_exact` block forever. The handshake runs **synchronously inside the bot's service
   loop** on every reconnect/redirect, so an infinite block hangs the worker and it ignores its stop flag.
   A timeout turns it into an `Err`, which the caller's connect-retry handles by picking a fresh proxy.
3. `relay_addr = socks5_handshake(control_stream, username, password)?`.
4. **If `relay_addr.ip().is_unspecified()`** (i.e. `0.0.0.0` or `::`), **overwrite the IP with the proxy's
   IP** (`relay_addr.set_ip(proxy_addr.ip())`), keeping the port. Many proxies return `0.0.0.0:relayport`;
   the real relay lives at the proxy's IP. **Do not skip this — it's the common case.**
5. `udp_socket = UdpSocket::bind(local_addr)` then `set_nonblocking(true)`.
6. Return `Self { udp_socket, _control_stream: control_stream, relay_addr }`.

**C++:** TCP connect with a 10s connect timeout (non-blocking connect + `select`, or `SO_SNDTIMEO`),
then set `SO_RCVTIMEO`/`SO_SNDTIMEO` = 15s on the control socket. Bind a UDP socket to `local_addr`
(typically `0.0.0.0:0`), set it non-blocking (`ioctlsocket FIONBIO=1` / `O_NONBLOCK`). Keep both fds.

### 2.2 `socks5_handshake` (private)

```
fn socks5_handshake(stream, username: Option<&str>, password: Option<&str>) -> Result<SocketAddr, Socks5Error>
```
`use_auth = username.is_some() && password.is_some()` (both must be present).
1. `negotiate_auth_method(stream, use_auth)`.
2. If `use_auth`: `authenticate(stream, username.unwrap(), password.unwrap())`.
3. Return `udp_associate(stream)`.

### 2.3 `negotiate_auth_method` (private)

```
fn negotiate_auth_method(stream, use_auth: bool) -> Result<(), Socks5Error>
```
1. `auth_methods = use_auth ? [05,02,00,02] : [05,01,00]`; `write_all`.
2. `read_exact(2 bytes)` → `response`.
3. `response[0] != 0x05` → `UnsupportedVersion`.
4. `match response[1]`: `0x00`→Ok, `0x02`→Ok, `0xFF`→`AuthenticationFailed`, else→`InvalidResponse`.

> Note: if the proxy selects method `0x02` but no credentials were supplied, this still returns Ok here
> and then `udp_associate` runs without an auth exchange, which the proxy will likely reject. Behavior is
> as-written; don't "fix" it silently.

### 2.4 `authenticate` (private)

```
fn authenticate(stream, username: &str, password: &str) -> Result<(), Socks5Error>
```
Build the RFC-1929 request (§1.3), `write_all`, `read_exact(2)`. Fail with `AuthenticationFailed`
unless `response == [0x01, 0x00]`.

### 2.5 `udp_associate` (private)

```
fn udp_associate(stream) -> Result<SocketAddr, Socks5Error>
```
1. `write_all([05,03,00,01,00,00,00,00,00,00])`.
2. `read_exact(response[..4])` (VER REP RSV ATYP).
3. `response[0] != 0x05` → `UnsupportedVersion`.
4. `match response[1]` (REP):
   `0x00`→continue; `0x01`→`GeneralFailure`; `0x02`→`ConnectionRefused`; `0x03`→`NetworkUnreachable`;
   `0x04`→`HostUnreachable`; `0x05`→`ConnectionRefused`; `0x06`→`ConnectionReset`;
   `0x07`→`CommandNotSupported`; `0x08`→`AddressTypeNotSupported`; else→`InvalidResponse`.
5. `match response[3]` (ATYP): `0x01`→read 6 more bytes, `Ipv4Addr(response[4..8])`, port `BE(response[8..10])`;
   `0x04`→read 18 bytes, `Ipv6Addr(bytes[0..16])`, port `BE(bytes[16..18])`; `0x03`→`InvalidResponse`;
   else→`AddressTypeNotSupported`.
6. Return the parsed `SocketAddr`.

### 2.6 `create_udp_header` (private, `&self`)

```
fn create_udp_header(&self, target_addr: SocketAddr) -> Vec<u8>
```
Builds the per-datagram header (§1.3): `[00 00]`, `push 00` (FRAG), then by address family:
IPv4 → `push 01` + 4 octets + `port.to_be_bytes()`; IPv6 → `push 04` + 16 octets + `port.to_be_bytes()`.
Returns the header only (payload appended by the caller). **Does not depend on `relay_addr`** — the
`target_addr` here is the *real game server* address ENet wants to reach.

### 2.7 `parse_udp_header` (private, `&self`)

```
fn parse_udp_header<'a>(&self, data: &'a [u8]) -> io::Result<(SocketAddr, &'a [u8])>
```
Validates and splits an incoming relayed datagram; returns `(real_source_addr, payload_slice)`:
1. `data.len() < 10` → `InvalidData "UDP header too short"`.
2. `data[0]!=0 || data[1]!=0` → `InvalidData "Invalid RSV field"`.
3. `data[2] != 0` → `Unsupported "Fragmentation not supported"`.
4. `data[3]` (ATYP):
   - `0x01`: `ip=Ipv4(data[4..8])`, `port=BE(data[8..10])`, payload=`data[10..]`.
   - `0x04`: if `data.len() < 22` → `InvalidData "IPv6 header too short"`; `ip=Ipv6(data[4..20])`,
     `port=BE(data[20..22])`, payload=`data[22..]`.
   - `0x03`: `Unsupported "Domain name addresses not supported"`.
   - else: `InvalidData "Unsupported address type"`.

### 2.8 `rusty_enet::Socket` trait impl → **C++ ENet socket-layer patch**

This is the heart of the port. In Rust the wrapper *is* the ENet socket via the trait:

```
type Address = SocketAddr;
type Error   = io::Error;

fn init(&mut self, _opts: SocketOptions) -> Result<(), io::Error> { Ok(()) }   // no-op

fn send(&mut self, address: SocketAddr, buffer: &[u8]) -> Result<usize, io::Error> {
    let mut packet = self.create_udp_header(address);   // header for the REAL target
    packet.extend_from_slice(buffer);                   // + ENet payload
    match self.udp_socket.send_to(&packet, self.relay_addr) {   // send to the RELAY
        Ok(sent) => if sent >= packet.len() { Ok(buffer.len()) } else { Ok(0) },
        Err(e) if e.kind()==WouldBlock => Ok(0),
        Err(e) => Err(e),
    }
}

fn receive(&mut self, buffer: &mut [u8; MTU_MAX])
        -> Result<Option<(SocketAddr, PacketReceived)>, io::Error> {
    match self.udp_socket.recv_from(buffer) {
        Ok((size, _source)) => match self.parse_udp_header(&buffer[..size]) {
            Ok((real_addr, payload)) => {
                let payload_len = payload.len();
                if payload_len <= MTU_MAX {
                    let off = payload.as_ptr() as usize - buffer.as_ptr() as usize;  // = header len
                    if off > 0 { buffer.copy_within(off..off+payload_len, 0); }      // move payload to front
                    Ok(Some((real_addr, PacketReceived::Complete(payload_len))))
                } else { Ok(None) }        // (defensive; can't happen with 4096 buffer)
            }
            Err(_) => Ok(None),            // malformed relayed datagram → drop silently
        },
        Err(e) if e.kind()==WouldBlock => Ok(None),
        Err(_) => Ok(None),                // <-- see WSAECONNRESET note below
    }
}

fn address(&self) -> SocketAddr {
    self.udp_socket.local_addr().unwrap_or_else(|_| "0.0.0.0:0".parse().unwrap())
}
```

**Key semantic contracts to preserve in the C++ ENet patch:**

- **`address` passed to `send` is the real remote (game server); the datagram is physically sent to
  `relay_addr`.** The header encapsulates the real target. Symmetrically, the address returned by
  `receive` is the *decapsulated* real source, not the relay.
- **`send` return convention:** ENet expects "bytes of *payload* accepted". Return `buffer.len()`
  (the ENet payload length, **not** the on-wire length incl. header) on full send; return `0` on partial
  send or `WouldBlock` (ENet treats 0 as "try later, not an error"). Only a hard error propagates.
- **`receive` returns `Complete(payload_len)` with the payload moved to offset 0** of ENet's buffer.
  ENet then reads `buffer[0..payload_len]`. Returning `None`/`Ok(None)` means "no packet this tick".
- **`MTU_MAX`** = `rusty_enet::MTU_MAX` = ENet's `ENET_PROTOCOL_MAXIMUM_MTU` = **4096**. The receive
  buffer is exactly 4096 bytes; a relayed datagram = header (10/22) + payload, so payload ≤ 4086 and the
  `payload_len <= MTU_MAX` guard is always true (keep it as defensive code).
- **WSAECONNRESET (Windows 10054) MUST be swallowed on receive.** On Windows a UDP socket surfaces a
  prior send's ICMP "port unreachable" as `WSAECONNRESET` on the *next* `recvfrom`. For a SOCKS5 relay
  this fires on transient relay-port hiccups and is **not fatal**. Treat *any* non-WouldBlock receive
  error as "no packet this tick" (`Ok(None)`), never propagate it. A truly dead relay is still caught by
  ENet's reliable-ack timeout, producing a clean `Disconnect` → deliberate reconnect instead of thrashing.
  Propagating 10054 would tear down the ENet peer and drop the in-world bot (reason code 0) for a blip.
- **`init` is a no-op.** `SocketOptions` (ENet's requested send/recv buffer sizes etc.) are ignored.
  In C++ you may honor them via `setsockopt(SO_RCVBUF/SO_SNDBUF)` on the UDP fd, or ignore them to match.

**How to realize this against vendored C ENet (recommended approach):**

ENet's host uses two socket primitives (in `unix.c`/`win32.c`):
`enet_socket_send(socket, &address, buffers, bufferCount)` and
`enet_socket_receive(socket, &address, buffers, bufferCount)`. Patch options, pick one:

1. **Per-host SOCKS5 context + patched send/receive (recommended).** Store a
   `struct Socks5Relay { int udp_fd; int control_fd; sockaddr_storage relay_addr; }` associated with the
   `ENetHost` (e.g. a side-table keyed by `host->socket`, or add a field to `ENetHost`). In the patched
   `enet_socket_send`: gather `buffers` into a contiguous scratch buffer *after* an emitted SOCKS5 header
   built from `address`; `sendto(udp_fd, scratch, relay_addr)`; return the summed **payload** byte count
   (mirror the Rust "return payload length" rule). In the patched `enet_socket_receive`:
   `recvfrom(udp_fd, into ENet's buffer, &from)`; parse+validate header; set `*address` to the decapsulated
   real source; `memmove` payload to the front of `buffers[0].data`; return `payload_len`. On WouldBlock
   return 0; on any other error return 0 (swallow, see 10054 note). Do the handshake (§2.1) once at host
   creation and stash `relay_addr` + keep `control_fd` open until `enet_host_destroy`.

2. **Custom `ENetSocket` shim.** If your ENet fork abstracts the socket behind function pointers, provide a
   SOCKS5 socket object whose `send`/`receive` do the same. Same semantics.

**ENet address plumbing:** ENet uses `ENetAddress { host, port }` (host is a `uint32` IPv4 in network
order in classic ENet, or an `in6_addr` in the ipv6 fork). Map SOCKS5 IPv4 → ENet host; the per-datagram
header's ATYP must match the family. GT uses IPv4, so IPv4 (ATYP `0x01`, 10-byte header) is the hot path;
implement IPv6 (ATYP `0x04`, 22-byte header) for parity but it is exercised rarely.

**Non-blocking + buffer sizing:** set the UDP fd non-blocking (matches `set_nonblocking(true)`). Receive
directly into ENet's provided 4096-byte buffer and `memmove` in place (mirrors `copy_within`) to avoid an
extra copy; or use a 4096-byte scratch. Never allocate per-datagram in the hot path (the Rust `Vec` alloc
in `create_udp_header`/`send` is a wart — in C++ use a reusable thread-local send buffer).

### 2.9 `server_data.rs` functions

**Public entry points (all three delegate to the same fetch):**

```
fn get_server_data(alternate: bool, login_info: &LoginInfo) -> Result<ServerData>
    // = get_server_data_proxied(alternate, login_info, None)
fn get_server_data_proxied(alternate, login_info, proxy_url: Option<&str>) -> Result<ServerData>
    // = fetch_server_data_from_endpoint(...)
fn get_server_data_proxied_live(alternate, login_info, proxy_url: Option<&str>) -> Result<ServerData>
    // = fetch_server_data_from_endpoint(...)   // identical alias to `_proxied`
```
`Result<T>` here = `std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>` (any boxed error).
In C++: return `std::optional<ServerData>` / throw, or an `expected<ServerData, std::string>`.

**`fetch_server_data_from_endpoint(alternate, login_info, proxy_url)`** — the real work:

1. **URL selection:** `alternate ? "https://www.growtopia2.com/growtopia/server_data.php"
   : "https://www.growtopia1.com/growtopia/server_data.php"`.
   > **Fallback note:** this function does NOT loop growtopia1→growtopia2 itself. The `alternate` bool
   > only *selects* one endpoint. The growtopia1-primary/growtopia2-fallback policy is the **caller's**
   > responsibility (call with `alternate=false`, and on `Err` retry with `alternate=true`). Port the
   > caller's retry policy in whatever Nxrth module drives login; this module just needs both URLs.
2. `println!("[server_data] proxy_url={:?}", proxy_url.map(redact_proxy_url))` — log the **redacted**
   proxy (credentials masked). Keep this log; keep the redaction.
3. **Build HTTP agent:**
   - Common config: **disable TLS certificate verification** (`disable_verification(true)`) and a
     **global timeout of 20 seconds**.
   - If `proxy_url` is `Some(p)`: attach an HTTP/SOCKS proxy parsed from `p`.
4. **POST** to the URL with headers (exact):
   - `User-Agent: UbiServices_SDK_2022.Release.9_PC64_ansi_static`
   - `Content-Type: application/x-www-form-urlencoded`
   - **Body:** `platform=0&protocol={protocol}&version={game_version}` (built inline — note the
     `platform=0&` prefix that `LoginInfo::to_form_data` lacks).
5. **On success (`Ok(resp)`):** read body to string; `logger::log("[server_data] Response from {url}: \n{body}")`;
   `data = ServerData::parse_from_response(&body)?`; if `data.has_required_login_fields()` → `Ok(data)`,
   else `Err("server_data response from {url} is missing login fields")`.
6. **On transport error (`Err(e)`):** `logger::log("[server_data] Error fetching {url}: {e:?}")`; return `Err(e)`.

**`ServerData::parse_from_response(response: &str) -> Result<Self>`** — the wire parser:

- Start from `ServerData::default()`.
- Iterate **lines** of the response.
  - If a line **starts with `RTENDMARKERBS1001`** → **`break`** (stop parsing; end-of-data marker).
  - `split_once('|')` into `(key, value)`; if there is no `|`, **skip** the line (`continue`).
  - `value = value.trim()`; then `match key.trim()` against the keys in §1.5 and assign. Numeric fields use
    `str::parse` — **a parse failure returns `Err` and aborts the whole fetch** (via `?`). Preserve this:
    a malformed `port`/`type` is a hard error, not a skipped field.
  - `#maint` → `maint = Some(value)`. Unknown keys are ignored.
- Return the populated `ServerData`.

Line/format specifics: keys and values are separated by a single `|`; both key and value are trimmed;
lines use `str::lines()` semantics (split on `\n`, a trailing `\r` is *not* stripped by `lines()` on its
own — but because the value is `.trim()`-ed, a trailing `\r` in the value is removed; a trailing `\r` in
the *key* is removed by `key.trim()`). Replicate with: split on `\n`, trim key and value of ASCII
whitespace (incl. `\r`).

**`has_required_login_fields(&self) -> bool`** (private): returns true iff
`!server.trim().is_empty() && port != 0 && !loginurl.trim().is_empty() && !meta.trim().is_empty()`.
These four (`server`, `port`, `loginurl`, `meta`) are the minimum for a usable login.

**`redact_proxy_url(value: &str) -> String`** (private): parse `value` as a URL; if it has a non-empty
username set it to `"***"`; if it has a password set it to `"***"`; return the re-serialized URL. If the
value doesn't parse as a URL, return it unchanged. C++: reuse the URL parser (§3) or a small manual
`user:pass@host` masker.

---

## 3. DEPENDENCY MAPPING (Rust crate → Nxrth C++)

| Rust (this module) | Used for | Nxrth C++ replacement |
|---|---|---|
| `rusty_enet::{Socket, SocketOptions, PacketReceived, MTU_MAX}` | ENet socket-layer trait | **Vendored C ENet, patched** at `enet_socket_send`/`enet_socket_receive` (or a socket shim). No trait — patch the C functions. `MTU_MAX` → `ENET_PROTOCOL_MAXIMUM_MTU` (4096). `PacketReceived::Complete(n)` → return `n`; `None` → return 0. |
| `std::net::{TcpStream, UdpSocket, SocketAddr, Ipv4Addr, Ipv6Addr}` | SOCKS5 control TCP + relay UDP | Native BSD/Winsock sockets. **Not libcurl** — this is raw UDP relay, curl can't do UDP-ASSOCIATE. TCP `connect` w/ 10s timeout; `SO_RCVTIMEO/SO_SNDTIMEO`=15s; UDP `bind`+non-blocking. `SocketAddr` → `sockaddr_storage`. |
| `std::time::Duration` | timeouts | `std::chrono` / raw `timeval`. Constants: connect 10s, control r/w 15s, HTTP 20s. |
| `ureq` + `ureq::tls::TlsConfig` | HTTPS POST to server_data.php | **libcurl.** Set `CURLOPT_USERAGENT`, `CURLOPT_HTTPHEADER` (Content-Type), `CURLOPT_POSTFIELDS`, `CURLOPT_TIMEOUT` 20s. **Disable TLS verify:** `CURLOPT_SSL_VERIFYPEER=0`, `CURLOPT_SSL_VERIFYHOST=0` (mirrors `disable_verification(true)`). |
| `ureq::Proxy` | route fetch through proxy | `CURLOPT_PROXY`. Use **`socks5h://`** (remote DNS) for SOCKS proxies per the memory note about IP-binding; `http://` for HTTP proxies. Enable cookies (`CURLOPT_COOKIEFILE ""`) if the login flow needs them. |
| `url::Url` (in `redact_proxy_url`) | mask proxy creds in logs | Small manual parser or a vendored URL lib. Only needs `user`/`pass` masking. |
| `crate::logger::log` | file/console logging | Nxrth logger (renamed; see §7). Keep the `[server_data]` tag. |
| `std::error::Error` boxed | error propagation | `std::string` error / `expected` / exceptions. |
| `md5`, `argon2`, `serde_json`, `mlua`, `scraper`, `crossbeam-channel`, `tokio/axum/tower` | — | **Not used in this module.** (Fleet-wide: json→nlohmann, hashing→bundled md5/argon2 lib, channels→std::mutex+condvar queue, async runtime/web→std::thread + Dear ImGui. None apply here.) |

---

## 4. THREADING & SHARED STATE

- **`Socks5UdpSocket` is per-bot / per-ENet-host.** Each bot owns its own control TCP socket, UDP socket,
  and `relay_addr`. It is created (handshake included) synchronously **inside that bot's service loop** on
  connect / reconnect / server-redirect. The 10s/15s timeouts (§2.1) exist specifically because this runs
  on the worker thread: a blocking handshake would freeze the bot and make it ignore its stop flag. In
  Nxrth, the ENet host lives on the bot's worker thread (`std::thread`); the patched socket send/receive
  run on that same thread inside the ENet service tick. No locking is needed on the socket itself — it is
  single-owner. **Do not share one `Socks5UdpSocket`/UDP fd across bots.**
- **Control TCP lifetime = association lifetime.** The `_control_stream` must stay open for as long as the
  UDP relay is used. In C++ store the control fd in the per-host SOCKS5 context and close it only in the
  host teardown path (alongside the UDP fd), never right after the handshake.
- **`server_data` fetch is a blocking HTTPS call (up to 20s).** It should run off the hot path — either on
  the bot's setup thread before ENet starts, or on a dedicated fetch thread. It shares no mutable state
  with the socket layer.
- **Fleet-wide shared-state notes (Nxrth "bots aware of each other"):**
  - **Proxy assignment must be stable per bot.** Per the memory note (GT ltoken is bound to the minting
    IP), a bot must use a **consistent** proxy across the server_data fetch *and* the SOCKS5 UDP relay *and*
    login — a rotating-per-stage proxy breaks GT login. The fleet's shared proxy-pool manager must pin one
    proxy (one egress IP) per bot for the whole session. This module consumes `proxy_url`/`proxy_addr` +
    credentials; the *source* of those must be the shared pinned assignment.
  - **`ServerData` is identical for all bots on the same GT client version.** Consider fetching it once and
    caching it in shared fleet state (guarded by a mutex), refreshing on `#maint`/failure, instead of every
    bot hammering `server_data.php`. If cached, still allow a per-bot proxied refetch when a bot's proxy
    must originate the request. Any shared cache → protect with `std::mutex` (the fleet's std::mutex+condvar
    queue pattern) since worker threads read it concurrently.
  - The `meta` field is an anti-bot token echoed back at login and can rotate; treat a cached `ServerData`
    as short-lived.

---

## 5. CONSTANTS / MAGIC VALUES (do not change)

| Constant | Value | Where |
|---|---|---|
| TCP connect timeout | **10 s** | `bind_through_proxy` |
| control read/write timeout | **15 s** | `bind_through_proxy` |
| HTTP global timeout | **20 s** | `fetch_server_data_from_endpoint` |
| SOCKS5 version | `0x05` | all handshake steps |
| method: no-auth / user-pass | `0x00` / `0x02` | negotiate |
| no-acceptable-methods | `0xFF` | negotiate reply |
| auth subneg version | `0x01` | RFC 1929 |
| CMD UDP ASSOCIATE | `0x03` | associate request |
| ATYP IPv4 / domain / IPv6 | `0x01` / `0x03` / `0x04` | associate + per-datagram |
| associate request (verbatim) | `05 03 00 01 00 00 00 00 00 00` | `udp_associate` |
| per-datagram header IPv4 / IPv6 length | **10** / **22** bytes | send/recv |
| `MTU_MAX` | **4096** (`ENET_PROTOCOL_MAXIMUM_MTU`) | receive buffer |
| end-of-data marker | `RTENDMARKERBS1001` | `parse_from_response` |
| User-Agent | `UbiServices_SDK_2022.Release.9_PC64_ansi_static` | POST header |
| Content-Type | `application/x-www-form-urlencoded` | POST header |
| POST body template | `platform=0&protocol={protocol}&version={version}` | POST body |
| primary URL | `https://www.growtopia1.com/growtopia/server_data.php` | `alternate=false` |
| alternate URL | `https://www.growtopia2.com/growtopia/server_data.php` | `alternate=true` |
| redaction token | `***` | `redact_proxy_url` |
| log tags | `[server_data]` | fetch/parse logs |

Ports and IPv6 address bytes are **big-endian (network order)**. (GT's own protocol payloads are
little-endian, but that's *inside* the ENet payload, opaque to this module.)

---

## 6. EDGE CASES / GOTCHAS TO REPRODUCE

- Half-open proxy → timeout → `Err` (not infinite block). Caller retries with a fresh proxy.
- Relay returns `0.0.0.0`/`::` → substitute the proxy IP (§2.1 step 4).
- `send` returns the **payload** length, not on-wire length; partial send / WouldBlock → `0`.
- `receive` swallows *all* non-WouldBlock errors (esp. Windows `WSAECONNRESET` 10054) → `None`.
- Malformed relayed datagram (bad RSV/FRAG/ATYP/short) → dropped silently (`None`), not an error.
- Auth method `0x02` selected without creds → not caught here; associate then fails at the proxy.
- ULEN/PLEN are single bytes → user/pass > 255 chars silently truncate via `as u8`.
- `server_data` numeric parse failure (`port`, any `type`) → hard `Err`, aborts fetch (via `?`).
- Line with no `|` → skipped. First line starting `RTENDMARKERBS1001` → stop parsing.
- Missing any of {`server`, `port`, `loginurl`, `meta`} → `has_required_login_fields()` false → `Err`.
- TLS verification is **disabled** for the fetch (proxies often MITM TLS); replicate with curl verify off.
- `LoginInfo::to_form_data` is dead code vs the live body — don't wire it into the fetch.

---

## 7. RENAME RULES (Mori → Nxrth, Cloei → North)

Global rules for the port:

- Every `Mori`/`mori` identifier, file path, log line, window title, User-Agent (Nxrth's *own* UA, if
  any), config filename → `Nxrth`/`nxrth`.
- Every `Cloei`/`cloei` (upstream author/repo) → `North`/`north`.

**Concrete occurrences in *these two files*:**

- **None literal.** Neither `socks5.rs` nor `server_data.rs` contains the strings `Mori`/`mori` or
  `Cloei`/`cloei`. The only branding-adjacent references are the Rust module path `crate::logger` → map to
  Nxrth's logger namespace (e.g. `nxrth::logger` / `north::logger` per project layout), and the source
  file names themselves (`socks5.rs`, `server_data.rs`) → Nxrth C++ units (e.g. `socks5.{h,cpp}`,
  `server_data.{h,cpp}` under the Nxrth `net` module).

**MUST-NOT-RENAME (these are Growtopia/Ubisoft wire constants, not Mori/Cloei branding):**

- `UbiServices_SDK_2022.Release.9_PC64_ansi_static` (User-Agent — Growtopia's real client UA; renaming it
  makes `server_data.php` reject the request).
- `www.growtopia1.com`, `www.growtopia2.com`, `/growtopia/server_data.php`.
- `RTENDMARKERBS1001`, and all form keys (`platform`, `protocol`, `version`) and response keys
  (`server`, `port`, `loginurl`, `type`, `beta*`, `type2`, `#maint`, `meta`).
- All SOCKS5 protocol bytes/versions.

Nxrth's log tags may keep `[server_data]` (descriptive, not branded) — but if the project prefixes logs
with the app name elsewhere, use `nxrth`/`north` there, never `mori`/`cloei`.

---

## 8. SUGGESTED C++ FILE LAYOUT (non-normative)

- `net/socks5.h` / `net/socks5.cpp` — `Socks5Relay` context, handshake (`negotiate_auth_method`,
  `authenticate`, `udp_associate`), `bind_through_proxy`, header build/parse helpers.
- `net/enet_socks5_patch.cpp` — the patched `enet_socket_send`/`enet_socket_receive` (or socket shim)
  that call the header build/parse helpers and honor the send/receive return conventions + 10054 swallow.
- `net/server_data.h` / `net/server_data.cpp` — `LoginInfo`, `ServerData`, `parse_from_response`,
  `has_required_login_fields`, `fetch_server_data_from_endpoint` (libcurl), `redact_proxy_url`.
