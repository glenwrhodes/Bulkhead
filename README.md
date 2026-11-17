# Bulkhead

A ray-traced first-person shooter set in a randomly generated sci-fi ship maze. It runs
on a custom C++20 engine with DirectX 12 and DXR 1.1 (inline ray queries). The modular
ship kit is generated in Blender (`art/blender/`). See [docs/PLAN.md](docs/PLAN.md) for
the plan.

**Current state (round 4):**
- The generated ship is built from the Blender kit: corridors, rooms, doors and props.
- It is ray traced with soft shadows from every fixture light, one bounce of global
  illumination, glossy reflections and glass.
- You can walk it with keyboard and mouse, with collision. Doors open as you approach.
- The handgun is on screen and the minimap tracks you.
- The exit airlock ends the run.

## Build

Needs Visual Studio 2022 (MSVC), the Windows SDK (its `dxc.exe` is found automatically),
CMake 3.24 or newer, and a GPU with DXR tier 1.1 (RTX 20-series or newer).

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Play

```
build\bin\Release\Bulkhead.exe                 # a random ship
build\bin\Release\Bulkhead.exe --seed 1234     # a specific ship
```

| Key | Action |
|---|---|
| WASD / arrows | Move |
| Mouse | Look (click the window to grab the mouse) |
| Shift | Run |
| M or Tab | Full map |
| R | Ray tracing on/off |
| H | HUD on/off |
| N | New ship |
| F12 | Screenshot to `captures/` |
| Esc | Release the mouse; press again to quit |

Find the red airlock: it is the room farthest from where you start.

## Headless modes (testing and filming)

```
Bulkhead.exe --capture out --res 1920x1080 --fps 30 --spp 12   # walk start -> exit as PNG frames
Bulkhead.exe --shots out --res 1920x1080 --spp 32              # corridor/door/room/junction, RT on and off
Bulkhead.exe --bench out --res 1920x1080                       # window-mode frame times + sample frames
Bulkhead.exe --overhead map.png --seed 7                       # top-down map, no GPU
Bulkhead.exe --help
```

## Test

```
ctest --test-dir build -C Release -L "unit|smoke" --output-on-failure    # quick
ctest --test-dir build -C Release --output-on-failure                    # also the 1080p film captures
```

Captures go to `captures/` (git-ignored):

| Folder or file | Contents |
|---|---|
| `captures/walkthrough/` | The full 1080p walk |
| `captures/walkthrough_overhead_seed1234.png` | The map of that ship |
| `captures/shots/` | RT on/off pairs |
| `captures/bench/bench.log` | Window-mode timing |

## Layout

```
src/core     image + PNG, logging, command line, math, Win32 window (raw mouse)
src/assets   kit loader (cgltf): assets/*.glb -> shared vertex/index pool, parts, lights
src/world    maze generator, level builder (maze -> kit pieces, lights, colliders, props),
             collision, scripted walk path, overhead map
src/game     game state: player, doors, gun viewmodel, scripted walk, showcase viewpoints
src/render   D3D12 device; ray-traced renderer (BLAS per part, TLAS per frame, accumulation)
shaders/     trace.hlsl (path tracer, cs_6_5 RayQuery), present.hlsl (AgX tonemap + HUD)
art/blender  kit generator scripts and the .blend
assets/      exported kit (.glb) + manifest
tests/       unit tests, PNG checker, ctest definitions
```
