# Nxrth MCP Server

`NxrthMcp` is the stdio control surface for Nxrth. When the desktop app is
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
cmake --build --preset vs-release --target Nxrth NxrthMcp NxrthSupervisor
```

Example MCP client configuration:

```json
{
  "mcpServers": {
    "nxrth": {
      "command": "C:\\Nxrth\\build\\vs-release\\NxrthMcp.exe",
      "args": [],
      "env": {
        "NXRTH_MCP_MODE": "app"
      }
    }
  }
}
```

Run it with `C:\Nxrth` as the working directory. The server expects:

- `items.dat` or `data/items.dat` for item names/collision metadata.
- `data/proxy_pool.json` for game proxies and rotating login proxies.
- Optional `data/automation_profiles.json` for saved automation profiles.

Credentials enter through login/script arguments or trusted local fleet files.
Tool results and resources never return passwords or raw login tokens.
`session_logs` redacts sensitive token/password fields before returning logs.

## Desktop Shared Fleet Mode

For normal interactive use, start components in this order:

1. Start `Nxrth.exe` and leave it open.
2. Start or reconnect the MCP client so it launches `NxrthMcp.exe`.
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
- `NXRTH_MCP_MODE=app`: require the desktop app and return an error instead of
  silently creating a second fleet.
- `NXRTH_MCP_MODE=headless`: always use an independent headless fleet.

For AI clients intended to control the visible app, setting
`NXRTH_MCP_MODE=app` is recommended. If the MCP client was launched before the
app, restart/reconnect that MCP server after opening the app because its route is
intentionally stable for the lifetime of the process.

The bridge is local-only at `\\.\pipe\Nxrth.AppMcp.v1`; remote pipe clients are
rejected. Credentials still travel only between the local MCP process and the
local desktop process.

## Capability Summary

The MCP server is sufficient for an AI to:

- Start GrowID or Google OAuth ltoken sessions.
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
- Run sandboxed Lua 5.4 scripts against the same shared fleet through
  `lua_execute`, including provider-format `addBot(table)` imports.
- Save/load the exact live fleet without returning its credentials, manage
  named Lua scripts, and execute saved scripts locally.
- Configure and verify the desktop crash supervisor so an abnormal exit can
  restore a protected fleet and/or rerun a saved script.

`tools/list` is the authoritative JSON Schema catalog. This document explains
how to combine the tools safely.

## Persistence and Recovery

Fleet tools use safe names under `data/fleets` and never accept arbitrary MCP
paths:

- `fleet_save`: `protected` (default) writes a DPAPI CurrentUser encrypted
  `.fleet`; `legacy_txt` is an explicit plaintext compatibility export.
- `fleet_load`: starts the stored bots. Its safe defaults merge into the current
  fleet (`replace_existing: false`) and leave the current automation profile
  unchanged (`restore_automation: false`). The returned `id_remap` is
  authoritative after restore; replacement and automation restoration require
  explicit opt-in.
- `fleet_backups_list`: returns name, format, size, and modification time only.

Protected persistence does **not** redact before encryption. Exact ltoken or
provider records, passwords, custom proxy authentication, proxy policy,
rotating-login intent, and automation state remain usable for a later login.
Redaction applies only to AI/MCP/log output. Tool results contain metadata and
always report `secrets_returned: false`; no tool can read a fleet file's
contents back through MCP.

Named Lua files live under `scripts/`:

- `script_write` stores exact source but does not echo it.
- `script_read` returns a conservatively pattern-redacted view plus a
  fingerprint; it is not a general secret vault for arbitrary script text.
- `script_execute` reads exact source locally and returns redacted output/error.
- `script_list` returns metadata only.

Desktop-only recovery tools are `recovery_configure`, `recovery_status`, and
`recovery_disable`. Modes are `snapshot_only`, `script_only`, and
`snapshot_then_script`. Recovery is implemented by the separate
`NxrthSupervisor.exe`; require `enabled: true` and `supervisor_running: true`
before reporting that monitoring is active. Normal app exit is not restarted,
and a bounded crash-loop breaker limits repeated abnormal restarts. See
`docs/RECOVERY.md` for the on-disk and restart contract.

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

## Lua Script Execution

`lua_execute` runs one complete Lua chunk against the current MCP fleet. In
desktop mode this is the open app's live bot list, proxy pool, world snapshots,
and fleet automation config. In headless mode it is that MCP process's owned
fleet.

```json
{
  "name": "lua_execute",
  "arguments": {
    "source": "autogeiger.addworld('HUNT1')\nautogeiger.addstorageworld('DEPOT1')\nautogeiger.addgeigerstorageworld('PICKUP1')\nautogeiger.enable()\nprint('configured')"
  }
}
```

The structured result contains `ok`, redacted `output`, redacted `error`, and
`added_bot_ids`. A Lua load/runtime/limit failure sets the MCP result's
`isError` flag while preserving those fields for diagnosis. The source itself
is never returned or logged.

Every execution gets a fresh sandboxed Lua state. Limits are fixed server-side
at 20,000,000 VM instructions, 30 seconds, and 512 KiB output. Since desktop
execution runs synchronously on the manager/UI owner thread, avoid long
`sleep()` or `waitOnline()` loops; bot network workers continue independently.

Read `nxrth://lua/api` for the machine-readable API catalog. The full human
reference and provider import example are in `docs/LUA.md`. Native MCP tools are
still preferable for a single simple action; Lua is intended for multi-step or
batch logic.

## Login and Proxy Behavior

`session_login` fields:

- `method: "growid"` uses the legacy/dashboard flow.
- `method: "ltoken"` accepts either `refreshToken|rid|mac|wk` or a provider
  record such as `mac:<mac>|wk:<wk>|platform:<id>|rid:<rid>|name:<name>|`
  `cbits:<bits>|playerAge:<age>|token:<providerToken>|vid:<uuid>`.
- **All** `ltoken` records — positional `refreshToken|rid|mac|wk`, keyed
  `refreshToken:`, and keyed provider `token:` (Apple/Google) — are refresh tokens
  validated through Growtopia `checktoken`. The raw record token is NOT a gateway
  ltoken; sending it directly is rejected with *"Fail to login. Please try again in
  30 seconds."*
- `rid` must be 32 characters. Positional `wk` must be 32 characters; keyed
  records also accept the provider sentinel `NONE0` (its literal value is not
  validated).
- Provider fields can be in any order. Unknown `key:value` fields are tolerated.
  `name` is the bot display name; `rid`, `mac`, and `vid` are this device and are
  the load-bearing fields in the `checktoken` `clientData`.
- `username` and `password` are used only by `growid`; never put the ltoken in
  `username`.
- `use_configured_proxy: true` should be left enabled for normal use.

The verified login flow is:

1. Fetch `server_data.php` → `meta`, gateway host/port.
2. `POST /player/growid/checktoken?valKey=…` with `refreshToken=<record token>` and
   a **PLAINTEXT** (form-encoded, NOT base64) `clientData` carrying this device
   (`rid`/`mac`/`vid`) + the `meta`. It returns the fresh **session ltoken**
   (`status:"success"`, `accountType` e.g. `apple`). `klv`/`hash` are accepted but
   not validated; a real `meta` is required (a fake `meta` bounces to an HTML page —
   the historical "2xx non-JSON").
3. First ENet gateway packet — the bare session ltoken:

   ```text
   protocol|226
   ltoken|<session ltoken from checktoken>
   platformID|2
   ```

   The gateway replies `OnSendToServer` (redirect + session `user`/`token`/`UUIDToken`).
4. Reconnect to the subserver and send the full-identity redirect packet
   (`protocol|225`, `platformID|1`, `vid`/`aid`, redirect `user`/`token`/`UUIDToken`/
   `doorID`/`aat`), matching the working reference client.

`server_data`, `checktoken`, and the gateway ENet login all egress from ONE pinned
exit IP (the session ltoken is bound to it).

MCP example (place the real composite secret in `ltoken`):

```json
{
  "name": "session_login",
  "arguments": {
    "method": "ltoken",
    "ltoken": "<refreshToken>|<32-char-rid>|<mac>|<32-char-wk>",
    "use_configured_proxy": true
  }
}
```

Provider-format example (all values are placeholders):

```json
{
  "name": "session_login",
  "arguments": {
    "method": "ltoken",
    "ltoken": "mac:02:00:00:00:00:00|wk:NONE0|platform:1|rid:<32-char-rid>|name:bot_name|cbits:1536|playerAge:25|token:<provider-token>|vid:<uuid>",
    "use_configured_proxy": true
  }
}
```

The desktop **Add bot** Ltoken field accepts the same full line. The Database
tab and `accounts_spawn` also accept multiple provider records, one per line.
Do not split a provider record into separate fields.

When proxy config is enabled:

- `ProxyPool::choose()` assigns one game/world SOCKS5 proxy per bot.
- `ProxyPool::rotating_login_proxy()` supplies the bypass login pool if enabled.
- GrowID login may use the rotating bypass pool.
- Google OAuth ltoken deliberately does not rotate: `server_data`, `checktoken`
  and ENet use the same assigned game proxy because the checked token is IP-bound.
- Provider `token:` records pin one rotating SOCKS5 session for `server_data`
  and gateway ENet by default. The game pool remains a fallback when no rotating
  session is available.
- Provider validate-ltokens are **not IP-bound** (they log in from any exit), so
  when the rotating-login pool is burned (HTTP 403 / login-blocked), set
  `NXRTH_PROVIDER_NO_ROTATING=1` to route provider `server_data` + gateway ENet
  through the assigned (fresh) game proxy instead of the rotating-login pool.
- World/subserver traffic continues through the assigned game proxy.
- Direct login is refused unless `NXRTH_ALLOW_DIRECT_LOGIN=1` is explicitly set.

Operational notes:

- The old `newly`, `newly_13_1_0`, and `newly_13` methods no longer exist.
- Invalid ltoken shape is rejected before a bot is accepted. Keyed records must
  contain non-empty `token`, `rid`, `mac`, and `wk` fields.
- A failed initial `checktoken` validation stops that bot without exposing the
  supplied refresh token or returned token in logs.
- On reconnect, both Google and provider records re-run `checktoken` with the
  original refresh token (`LoginMethod::source_token`) + the fresh `meta` to mint a
  new session ltoken bound to the new session/exit, then perform
  `server_data -> gateway ENet` on that exit.
- GT rate-limits logins per exit IP (`too_many_logins`, "reached the limit of
  accounts that can be login from this IP", "try again in 30 seconds"). Pace logins
  and spread them across proxy exits.
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

- `nxrth://fleet`
- `nxrth://automation`
- `nxrth://automation/status`
- `nxrth://logs`
- `nxrth://bot/{bot_id}/state`
- `nxrth://bot/{bot_id}/world`
- `nxrth://bot/{bot_id}/objects`
- `nxrth://bot/{bot_id}/inventory`
- `nxrth://bot/{bot_id}/chat`

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

The same data is available as `nxrth://bot/{bot_id}/objects`.

## Recommended AI Workflows

### Connect one bot

1. Ensure `data/proxy_pool.json` has a working game SOCKS5 proxy.
2. Call `session_login` with `method: "ltoken"`, the composite `ltoken` field,
   and `use_configured_proxy: true`.
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
- Use `accounts_spawn` with `mode: "ltoken"` for account-stat files whose
  records contain a composite `login_token`/`ltoken` value.
- Rotating login is independent of ltoken login and may remain enabled for
  legacy GrowID sessions.
- Treat `accepted: true` as queued/configured, not completed.
- Poll state after every meaningful action.
- Stop test bots with `session_stop` when finished.
