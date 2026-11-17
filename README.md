# Bulkhead

A first-person shooter inside a randomly generated sci-fi ship maze. It runs on a custom C++20
engine using DirectX 12 and DXR ray tracing. See [docs/PLAN.md](docs/PLAN.md) for the full plan.

**Current state (round 1):**
- The engine skeleton: a D3D12 window, plus headless GPU capture to PNG.
- The maze generator with its tests and the overhead map. The window shows the generated
  ship as a tactical map, with a marker walking the start-to-exit route.

## Build

Needs Visual Studio 2022 (MSVC), the Windows SDK (its `dxc.exe` is found automatically) and CMake 3.24 or newer.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Run

```
build\bin\Release\Bulkhead.exe                         # window: Esc quit, M map, N next seed, F12 screenshot
build\bin\Release\Bulkhead.exe --seed 42 --size 20x16  # a specific ship
build\bin\Release\Bulkhead.exe --capture out --frames 120 --res 1920x1080
build\bin\Release\Bulkhead.exe --overhead map.png --seed 7
build\bin\Release\Bulkhead.exe --help
```

## Test

```
ctest --test-dir build -C Release --output-on-failure          # everything (~75 s)
ctest --test-dir build -C Release -L "unit|smoke"              # quick (~7 s)
```

Capture tests write to `captures/`, which git ignores:

| Folder or file | Contents |
|---|---|
| `captures/smoke/` | 60 frames at 720p |
| `captures/round1_map_tour/` | 240 frames at 1080p, 30 fps |
| `captures/overhead_*.png` | Overhead maps for seeds 1234, 7, 42 and 2026, plus a 24×24 ship |

## Layout

```
src/core     image + PNG, logging, command line, Win32 window
src/render   D3D12 device (Gpu), Renderer (offscreen target -> swap chain or readback)
src/world    PCG32 RNG, maze generator + A*, overhead map, path track
shaders/     HLSL, compiled by dxc at build time into headers
tests/       maze_tests (unit), capture_check (PNG verifier), ctest definitions
docs/        plan
```
