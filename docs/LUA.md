# Nxrth Lua 5.4 API

Nxrth embeds Lua 5.4 in the desktop Executor. Open `Executor > Lua`, edit the
script, and press Run. The same runtime is available to AI clients through the
MCP `lua_execute` tool. The editor starts with a provider-token `addBot(table)`
template compatible with the seller record format used by Nxrth.

## MCP Execution

Call `lua_execute` with one required `source` string. When `NxrthMcp.exe` is in
desktop/app mode, the script uses the open application's same bots, proxy pool,
world state, and shared automation config. In headless mode it uses that MCP
server's owned fleet.

```json
{
  "name": "lua_execute",
  "arguments": {
    "source": "for _, b in ipairs(getBots()) do print(b.id, b.name) end"
  }
}
```

The response preserves redacted output and errors and includes every bot id
accepted by `addBot`. Read the MCP resource `nxrth://lua/api` for a compact API
catalog. Script source is consumed locally and is not returned or logged.

## Execution Model

- A fresh Lua state is created for every desktop or MCP execution.
- Execution is synchronous on the UI/BotManager owner thread.
- Default limits are 20,000,000 VM instructions, 30 seconds, and 512 KiB output.
- Output, load errors, and runtime errors redact token/password/proxy secrets and
  long Base64-like values. Each fresh Lua state has a 64 MiB allocator ceiling
  in addition to the 20 million instruction, 30 second, and 512 KiB output
  limits.
- Available standard libraries: base, coroutine, table, string, math, and utf8.
- `os`, `io`, `package`, `debug`, `dofile`, and `loadfile` are unavailable.

## Saved Scripts

The Executor can Save/Load scripts by safe name and Import/Export `.lua` files.
Managed scripts live under `scripts/`; names are limited to letters, digits,
`_`, and `-`, with an optional `.lua` extension. Files are capped at 1 MiB and
are replaced atomically.

MCP exposes `script_list`, `script_read`, `script_write`, and `script_execute`.
`script_read` returns a redacted source view. Execution reads the exact local
file internally, so provider records in a restart script still work, but exact
source and secret-like output do not leave the application.

## Add Bot

```lua
local ok, botIdOrError = addBot({
    token = "provider refresh token",
    rid = "32-character RID",
    mac = "02:00:00:00:00:00",
    wk = "NONE0",
    platform = 1,
    name = "display name",
    cbits = 1536,
    playerAge = 25,
    vid = "provider VID",
    proxy = "auto",
    connect = true,
    type = TOKEN
})
```

Accepted aliases are `refreshToken`, `platformID`, `player_age`, `username`,
and `growid`. A legacy account can use `username`/`growid` plus `password`.

- Success returns `true, bot_id`. Spawn acceptance is asynchronous and does not
  mean the account is already online.
- Failure returns `false, error`; one invalid record does not abort an import loop.
- `connect=false` returns an error because Nxrth has no disconnected account catalog.
- `proxy="auto"` requires an enabled game proxy pool and never silently uses the
  real IP. An explicit proxy line may be supplied instead.
- Provider login uses the selected game proxy by default. Set
  `rotatingLogin=true` only when the rotating-login pool should also be used.
- Direct login requires both `proxy="direct"` and the process environment variable
  `NXRTH_ALLOW_DIRECT_LOGIN=1`.
- Provider `platform`, `cbits`, and `playerAge` values are preserved in the
  checktoken/redirect identity; absent fields use the proven defaults.

Compatibility globals required by SurferBot-style imports are present:
`TOKEN`, `calculateBackpackCost`, `addBot`, `getBot`, `getBots`, `removeBot`,
`print`, and `sleep`.

## AutoGeiger

All AutoGeiger values mutate the one fleet `AutomationConfig`, are immediately
visible in Executor and every bot's Automation tab, and are saved to
`data/automation_config.json`.

```lua
autogeiger.setworlds({"HUNT1", "HUNT2:door"})
autogeiger.setstorageworlds({"LOOTDEPOT"})
autogeiger.setgeigerstorageworlds({"COUNTERDEPOT"})
autogeiger.setoption("dig", true)
autogeiger.enable()
```

Hunt worlds:
`addworld`, `setworlds`, `removeworld`, `clearworlds`, `getworlds`.

Loot depot worlds:
`addstorageworld`, `setstorageworlds`, `removestorageworld`,
`clearstorageworlds`, `getstorageworlds`.

Counter pickup worlds:
`addgeigerstorageworld`, `setgeigerstorageworlds`,
`removegeigerstorageworld`, `cleargeigerstorageworlds`,
`getgeigerstorageworlds`.

Control and inspection: `enable([bool])`, `disable()`, `status()`/`getconfig()`,
`getSignal(bot)`, and `setoption(key, value)`. `enable()` is fleet-wide. For an explicit bot scope use
`automation.setbots("geiger", {botId1, botId2})`; use
`automation.allbots("geiger")` to remove that scope.

## Bot API

Every function accepts a numeric bot id or a case-insensitive bot name as its
first argument. Both dot and colon table-call styles are accepted.

Fleet and state:

- `bot.list()`, `bot.status(selector)`, `bot.find(name)`, `bot.remove(selector)`
- `bot.getLocal`, `bot.getPing`, `bot.getSignal`
- `bot.isInWorld`, `bot.isInTile`, `bot.isWearing`
- `bot.waitOnline(selector, timeoutMs)`

World and inventory snapshots:

- `bot.getWorld`, `bot.getPlayers`, `bot.getInventory`
- `bot.getFloatingItems`, `bot.getObjects`
- `bot.getTiles`, `bot.getTile`
- `bot.getConsole`, `bot.chatlog`

Floating-item filtering:

```lua
local result = bot.getFloatingItems(botId, {
    item_id = 2204,
    radius = 20,
    name_contains = "geiger"
})
```

Each floating item includes `uid`, `item_id`, optional `name`, `count`, `world`,
pixel/tile positions, and `distance_tiles`. Inventory and tile snapshots resolve
item names through the shared `items.dat` database.

Actions:

- Session: `connect`, `reconnect`, `disconnect`, `leaveWorld`, `respawn`
- Chat/world: `warp`, `say`, `chat`, `enter`, `acceptAccess`
- Movement: `move`, `moveTile`, `moveTo`, `moveLeft`, `moveRight`, `moveUp`,
  `moveDown`, `walk`, `findPath`, `setDirection`
- Tile/player: `place`, `punch`, `hit`, `wrench`, `wrenchPlayer`,
  `activateTile`, `active`
- Inventory: `wear`, `use`, `unwear`, `drop`, `trash`, `activateItem`,
  `collect`, `collectObject`
- Settings: `setAutoCollect`, `auto_collect`, `autoreconnect`

Action functions return whether the command was accepted by the bot's command queue.

## Generic Automation

- `automation.enable(module, bool)`
- `automation.setparam(key, value)` / `automation.getparam(key)`
- `automation.setbots(module, {selectors...})`
- `automation.allbots(module)`

Known native modules are `geiger`, `collect`, and `coordinate`.

## Deliberate Limits

This is the complete safe Nxrth API over existing `BotCommand`, bot snapshots,
fleet configuration, and native automations.
arbitrary HTTP/file access, event hooks, and script-defined rotation engines are
not exposed. Native AutoGeiger and the MCP automation tools remain the supported
paths for those workflows.
