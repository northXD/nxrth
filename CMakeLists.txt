cmake_minimum_required(VERSION 3.21)
project(Nxrth LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/utf-8 /W3 /MP)
endif()

# --- Dependencies (via vcpkg manifest; see vcpkg.json) -----------------------
find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(Lua 5.4 REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# --- Vendored ENet (patched for SOCKS5-UDP; added under third_party/enet) -----
# Built as a static lib once the source is vendored. Guarded so the UI shell
# still configures before ENet is dropped in.
set(NXRTH_HAVE_ENET OFF)
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/enet/CMakeLists.txt)
  add_subdirectory(third_party/enet)
  set(NXRTH_HAVE_ENET ON)
endif()

# --- Nxrth sources -----------------------------------------------------------
file(GLOB_RECURSE NXRTH_ALL_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.cpp
  ${CMAKE_SOURCE_DIR}/src/*.h)

set(NXRTH_GUI_SOURCES ${NXRTH_ALL_SOURCES})
list(FILTER NXRTH_GUI_SOURCES EXCLUDE REGEX "[/\\\\]mcp[/\\\\]mcp_main\\.cpp$")
list(FILTER NXRTH_GUI_SOURCES EXCLUDE REGEX "[/\\\\]supervisor[/\\\\]")

set(NXRTH_MCP_SOURCES ${NXRTH_ALL_SOURCES})
list(FILTER NXRTH_MCP_SOURCES EXCLUDE REGEX "[/\\\\]ui[/\\\\]")
list(FILTER NXRTH_MCP_SOURCES EXCLUDE REGEX "[/\\\\]main\\.cpp$")
list(FILTER NXRTH_MCP_SOURCES EXCLUDE REGEX "[/\\\\]supervisor[/\\\\]")

add_executable(Nxrth WIN32 ${NXRTH_GUI_SOURCES})
target_include_directories(Nxrth PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(Nxrth PRIVATE ${LUA_INCLUDE_DIR})

add_executable(NxrthMcp ${NXRTH_MCP_SOURCES})
target_include_directories(NxrthMcp PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(NxrthMcp PRIVATE ${LUA_INCLUDE_DIR})

add_executable(NxrthSupervisor WIN32
  ${CMAKE_SOURCE_DIR}/src/supervisor/supervisor_main.cpp)
target_include_directories(NxrthSupervisor PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_dependencies(Nxrth NxrthSupervisor)

# WIN32 = /SUBSYSTEM:WINDOWS (no console window), but our GLFW entry point is a
# plain int main(), so point the CRT startup at mainCRTStartup instead of WinMain.
if(MSVC)
  target_link_options(Nxrth PRIVATE /ENTRY:mainCRTStartup)
  target_link_options(NxrthSupervisor PRIVATE /ENTRY:mainCRTStartup)
endif()
target_compile_definitions(Nxrth PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  $<$<BOOL:${NXRTH_HAVE_ENET}>:NXRTH_HAVE_ENET=1>
  $<$<BOOL:${NXRTH_HAVE_ENET}>:NXRTH_ENET_HAS_NEW_PACKET=1>)

target_compile_definitions(NxrthMcp PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  $<$<BOOL:${NXRTH_HAVE_ENET}>:NXRTH_HAVE_ENET=1>
  $<$<BOOL:${NXRTH_HAVE_ENET}>:NXRTH_ENET_HAS_NEW_PACKET=1>)

target_compile_definitions(NxrthSupervisor PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

target_link_libraries(Nxrth PRIVATE
  imgui::imgui
  glfw
  CURL::libcurl
  ${LUA_LIBRARIES}
  nlohmann_json::nlohmann_json
  d3d11 dxgi dwmapi ws2_32 winmm crypt32)

target_link_libraries(NxrthMcp PRIVATE
  CURL::libcurl
  ${LUA_LIBRARIES}
  nlohmann_json::nlohmann_json
  ws2_32 winmm crypt32)

target_link_libraries(NxrthSupervisor PRIVATE
  nlohmann_json::nlohmann_json)

if(NXRTH_HAVE_ENET)
  target_link_libraries(Nxrth PRIVATE enet)
  target_link_libraries(NxrthMcp PRIVATE enet)
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(NAME nxrth_mcp_protocol COMMAND NxrthMcp --self-test)
  set_tests_properties(nxrth_mcp_protocol PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
