cmake_minimum_required(VERSION 3.21)
project(Adonai LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/utf-8 /W3 /MP)
endif()

# --- Dependencies (via vcpkg manifest; see vcpkg.json) -----------------------
find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# --- Vendored ENet (patched for SOCKS5-UDP; added under third_party/enet) -----
# Built as a static lib once the source is vendored. Guarded so the UI shell
# still configures before ENet is dropped in.
set(ADONAI_HAVE_ENET OFF)
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/enet/CMakeLists.txt)
  add_subdirectory(third_party/enet)
  set(ADONAI_HAVE_ENET ON)
endif()

# --- Adonai sources -----------------------------------------------------------
file(GLOB_RECURSE ADONAI_ALL_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.cpp
  ${CMAKE_SOURCE_DIR}/src/*.h)

set(ADONAI_GUI_SOURCES ${ADONAI_ALL_SOURCES})
list(FILTER ADONAI_GUI_SOURCES EXCLUDE REGEX "[/\\\\]mcp[/\\\\]mcp_main\\.cpp$")

set(ADONAI_MCP_SOURCES ${ADONAI_ALL_SOURCES})
list(FILTER ADONAI_MCP_SOURCES EXCLUDE REGEX "[/\\\\]ui[/\\\\]")
list(FILTER ADONAI_MCP_SOURCES EXCLUDE REGEX "[/\\\\]main\\.cpp$")

add_executable(Adonai WIN32 ${ADONAI_GUI_SOURCES})
target_include_directories(Adonai PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(AdonaiMcp ${ADONAI_MCP_SOURCES})
target_include_directories(AdonaiMcp PRIVATE ${CMAKE_SOURCE_DIR}/src)

# WIN32 = /SUBSYSTEM:WINDOWS (no console window), but our GLFW entry point is a
# plain int main(), so point the CRT startup at mainCRTStartup instead of WinMain.
if(MSVC)
  target_link_options(Adonai PRIVATE /ENTRY:mainCRTStartup)
endif()
target_compile_definitions(Adonai PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  $<$<BOOL:${ADONAI_HAVE_ENET}>:ADONAI_HAVE_ENET=1>
  $<$<BOOL:${ADONAI_HAVE_ENET}>:ADONAI_ENET_HAS_NEW_PACKET=1>)

target_compile_definitions(AdonaiMcp PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  $<$<BOOL:${ADONAI_HAVE_ENET}>:ADONAI_HAVE_ENET=1>
  $<$<BOOL:${ADONAI_HAVE_ENET}>:ADONAI_ENET_HAS_NEW_PACKET=1>)

target_link_libraries(Adonai PRIVATE
  imgui::imgui
  glfw
  OpenGL::GL
  CURL::libcurl
  nlohmann_json::nlohmann_json
  dwmapi ws2_32 winmm)

target_link_libraries(AdonaiMcp PRIVATE
  CURL::libcurl
  nlohmann_json::nlohmann_json
  ws2_32 winmm)

if(ADONAI_HAVE_ENET)
  target_link_libraries(Adonai PRIVATE enet)
  target_link_libraries(AdonaiMcp PRIVATE enet)
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(NAME adonai_mcp_protocol COMMAND AdonaiMcp --self-test)
  set_tests_properties(adonai_mcp_protocol PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
