# Adonai

Native C++ Growtopia fleet client — a ground-up recode of the Rust "Mori"
framework. Dear ImGui + DirectX 11 UI (Tahoma), vendored ENet (patched for
SOCKS5-UDP), and **in-engine, fleet-aware automation** (no per-bot Lua VMs — every
bot shares one `FleetState`, so bots coordinate and see each other).

Upstream lineage is credited to **North** (formerly the "Cloei" fork).

## Build (Windows, MSVC + vcpkg)

```powershell
# one-time: get vcpkg and integrate
git clone https://github.com/microsoft/vcpkg
.\vcpkg\bootstrap-vcpkg.bat

# configure + build (manifest mode installs imgui/curl/nlohmann-json)
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<path>\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

The UI shell (window + Tahoma + panels) builds today. Engine modules are being
ported in from the specs under `docs/port-specs/`; see `docs/ARCHITECTURE.md`.

## Layout

```
src/
  core/       constants, logger
  protocol/   packets, VariantList, klv/crc32/md5
  net/        SOCKS5-UDP, ENet host wrapper, HTTP (libcurl), server_data
  proxy/      game + bypass pools, DataImpulse sticky, rotation
  login/      server_data -> dashboard -> growid -> ltoken flow
  world/      tile/object model, items.dat, A* pathfinding
  bot/        the per-bot engine, BotManager, FleetState (shared)
  automation/ native in-engine automation modules (geiger, collect, coordinate)
  ui/         ImGui panels + theme (Tahoma)
  mcp/        headless stdio MCP server for autonomous agent control
third_party/  vendored ENet (SOCKS5-patched), etc.
docs/         ARCHITECTURE.md + port-specs/ (per-module C++ port specs)
```

## MCP server

The build also produces `AdonaiMcp.exe`, a headless stdio MCP server exposing
bot sessions, navigation, world sensing, chat, inventory, collection, mining,
building, and other player actions. See [`docs/MCP.md`](docs/MCP.md) for client
configuration and the asynchronous command model.

## Naming rules (enforced across the port)

- `Mori` / `mori` -> `Adonai` / `adonai` (identifiers, paths, logs, window title, config names)
- `Cloei` / `cloei` (upstream author/repo) -> `North` / `north`
