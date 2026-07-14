# Adonai — first-compile reconciliation

The port is spec-driven and was authored without an in-loop C++ compiler, so the
FIRST `cmake --build` will surface integration errors. This is expected for a
17k-line cross-language port. Work top-down (headers first) and paste the error
list back to resolve them in order. Known items below are already accounted for
or flagged.

## Already reconciled in-session
- **Fleet gates unified.** `login/login.cpp` had its own `FleetGate` machinery; it
  now forwards `pace_dashboard`/`pace_http_login` to the authoritative
  `bot/gates.h` so the whole fleet shares ONE cursor per phase. (The old local
  `FleetGate` funcs in login.cpp are now unused → `C4505` warnings only.)
- **App wired.** `main.cpp` constructs `BotManager` + `ProxyPool` + a `UiEventSink`
  and passes them to `AppUi(manager, proxy_pool)`.
- **Automation attached.** `BotManager` spawn thread now attaches every native
  module (`automation::build_all()`) to each bot before `run()`; modules self-gate
  per tick on the shared `FleetState` `AutomationConfig`.
- **Module name keys aligned.** UI toggles now use the module `kName`
  ("collect"/"geiger"/"coordinate"/"webhook"), matching `automation/modules/*`.
- **Automation set complete:** collect, coordinate, geiger, webhook — all
  fleet-aware (claim/spread through `FleetState`).

## Known items to resolve at first compile
1. **HAR / Requestly — REMOVED (done 2026-07-11).** `LoginMethodKind` is now
   `Legacy / Newly / Ltoken`; `create_requestly` / `create_har_token` /
   `spawn_requestly` / `spawn_har_token` and the `fetch_requestly_credentials` /
   `extract_har_auth_data` forward-decls are deleted; UI modes are
   Standard / Newly / Ltoken. No functional HAR refs remain (verified).
2. **Cross-TU signature drift.** The `Bot` class is split across
   `bot_connection.cpp` / `bot_handlers.cpp` / `bot_world.cpp` against one `bot.h`.
   If any `.cpp` defines a method with a signature the header doesn't declare (or
   vice-versa), MSVC flags it — align to `bot.h`.
3. **`emit_inventory_update`** is defined in `bot_world.cpp`; ensure
   `bot_handlers.cpp` only *calls* it (no duplicate definition).
4. **ENet.** `net/enet_host.*` is guarded by `ADONAI_HAVE_ENET`. Until
   `third_party/enet/` is vendored + patched (see its README), the game connection
   is a stub. Vendor ENet, then rebuild.
5. **items.dat** must be present at runtime (copy Growtopia's `items.dat` next to
   the exe / into `data/`), or `ItemsDat::load` fails and bots can't resolve items.

## Sanity checklist after it links
- Window opens titled "Adonai", Tahoma font, dockable panels.
- Add a bot (newly mode) with a proxy → it logs in (watch the Console panel).
- Toggle an automation module → the shared `FleetState` flips and bots act.
