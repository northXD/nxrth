# Crash Recovery

Nxrth uses a separate `NxrthSupervisor.exe` process because an application
cannot reliably restart itself after it has crashed. Recovery is configured
through the desktop-only MCP tools, including from the built-in AI tab.

## Modes

- `snapshot_only`: restore the protected fleet checkpoint and shared
  `AutomationConfig`.
- `script_only`: reopen Nxrth and execute one saved script from `scripts/`.
- `snapshot_then_script`: restore the fleet first, then execute the script.

Bot connections are asynchronous. In `snapshot_then_script`, the Lua script is
started after bot objects are recreated, not after every bot is online. A
recovery script that depends on a connection must call
`bot.waitOnline(selector, timeoutMs)` with a bounded timeout before sending
world or inventory commands. Scripts
should also avoid re-adding accounts that the snapshot already restores.

`recovery_configure` accepts only safe backup and script names, not arbitrary
paths. It creates or updates:

- `data/fleets/<name>.fleet`: exact secret-bearing fleet state encrypted with
  Windows CurrentUser DPAPI.
- `data/recovery/recovery_plan.json`: non-secret mode, path, delay, and
  crash-loop settings.
- `data/recovery/supervisor.log`: PID/exit/restart events only; no credentials.

While enabled, the desktop app refreshes the protected checkpoint after the bot
launch catalog or shared automation config changes. The recovery plan itself
never contains login records or script source.

## Restart Behavior

The supervisor watches the desktop PID. Exit code `0` is a normal close and is
not restarted. An abnormal exit is restarted with:

```text
Nxrth.exe --recover-plan <trusted-plan-path>
```

The application accepts only its canonical local recovery plan path, loads the
protected checkpoint without returning its contents, optionally executes the
saved script, and establishes a new supervisor for the replacement process.

Restart delay uses bounded exponential backoff. `max_restarts` within
`window_seconds` opens the crash-loop breaker and requires a manual restart.
The defaults are a 2 second initial delay, 3 restarts, and a 10 minute window.

## Secret and Failure Rules

- `.fleet` values are not redacted before encryption. The original token and
  password bytes remain usable for login after restore.
- DPAPI protected backups are intended for the same Windows user context.
- The supervisor never reads or decrypts a fleet backup.
- Startup logs report only restored counts and generic script status. A Lua
  exception is not copied into the AI-visible shared log.
- Pool-backed bots request a fresh endpoint from the current pool. Custom proxy
  records restore their exact saved endpoint and credentials. Direct records
  remain direct.
- Merge restore may be partial when a required proxy pool is unavailable. The
  tool returns skipped-record metadata without returning credentials. A
  replacement restore preflights every record first and leaves the current
  fleet untouched if any required proxy dependency is unavailable.

Recovery does not survive a powered-off machine, a missing desktop session, a
deleted executable, or a Windows account change that prevents DPAPI decryption.
