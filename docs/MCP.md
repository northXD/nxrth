# Adonai MCP Server

`AdonaiMcp` is the stdio control surface for Adonai. When the desktop app is
open, it automatically forwards MCP requests through a local Windows named pipe
to the app's live `BotManager`, `ProxyPool`, `FleetState`, world/inventory state,
and logs. Bots added by AI therefore appear in the desktop UI and both sides
control the same fleet. If the app is not open, the same executable can still
run its original independent headless fleet.

The protocol stream is one JSON-RPC message per line on stdin/stdout. Game logs
stay in memory and are exposed through tools/resources, so normal bot logging
does not corrupt MCP output.

## Build and Client Setup

```powershell
cmake --preset vs-release
cmake --build --preset vs-release --target Adonai AdonaiMcp
```

Example MCP client configuration:

```json
{
  "mcpServers": {
    "adonai": {
      "command": "C:\\Adonai\\build\\vs-release\\AdonaiMcp.exe",
      "args": [],
      "env": {
        "ADONAI_MCP_MODE": "app"
      }
    }
  }
}
```

Run it with `C:\Adonai` as the working directory. The server expects:

- `items.dat` or `data/items.dat` for item names/collision metadata.
- `data/proxy_pool.json` for game proxies and rotating login proxies.
- Optional `data/automation_profiles.json` for saved automation profiles.

Credentials are accepted only as tool arguments. Tool results and resources must
not be used to reveal passwords or raw login tokens. `session_logs` redacts
sensitive token/password fields before returning logs.

## Desktop Shared Fleet Mode

For normal interactive use, start components in this order:

1. Start `Adonai.exe` and leave it open.
2. Start or reconnect the MCP client so it launches `AdonaiMcp.exe`.
3. Call `session_list` or `fleet_logs` to verify the live app state.
4. Call `session_login`; the new bot appears in the app's bot list.

The desktop app owns the only live `BotManager` in this mode. Pipe I/O runs on a
worker thread, but every MCP request is executed from the app's UI thread. This
preserves the manager's single-thread ownership while bot network workers keep
running normally.

Routing is selected on the first MCP request and remains fixed for that MCP
process:

- Default/`auto`: use the desktop shared fleet if the app is already listening;
  otherwise create an independent headless fleet.
- `ADONAI_MCP_MODE=app`: require the desktop app and return an error instead of
  silently creating a second fleet.
- `ADONAI_MCP_MODE=headless`: always use an independent headless fleet.

For AI clients intended to control the visible app, setting
`ADONAI_MCP_MODE=app` is recommended. If the MCP client was launched before the
app, restart/reconnect that MCP server after opening the app because its route is
intentionally stable for the lifetime of the process.

The bridge is local-only at `\\.\pipe\Adonai.AppMcp.v1`; remote pipe clients are
rejected. Credentials still travel only between the local MCP process and the
local desktop process.

## Capability Summary

The MCP server is sufficient for an AI to:

- Start GrowID, newly, or ltoken sessions.
- Add and remove bots in the open desktop app's visible fleet.
- Use the configured proxy pool and rotating login pool.
- Observe connection status, ping, world, position, inventory, chat, system logs,
  nearby tiles, players, and floating ground items.
- Warp/leave/reconnect/disconnect/stop sessions.
- Move/pathfind, punch, place, wrench, enter doors, face, respawn, accept access,
  collect objects, drop/trash/use/equip items, and send chat.
- Enable, configure, validate, scope, pause, and profile native automations.
- Start/stop AutoGeiger with the same hunt/depot/pickup params the desktop UI
  uses, plus advanced params.

`tools/list` is the authoritative JSON Schema catalog. This document explains
how to combine the tools safely.

## Control Model

Most mutating tools are asynchronous. `accepted: true` means the command was
queued on the bot's worker thread or the shared automation config was updated.
It does not mean Growtopia has completed the action.

Recommended loop:

1. Call a mutating tool.
2. Poll `session_status`, `sense_environment`, `world_floating_items`,
   `inventory_list`, or a resource.
3. Use `session_logs` only when something is stuck or failed.

Do not spam movement or world actions without reading state between steps. The
bot engine already has reconnect/backoff/gating logic; let it breathe.

## Login and Proxy Behavior

`session_login` fields:

- `method: "growid"` uses the legacy/dashboard flow.
- `method: "newly"` uses the newly flow. This is often the best choice for the
  current account files.
- `method: "ltoken"` expects the ltoken string as `username`.
- `use_configured_proxy: true` should be left enabled for normal use.

When proxy config is enabled:

- `ProxyPool::choose()` assigns one game/world SOCKS5 proxy per bot.
- `ProxyPool::rotating_login_proxy()` supplies the bypass login pool if enabled.
- Rotating login performs HTTP login through a fresh bypass exit and pins the
  ENet gateway logon to the same SOCKS5 exit, because the token is IP-bound.
- World/subserver traffic continues through the assigned game proxy.
- Direct login is refused unless `ADONAI_ALLOW_DIRECT_LOGIN=1` is explicitly set.

Operational notes:

- `newly` can need repeated attempts. `Fail to login. Please try again in 30
  seconds` is not always terminal.
- A practical policy is: retry the same account, but if that exact fail-to-login
  response appears twice, stop that bot and try the next account.
- `Server requesting that you re-logon` can happen transiently. Treat it as
  retryable unless it loops.
- `wrong credentials`, `exhausted`, `update_required`, and persistent maintenance
  states are terminal for that attempt.

## Core Observation Tools

Use these after any action:

- `session_list`: compact fleet overview.
- `session_status`: one bot's status, world, position, ping, inventory counts,
  auto flags, and other details.
- `session_logs`: redacted per-bot system logs. Useful for login/reconnect debug.
- `fleet_logs`: the same redacted shared application/manager log stream shown by
  the desktop app, optionally filtered by `bot_id`.
- `chat_read`: in-game chat/console buffer.
- `sense_environment`: nearby tiles, blocked tiles, players, objects.
- `world_floating_items`: dropped/floating ground items visible to a bot, with
  item id, name, uid, count, tile position, and distance.
- `inventory_list`: inventory stacks with names and active/equipped state.

Useful resources:

- `adonai://fleet`
- `adonai://automation`
- `adonai://automation/status`
- `adonai://logs`
- `adonai://bot/{bot_id}/state`
- `adonai://bot/{bot_id}/world`
- `adonai://bot/{bot_id}/objects`
- `adonai://bot/{bot_id}/inventory`
- `adonai://bot/{bot_id}/chat`

## Automation Model

Every bot has the native automation modules attached at spawn. Modules self-gate
against the shared `FleetState::AutomationConfig` every tick. Updating automation
config is live; existing bots see it without restart.

Supported modules:

- `geiger`
- `collect`
- `coordinate`
- `webhook`

Important tools:

- `automation_get_config`: read enabled flags, params, groups, scopes, and
  supported params.
- `automation_status`: read validation, module scopes, active bot ids, per-bot
  AutoGeiger phase, geiger signal, counter counts, and floating item counts.
- `automation_validate_config`: validate current params without changing them.
- `automation_define_group`: define a named group of bot ids.
- `automation_set_module`: enable/disable any module, merge params, and set scope
  by `bot_ids` or `group`.
- `automation_pause_all`: disable all automation modules without clearing params.

Scope rules:

- No scope means the enabled module applies to the whole fleet.
- `bot_ids` means only those bot ids run that module.
- `group` means only bot ids in that named group run that module.
- Setting one scope type clears the other for that module.

## AutoGeiger

Main tool:

```json
{
  "name": "automation_start_geiger_farm",
  "arguments": {
    "hunt_worlds": "HUNT1,HUNT2|door",
    "depot_worlds": "DEPOT1",
    "pickup_worlds": "PICKUP1"
  }
}
```

Equivalent detailed tool:

- `automation_configure_geiger`

Required params:

- `hunt_worlds`: worlds to search/hunt in.
- `depot_worlds`: worlds used when inventory must be deposited.
- `pickup_worlds`: worlds where a counter can be picked up when missing.

Optional params:

- `geiger_item`, default `2204`.
- `wear`, default `true`.
- `dig`, default `true`.
- `recharge_min`, default `30`.
- `min_y`, default `0`.
- `max_y`, default `53`.
- `world_width`, default `100`.
- `signal_wait_ms`, default `4200`.
- `max_steps`, default `70`.
- `webhook_url`.

Stop/pause:

- `automation_stop_geiger_farm` disables only geiger.
- `automation_pause_all` disables every automation module.

AutoGeiger status phases are derived for AI readability, for example:

- `disabled`
- `waiting_for_world`
- `pickup`
- `recharging`
- `deposit`
- `hunting`
- `blocked:no_hunt_worlds`
- `blocked:no_depot_worlds`
- `blocked:no_counter_pickup_worlds`

Always call `automation_status` after starting AutoGeiger. If validation is not
valid, fix config before adding more bots.

## Profiles

Automation profiles persist full automation config under
`data/automation_profiles.json`.

Tools:

- `automation_save_profile`
- `automation_load_profile`
- `automation_list_profiles`

Profile names may contain only letters, digits, `_`, and `-`.

Example:

```json
{
  "name": "automation_save_profile",
  "arguments": { "name": "geiger_default" }
}
```

Loading a profile replaces the live automation config.

## Floating Items

`world_floating_items` is the direct AI-facing way to inspect dropped items:

```json
{
  "name": "world_floating_items",
  "arguments": {
    "bot_id": 0,
    "radius_tiles": 8,
    "name_contains": "Geiger"
  }
}
```

It returns sorted items by distance. Use the `uid` with `inventory_collect` to
collect a specific object, or omit `uid` in `inventory_collect` to collect nearby
eligible objects.

The same data is available as `adonai://bot/{bot_id}/objects`.

## Recommended AI Workflows

### Connect one bot

1. Ensure `data/proxy_pool.json` has game proxies and rotating login enabled.
2. Call `session_login` with `method: "newly"` and `use_configured_proxy: true`.
3. Poll `session_status` every few seconds until `in_game` or a terminal state.
4. If stuck, call `session_logs` and classify:
   - Two `Fail to login ... 30 seconds` messages: stop and try next account.
   - Re-logon once: keep retrying.
   - Wrong credentials/exhausted/update required: stop and move on.

### Start geiger fleet

1. Call `automation_start_geiger_farm`.
2. Call `automation_validate_config`.
3. Spawn/load bots with `session_login`.
4. Poll `automation_status`.
5. Use `world_floating_items` and `inventory_list` only when diagnosing.

### Scope geiger to selected bots

```json
{
  "name": "automation_define_group",
  "arguments": { "group": "geiger_team", "bot_ids": [0, 1, 2] }
}
```

Then:

```json
{
  "name": "automation_set_module",
  "arguments": { "module": "geiger", "enabled": true, "group": "geiger_team" }
}
```

## Safety Notes

- Do not print passwords or raw tokens in client logs.
- Prefer `newly` for the account-stat files unless a specific account requires
  another method.
- Keep rotating login enabled for normal bot login attempts.
- Treat `accepted: true` as queued/configured, not completed.
- Poll state after every meaningful action.
- Stop test bots with `session_stop` when finished.
