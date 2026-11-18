# Bulkhead

A ray-traced first-person shooter set in a randomly generated sci-fi ship maze. It runs
on a custom C++20 engine with DirectX 12 and DXR 1.1 (inline ray queries). The modular
ship kit is generated in Blender (`art/blender/`). See [docs/PLAN.md](docs/PLAN.md) for
the plan.

## Download and play

**[Download Bulkhead for Windows (zip)](https://github.com/glenwrhodes/Bulkhead/releases/latest/download/Bulkhead-windows-x64.zip)**

Unzip it anywhere and run `Bulkhead\Bulkhead.exe`. Nothing needs installing. Every push to
`master` is built on GitHub and published as a new [release](https://github.com/glenwrhodes/Bulkhead/releases).

**You need** Windows 10 (version 2004 or newer) or Windows 11, 64-bit, and a graphics
card with hardware ray tracing (DirectX 12 Ultimate, DXR 1.1): NVIDIA GeForce RTX
20-series or newer, AMD Radeon RX 6000-series or newer, or Intel Arc, with a current
driver.

Find the red airlock: it is the room farthest from where you start. The controls are in
the table under [Play](#play) and in `PLAY.txt` in the zip.

**Current state (rounds 5-11):**
- Ten things live in the ship: biomechanical crawlers (eyeless wedge skull, four petal
  jaws, hooked forelimbs, plated limbs, amber seams) that roam floors, walls and
  ceilings. Every so often one heads for roughly where you are, and they hear footsteps
  (running carries further; creeping crouched is silent). One that sees you stalks you
  along the ceiling and charges over the walls and ceiling. A few metres off it stops,
  rears, opens its jaws and hisses, sinks low, and leaps at your face from wherever it
  clings; if it reaches you, you die. Four hits kill one. Dead ones are replaced. A player exploring meets the first after about a
  minute (`--creatures N` and `--hunt original` change this).
- The generated ship (40 × 40 cells, 160 m square) is built from the Blender kit:
  corridors, rooms, doors and props.
- It is split into sections, each with a mood: **lit**, **dim** (warm, failing lights),
  **dark** (dead lights, red emergency lighting) and **derelict** (flickering and strobing
  tubes, rust and grime, haze). The deeper into the ship you go, the worse it gets.
- It is ray traced with soft shadows from every fixture light, one bounce of global
  illumination, glossy reflections, glass, and haze that the lights and your flashlight
  shine through.
- You can walk it with keyboard and mouse, with collision. Doors open as you approach.
- Derelict corridors have torn-open ceilings with hanging panels and live cables that spark,
  burst pipes venting steam, debris, red emergency beacons, and low pipe bundles you have
  to crouch under.
- You hold the BH-9 energy pistol in armoured gloves. It fires glowing bolts with a muzzle
  flash and recoil, and they leave sparks and scorch marks where they hit. A flashlight
  sits under the barrel. The minimap shows the cells around you, but only the ones you
  have seen.
- Corridors along the ship's edge have viewports out into space: stars, and glass that
  reflects the corridor (and whatever is crawling behind you).
- Sound: footsteps on the grating, the pistol and its hits, claws clicking as each foot
  lands, hissing, screeching, doors, sparks, steam and the ship's hum. It's placed round you,
  and muffled behind walls (`assets/sounds/CREDITS.md` says where each came from).
- The exit airlock ends the run.

## Build

Needs Visual Studio 2022 (MSVC), the Windows SDK (its `dxc.exe` is found automatically),
CMake 3.24 or newer, and a GPU with DXR tier 1.1 (RTX 20-series or newer).

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Play

From the download, run `Bulkhead.exe`. From a source build:

```
build\bin\Release\Bulkhead.exe                 # a random ship
build\bin\Release\Bulkhead.exe --seed 1234     # a specific ship
```

| Key | Action |
|---|---|
| WASD / arrows | Move |
| Mouse | Look (click the window to grab the mouse) |
| Left click | Fire |
| Ctrl or C | Crouch (to get under low pipes) |
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
Bulkhead.exe --bench out --res 1920x1080 --creatures 10        # the same with ten creatures loose, hunting the walk
Bulkhead.exe --encounters out                                  # no GPU: how long until an exploring player first sees a creature
Bulkhead.exe --overhead map.png --seed 7                       # top-down map with sections in colour, no GPU
Bulkhead.exe --help                                            # also --size, --flashlight on|off, --haze X, --hunt original
```

## Test

```
ctest --test-dir build -C Release -L "unit|smoke" --output-on-failure    # quick
ctest --test-dir build -C Release -L film --output-on-failure            # the 1080p film captures (~20 min)
ctest --test-dir build -C Release -L bench                               # the timing runs (~2.5 min each)
ctest --test-dir build -C Release -L encounters                          # time to first see a creature, before/after (~1.5 min)
```

A single ctest command is cut off after 10 minutes on this machine, so run the two halves
of the walk film separately (`-R film_walkthrough_a`, `-R film_walkthrough_b`).

Captures go to `captures/` (git-ignored):

| Folder or file | Contents |
|---|---|
| `captures/walkthrough_a/`, `walkthrough_b/` | The 1080p walk from start to exit, in two halves (lit, dim, derelict, dark; it crouches under a low pipe at 71 s and 140 s) |
| `captures/fire/` | Standing in a dark corridor and firing six shots (1080p, 6 s) |
| `captures/crawl/` | The creature: along the floor, up the wall, across the ceiling overhead (1080p, 15 s, seed 42) |
| `captures/hunt/` | A creature hunts the player down: stalks, charges over the walls, threatens, coils, leaps; the player dies (1080p, ~14 s) |
| `captures/approach_before/`, `approach_after/` | One that has spotted you closes in and leaps, before round 14 (a sprint along the floor) and after (over the walls, threat, coil, leap off the wall); same seed and start |
| `captures/approach/` | The approach on another ship (seed 42) |
| `captures/threat/` | The threat close up: on the ceiling of a lit corridor, reared down at you with its jaws open, then the coil and the leap |
| `captures/climb/`, `climb_overlay/` | The crawl as a showcase: floor, wall, the wall-to-ceiling edge, ceiling, down the far wall; then the same frames with the legs, footholds, step arcs, surface and gait drawn over them |
| `<capture>/audio.wav` | The sound of every capture, frame for frame, with `audio_events.csv` (what played when) and `audio_wave.png` |
| `captures/window/` | A viewport on the ship's edge: stars outside, the corridor and the creature reflected in the glass, the flashlight across it, then the creature on the wall (1080p, 17.5 s) |
| `video/blender/window/` | The viewport modelled in Blender step by step (`step_01..08.png`), Cycles finals, a Blender window screenshot |
| `captures/fight/` | The player kills one: four hits as it charges; it dies on its back (1080p, ~11 s) |
| `video/blender/creature/` | Blender renders of the creature: turntable, hero shot, crawl cycle |
| `captures/walkthrough_overhead_seed1234.png` | The map of that ship, sections in colour |
| `captures/shots/` | RT on/off pairs, including `shot_corridor_{lit,dim,dark,derelict}_rt.png` and `shot_{broken_ceiling,steam,low_pipe,beacon}_rt.png` |
| `captures/bench_walk/bench.log` | Window-mode timing over the whole walk, by mood |
| `captures/bench/bench.log` | Short window-mode timing (smoke) |
| `captures/bench_creatures/bench.log`, `bench_creatures_original/` | The walk's timing with 10 creatures loose (and with the original 3) |
| `captures/encounters_new/encounters.log`, `encounters_original/` | Time until an exploring player first sees a creature: the summary is at the end; `encounters.csv` has every run |

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
package/     PLAY.txt and third-party notices that go in the download
cmake/       shader compiler lookup; PackageZip / CheckPackage (the download); TestSummary
.github/     the GitHub Actions build: unit tests, the zip, the release
```

## Releases

`.github/workflows/build.yml` runs on every push to `master`. It builds on a clean
Windows machine, runs the unit tests, packs the zip (`cmake/PackageZip.cmake`), unpacks it
into an empty folder and starts it from there (`cmake/CheckPackage.cmake`: `--help`, and
`--check`, which loads the models from beside the EXE and builds a ship without a GPU). It
then publishes the zip as release `build-<n>`. GitHub's machines have no ray-tracing GPU, so
the capture tests run on the development PC. `ctest -L package` there does the same
unpack-and-run check, plus a short ray-traced capture.

## Copyright

Copyright © 2026 Tiny Moosh Games Inc. All rights reserved. Third-party code (stb, cgltf)
is under its own licenses; see `package/THIRD_PARTY_NOTICES.txt`.
