# Bulkhead

A ray-traced first-person shooter set in a randomly generated sci-fi ship maze. It runs
on a custom C++20 engine with DirectX 12 and DXR 1.1 (inline ray queries). The modular
ship kit is generated in Blender (`art/blender/`). See [docs/PLAN.md](docs/PLAN.md) for
the plan.

**Current state (round 5):**
- The generated ship (40 × 40 cells, 160 m square) is built from the Blender kit:
  corridors, rooms, doors and props.
- It is split into sections, each with a mood: **lit**, **dim** (warm, failing lights),
  **dark** (dead lights, red emergency lighting) and **derelict** (flickering and strobing
  tubes, rust and grime, haze). The deeper into the ship you go, the worse it gets.
- It is ray traced with soft shadows from every fixture light, one bounce of global
  illumination, glossy reflections, glass, and haze that the lights and your flashlight
  shine through.
- You can walk it with keyboard and mouse, with collision. Doors open as you approach.
- The handgun is on screen with a flashlight under the barrel. The minimap shows the cells
  around you, but only the ones you have seen.
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
| F | Flashlight on/off |
| M or Tab | Full map (only what you have seen) |
| R | Ray tracing on/off |
| H | HUD on/off |
| N | New ship |
| F12 | Screenshot to `captures/` |
| Esc | Release the mouse; press again to quit |

Find the red airlock: it is the room farthest from where you start.

## Headless modes (testing and filming)

```
Bulkhead.exe --capture out --res 1920x1080 --fps 30 --spp 12   # walk start -> exit as PNG frames
Bulkhead.exe --shots out --res 1920x1080 --spp 32              # fixed views incl. one corridor per mood, RT on and off
Bulkhead.exe --bench out --res 1920x1080                       # window-mode frame times (by mood) over the whole walk
Bulkhead.exe --overhead map.png --seed 7                       # top-down map with sections in colour, no GPU
Bulkhead.exe --help                                            # also --size, --flashlight on|off, --haze X
```

## Test

```
ctest --test-dir build -C Release -L "unit|smoke" --output-on-failure    # quick
ctest --test-dir build -C Release -L film --output-on-failure            # the 1080p film captures (~20 min)
ctest --test-dir build -C Release -L bench                               # the timing run (~2 min)
```

A single ctest command is cut off after 10 minutes on this machine, so run the two halves
of the walk film separately (`-R film_walkthrough_a`, `-R film_walkthrough_b`).

Captures go to `captures/` (git-ignored):

| Folder or file | Contents |
|---|---|
| `captures/walkthrough_a/`, `walkthrough_b/` | The 1080p walk from start to exit, in two halves (lit, dim, derelict, dark) |
| `captures/walkthrough_overhead_seed1234.png` | The map of that ship, sections in colour |
| `captures/shots/` | RT on/off pairs, including `shot_corridor_{lit,dim,dark,derelict}_rt.png` |
| `captures/bench_walk/bench.log` | Window-mode timing over the whole walk, by mood |
| `captures/bench/bench.log` | Short window-mode timing (smoke) |

Map colours: steel blue = lit, amber = dim, indigo = dark, rust = derelict; dashed white
gates mark the openings between sections.

## Layout

```
src/core     image + PNG, logging, command line, math, Win32 window (raw mouse)
src/assets   kit loader (cgltf): assets/*.glb -> shared vertex/index pool, parts, lights
src/world    maze generator, sections and moods, level builder (maze -> kit pieces, lights,
             colliders, props), collision, scripted walk path, overhead map
src/game     game state: player, doors, gun viewmodel, scripted walk, showcase viewpoints
src/render   D3D12 device; ray-traced renderer (BLAS per part, TLAS per frame, accumulation)
shaders/     trace.hlsl (path tracer, cs_6_5 RayQuery), present.hlsl (AgX tonemap + HUD)
art/blender  kit generator scripts and the .blend
assets/      exported kit (.glb) + manifest
tests/       unit tests, PNG checker, ctest definitions
```
