cmake_minimum_required(VERSION 3.24)
project(Bulkhead VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Bulkhead is a Windows / DirectX 12 project.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Captures (PNG frames, overhead maps) land here so they are easy to find and film.
set(BULKHEAD_CAPTURE_DIR "${CMAKE_SOURCE_DIR}/captures" CACHE PATH "Where ctest capture runs write PNGs")

# ---------------------------------------------------------------------------
# Third-party: stb (PNG read/write). Header-only, fetched at configure time.
# ---------------------------------------------------------------------------
include(FetchContent)
# Pinned to a commit (an archive download, so no git history is fetched).
FetchContent_Declare(stb
  URL https://github.com/nothings/stb/archive/9600522de53973336fa033a289858a4354cbb04c.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(stb)

add_library(bh_stb STATIC third_party/stb_impl.cpp)
target_include_directories(bh_stb SYSTEM PUBLIC ${stb_SOURCE_DIR})
set_target_properties(bh_stb PROPERTIES FOLDER third_party)

# ---------------------------------------------------------------------------
# Shader compiler: dxc.exe from the Windows SDK (not on PATH on this machine).
# ---------------------------------------------------------------------------
include(cmake/Shaders.cmake)

# ---------------------------------------------------------------------------
# Common compile settings for our own targets.
# ---------------------------------------------------------------------------
function(bulkhead_settings target)
  target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /MP /external:W0)
  target_compile_definitions(${target} PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endfunction()

# Platform-independent library: image I/O, logging, maze generation, overhead map.
# Linked by the game and by the unit tests.
add_library(bulkhead_lib STATIC
  src/core/Image.cpp       src/core/Image.h
  src/core/Log.cpp         src/core/Log.h
  src/world/Random.h
  src/world/Maze.cpp       src/world/Maze.h
  src/world/OverheadMap.cpp src/world/OverheadMap.h
  src/world/PathTrack.cpp  src/world/PathTrack.h)
target_include_directories(bulkhead_lib PUBLIC src)
target_link_libraries(bulkhead_lib PUBLIC bh_stb)
bulkhead_settings(bulkhead_lib)

# HLSL -> embedded bytecode headers.
bulkhead_compile_shader(FULLSCREEN_VS shaders/fullscreen.hlsl VSMain vs_6_0 g_FullscreenVS)
bulkhead_compile_shader(FULLSCREEN_PS shaders/fullscreen.hlsl PSMain ps_6_0 g_FullscreenPS)

add_executable(Bulkhead
  src/main.cpp
  src/core/CommandLine.cpp src/core/CommandLine.h
  src/core/Window.cpp      src/core/Window.h
  src/render/Gpu.cpp       src/render/Gpu.h
  src/render/Renderer.cpp  src/render/Renderer.h
  shaders/fullscreen.hlsl
  ${FULLSCREEN_VS} ${FULLSCREEN_PS})
set_source_files_properties(shaders/fullscreen.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(Bulkhead PRIVATE ${BULKHEAD_SHADER_GEN_DIR})
target_link_libraries(Bulkhead PRIVATE bulkhead_lib d3d12 dxgi dxguid)
bulkhead_settings(Bulkhead)
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Bulkhead)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
