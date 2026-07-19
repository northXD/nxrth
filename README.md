# Nxrth

A native **C++ Growtopia multibot client** — run and coordinate many bots from one
Dear ImGui desktop app, with in-engine automation, proxy support, a sandboxed Lua
executor, and a built-in AI operator.

**Discord: [discord.gg/nxrth](https://discord.gg/nxrth)**

---

## Features

- **Multi-account fleet** — spawn and manage many bots at once, each on its own worker.
- **Login methods** (see below) — GrowID/Mail, Fruit OAuth ltokens, and provider gateway tokens.
- **Proxies** — SOCKS5 game pool (per-bot exit, capacity-limited) + a rotating "logon bypass" pool (one clean IP per login).
- **AutoGeiger / geiger farming** — fleet-aware geiger hunting, digging, depositing, and pickup.
- **Lua 5.4 executor** — sandboxed scripting over the bot + automation API, run fleet-wide or per bot.
- **Built-in AI operator** — drive the app in natural language (OpenAI, Anthropic, Gemini, and more).
- **Items database** — built-in `items.dat` viewer.

## Login methods

Nxrth accepts several credential formats (paste a full record or fill the fields
in **Add Bot**):

- **GrowID / Mail + password** — the classic dashboard login.
- **Google OAuth `ltoken`** — `refreshToken|rid|mac|wk` (or a keyed `refreshToken:` record),
  validated through Growtopia's `checktoken` into a fresh session token.
- **Provider gateway tokens** — keyed `key:value` records containing `token:` / `rid:` / `mac:` / `wk:`
  (and optional `platform:` / `name:` / `cbits:` / `playerAge:` / `vid:`), each pinned to one
  rotating exit IP for both the token fetch and the ENet logon.

Bots can be added one at a time, pasted/loaded in bulk, or added from the Lua
executor / MCP.

## AutoGeiger / geiger support

The engine ships a fleet-aware **AutoGeiger** module: configure hunt worlds,
storage/depot worlds, and pickup worlds, the target item, recharge timing, and
signal/settle waits, then arm it on selected bots. It walks the geiger signal,
digs, deposits loot to the depot worlds, and picks prizes up — coordinated across
the whole fleet through a shared `FleetState`. Configure it per bot from the bot
detail **Automation** tab, or script it from the Lua executor (`autogeiger.*`).

## Build (Windows, MSVC + vcpkg)

```powershell
git clone https://github.com/microsoft/vcpkg
.\vcpkg\bootstrap-vcpkg.bat

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<path>\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

Manifest mode installs the dependencies (Dear ImGui, libcurl, Lua, nlohmann-json).
Run the app from the project root so it finds `data/`. Provide your own
`data/items.dat` (a Growtopia item database) for item names and the Database tab.

## Extras

- **`NxrthMcp.exe`** — a stdio MCP server exposing the same fleet to external agents (see `docs/MCP.md`).
- **Lua API** — `docs/LUA.md`. **AI operator** — `docs/AI.md`. **Crash recovery** — `docs/RECOVERY.md`.

---

Questions / community: **[discord.gg/nxrth](https://discord.gg/nxrth)**
