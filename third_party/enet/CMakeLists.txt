# Nxrth — vendored ENet (lsalzman/enet) + GT new-packet + SOCKS5 patches.
# Built as a static lib; the root CMake auto-detects this file (NXRTH_HAVE_ENET).
cmake_minimum_required(VERSION 3.15)
project(enet C)

# Windows build: win32.c (not unix.c).
add_library(enet STATIC
  callbacks.c
  compress.c
  host.c
  list.c
  packet.c
  peer.c
  protocol.c
  win32.c)

# PUBLIC include root = include/ ONLY, so both ENet's internal <enet/enet.h> and
# Nxrth's <enet/enet.h> resolve. Do NOT expose include/enet/ — its generic
# headers (time.h, list.h, types.h) would shadow the system/std <time.h> etc.
target_include_directories(enet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32)
  target_link_libraries(enet PUBLIC winmm ws2_32)
  target_compile_definitions(enet PRIVATE
    _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
  target_compile_options(enet PRIVATE /w)  # silence ENet's C warnings
endif()
