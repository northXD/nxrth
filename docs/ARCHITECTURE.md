# Adonai — Architecture

A native C++ recode of the Rust "Mori" Growtopia fleet client. This document is
the design contract the port follows. Per-module details live in
`docs/port-specs/*.md` (generated from the Rust source).

## Goals / differences from Mori

1. **Native C++**, no Rust, no Tokio/Axum web server.
2. **Dear ImGui + DirectX 11** desktop UI, **Tahoma** font. No browser dashboard.
3. **In-engine, fleet-aware automation.** Mori ran a per-bot Lua VM per automation;
   bots were isolated and couldn't see each other. Adonai runs automation as
   native C++ modules inside the engine, all reading/writing one shared
   `FleetState`, so bots coordinate (shared targets, claim/among each other,
   fleet-wide throttle, no double-work).
4. Same wire protocol / login flow / proxy model as Mori (GT 5.51 / proto 226).

## Module map (Rust -> C++)

| Rust (Mori) | C++ (Adonai) | Notes |
|---|---|---|
| `constants.rs` | `core/constants.h` | done; values verbatim |
| `logger.rs` | `core/logger.{h,cpp}` | done; ring buffer, UI reads it |
| `protocol/packet.rs` | `protocol/packet.{h,cpp}` | TextPacket, GameUpdatePacket (tank 0x4), packet-type enum |
| `protocol/variant.rs` | `protocol/variant.{h,cpp}` | VariantList (CallFunction args) |
| `protocol/crypto.rs` | `protocol/crypto.{h,cpp}` | klv (MD5), crc32, hash_string |
| `socks5.rs` | `net/socks5_udp.{h,cpp}` | SOCKS5 UDP ASSOCIATE; becomes an ENet socket patch |
| `server_data.rs` | `net/server_data.{h,cpp}` | server_data.php fetch/parse |
| `dashboard.rs`, `auth.rs`, `login.rs`, `har_parser.rs`, `bot/auth.rs` | `login/*` | newly login flow -> ltoken |
| `proxy_pool.rs`, `rotation_pool.rs`, `proxy_test.rs` | `proxy/proxy_pool.{h,cpp}` | pools + DataImpulse sticky + rotation |
| `items.rs` | `world/items.{h,cpp}` | items.dat parser |
| `save_dat.rs`, `world/mod_impl.rs`, `inventory.rs`, `cursor.rs`, `astar.rs` | `world/*` | world model, inventory, A* |
| `bot/core.rs` | `bot/bot.{h,cpp}` | the per-bot engine (biggest module) |
| `bot_manager.rs`, `bot_state.rs`, `bot/shared.rs`, `events.rs`, `player.rs` | `bot/*` | manager + shared views |
| `script_channel.rs` + `lua/*` | `automation/*` | native in-engine automation |
| `web.rs`, `main.rs` | `ui/*`, `main.cpp` | ImGui UI + entry |

## Threading model

- **UI thread**: Win32 + DX11 + ImGui at vsync. Only touches shared state through
  locks/snapshots; never blocks on network.
- **One worker thread per bot** (`Bot::Run`), like Mori. Each owns its ENet host
  and services it every ~10 ms. Bots communicate via `FleetState`, not directly.
- **Global stagger gates** (login-packet / enter-game / dashboard / http-login /
  gateway-logon) are process-wide mutex+timestamp gates, exactly as in Mori
  (`reserve_gate_slot`, `wait_for_global_gate` keeps servicing ENet while waiting).
- **FleetState** is a single `std::shared_mutex`-guarded struct the manager owns;
  bots take read snapshots each tick and publish deltas.

## FleetState + in-engine automation

```
FleetState {
  map<int, BotView> bots;            // id -> {name,status,world,pos,gems,inv,...}
  map<string, Claim> claims;         // e.g. "tile:WORLD:x,y" -> bot_id (avoid double-work)
  map<string, WorldShare> worlds;    // shared knowledge of visited worlds
  AutomationConfig config;           // which modules on, params
}
```

Each automation module implements:

```cpp
struct AutomationModule {
  virtual const char* name() const = 0;
  // Called on a bot's worker tick with that bot's live context + the shared fleet.
  virtual void tick(BotContext& self, FleetState& fleet) = 0;
};
```

Because every module sees `FleetState`, coordination is natural: the geiger/collect
modules `claim()` a target before acting so two bots don't chase the same drop; a
"spread" module assigns bots to different worlds; a "guard" module reacts to another
bot's disconnect. This is the seam where Mori called into Lua (`script_channel` +
`lua/runtime`) — Adonai calls native modules instead.

Initial modules (`src/automation/modules/`): `collect` (auto-pickup),
`geiger` (farming, from `lua/geiger_stats` + core hooks), `coordinate`
(claims/spread), `webhook` (Discord reporting, from `lua/webhook`).

## ENet + SOCKS5-UDP

Mori used `rusty_enet` with a custom `Socket` trait impl that wraps every datagram
in a SOCKS5 UDP header and relays it to the proxy. Adonai vendors the C ENet under
`third_party/enet/` and patches its socket layer (`enet_socket_send/receive`) to do
the same when a relay is configured — one relay endpoint per host. The safe
UDP-bind-fail fallback (bind a dead loopback socket, never connect direct → never
leak the real IP) is preserved. Peer timeout is raised (min 12s) for flaky relays.

## Login flow (unchanged from Mori)

`server_data.php` (growtopia1, fallback growtopia2) -> growid dashboard POST (the
EXACT 22 upstream fields — NO fz/hash2/zf/steamToken, which 500 the 5.51 dashboard)
-> growid login/validate -> `ltoken`. `platformID=15,1,0`. The ltoken is IP-bound:
the HTTP fetch and the ENet gateway logon must leave from the same exit IP (bypass
session pin, or a sticky game proxy when bypass is off). Then ENet `protocol|226`
logon -> `OnSendToServer` redirect -> subserver `protocol|211` -> `Authenticated|1`
-> `OnSpawn` -> world.

## Port order

1. `protocol/*`, `core/*` (leaf, correctness-critical) — from specs 01, 02.
2. `net/*` (socks5, http, server_data, enet wrapper) — specs 03.
3. `proxy/*` — spec 04.
4. `login/*` — spec 05.
5. `world/*` — spec 10, 02.
6. `bot/*` (engine, manager, fleet) — specs 06/07/08/09.
7. `automation/*` — spec 11.
8. `ui/*` — spec 12 (fill the panels).

## Naming rules

- `Mori`/`mori` -> `Adonai`/`adonai` everywhere (identifiers, files, log strings,
  window title `"Adonai"`, config filenames, user-agent).
- `Cloei`/`cloei` -> `North`/`north` (upstream credit, repo references).
