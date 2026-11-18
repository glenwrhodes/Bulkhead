cmake_minimum_required(VERSION 3.24)
project(Bulkhead VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Bulkhead is a Windows / DirectX 12 project.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The C++ runtime is linked in statically, so the download needs no Visual C++
# redistributable: the EXE runs on a bare Windows 10/11.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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

# cgltf (glTF / .glb loader), pinned to a release tag.
FetchContent_Declare(cgltf
  URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.14.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(cgltf)
add_library(bh_cgltf STATIC third_party/cgltf_impl.cpp)
target_include_directories(bh_cgltf SYSTEM PUBLIC ${cgltf_SOURCE_DIR})
set_target_properties(bh_cgltf PROPERTIES FOLDER third_party)

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

# Platform-independent library (no D3D12): image I/O, logging, maze generation, overhead
# map, kit loading, level building, collision, the scripted walk and the game state.
# Linked by the game and by the unit tests.
add_library(bulkhead_lib STATIC
  src/core/Image.cpp       src/core/Image.h
  src/core/Log.cpp         src/core/Log.h
  src/core/Math.h
  src/assets/Kit.cpp       src/assets/Kit.h
  src/world/Random.h
  src/world/Maze.cpp       src/world/Maze.h
  src/world/Sections.cpp   src/world/Sections.h
  src/world/OverheadMap.cpp src/world/OverheadMap.h
  src/world/PathTrack.cpp  src/world/PathTrack.h
  src/world/Collision.cpp  src/world/Collision.h
  src/world/Level.cpp      src/world/Level.h
  src/world/WalkPath.cpp   src/world/WalkPath.h
  src/world/CrawlSurface.cpp src/world/CrawlSurface.h
  src/game/Game.cpp        src/game/Game.h
  src/game/Creature.cpp    src/game/Creature.h
  src/game/Hunter.cpp      src/game/Hunter.h
  src/game/Effects.h)
target_include_directories(bulkhead_lib PUBLIC src)
target_link_libraries(bulkhead_lib PUBLIC bh_stb bh_cgltf)
target_compile_definitions(bulkhead_lib PRIVATE BULKHEAD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
bulkhead_settings(bulkhead_lib)

# HLSL -> embedded bytecode headers. The tracer uses inline ray tracing (SM 6.5).
bulkhead_compile_shader(TRACE_CS   shaders/trace.hlsl   CSMain cs_6_5 g_TraceCS)
bulkhead_compile_shader(PRESENT_VS shaders/present.hlsl VSMain vs_6_0 g_PresentVS)
bulkhead_compile_shader(PRESENT_PS shaders/present.hlsl PSMain ps_6_0 g_PresentPS)

add_executable(Bulkhead
  src/main.cpp
  src/core/CommandLine.cpp src/core/CommandLine.h
  src/core/Window.cpp      src/core/Window.h
  src/render/Gpu.cpp       src/render/Gpu.h
  src/render/Renderer.cpp  src/render/Renderer.h
  shaders/trace.hlsl shaders/present.hlsl
  ${TRACE_CS} ${PRESENT_VS} ${PRESENT_PS})
set_source_files_properties(shaders/trace.hlsl shaders/present.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(Bulkhead PRIVATE ${BULKHEAD_SHADER_GEN_DIR})
target_link_libraries(Bulkhead PRIVATE bulkhead_lib d3d12 dxgi dxguid delayimp)
# D3D12 and DXGI load on first use, so the CPU-only modes (--check, --overhead, --help)
# start even on a machine without them (GitHub's build machines check the download so).
target_link_options(Bulkhead PRIVATE /DELAYLOAD:d3d12.dll /DELAYLOAD:dxgi.dll)
bulkhead_settings(Bulkhead)
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Bulkhead)

# ---------------------------------------------------------------------------
# Install = the downloadable game folder: the EXE, the models beside it, and a short
# text file. cmake/PackageZip.cmake installs it and zips it; cmake/CheckPackage.cmake
# unpacks the zip into an empty folder and runs it from there.
# ---------------------------------------------------------------------------
install(TARGETS Bulkhead RUNTIME DESTINATION .)
install(DIRECTORY assets/ DESTINATION assets FILES_MATCHING PATTERN "*.glb" PATTERN "kit_manifest.json")
install(FILES package/PLAY.txt package/THIRD_PARTY_NOTICES.txt DESTINATION .)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
