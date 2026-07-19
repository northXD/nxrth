# Building Nxrth (Windows / MSVC / vcpkg)

## Prerequisites
- Visual Studio 2022 (Desktop C++ workload) or Build Tools — MSVC v143.
- CMake ≥ 3.21 (bundled with VS).
- [vcpkg](https://github.com/microsoft/vcpkg).

## 1. vcpkg
```powershell
git clone https://github.com/microsoft/vcpkg
.\vcpkg\bootstrap-vcpkg.bat
```
Manifest mode (our `vcpkg.json`) installs `imgui[dx11-binding,win32-binding,docking-experimental]`, `curl`, `nlohmann-json` automatically at configure time.

## 2. Vendor ENet (SOCKS5-UDP patched)
Nxrth needs ENet for the game connection. Drop the C ENet source into `third_party/enet/` and add a `CMakeLists.txt` there that builds a static `enet` target — the root CMake auto-detects it (`NXRTH_HAVE_ENET`). Then apply the SOCKS5 relay patch to `enet_socket_send`/`enet_socket_receive` (see `third_party/enet/README.md` and `docs/port-specs/03-net-socks5.md` for the exact header format). Until ENet is vendored the project still configures, but `net/enet_host.*` is compiled with `NXRTH_HAVE_ENET` off (stub) and bots can't connect.

## 3. Configure + build
```powershell
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```
The exe lands at `build\Release\Nxrth.exe`.

## 4. First-compile reconciliation
This is a large spec-driven port authored without an in-loop compiler, so the first
build will surface integration errors. They are catalogued in `docs/RECONCILE.md`.
Fix them top-down (headers first). Typical classes:
- missing/duplicate symbols across the split `bot_*.cpp` translation units,
- signature drift between a `bot.h` declaration and its `.cpp` definition,
- include-order / forward-declaration gaps,
- the removed HAR/Requestly login modes (Nxrth does not use them).

Paste the compiler error list back and we resolve them in order.

## Data files
Copy `items.dat` (Growtopia's, from the Mori build) next to the exe or into
`data/`. Proxy config persists to `data/nxrth_proxy_pool.json`.
