# Bulkhead: game and engine plan

A first-person shooter set inside a derelict sci-fi ship. Each level is a randomly generated
maze of corridors, rooms and doors on a grid. Creatures roam the ship and hunt you; you find
weapons (starting with a sci-fi handgun) and escape through the exit airlock.
Custom C++20 engine on DirectX 12 with DXR ray tracing (RTX 4090 Laptop GPU, ray tracing tier 1.2).

## Milestones

| # | Milestone | Status |
|---|---|---|
| 1 | CMake skeleton: D3D12 window, offscreen render target, headless capture to PNG, ctest | **Done (round 1)** |
| 2 | Maze generator + unit tests + overhead map PNG + A* route | **Done (round 1)** |
| 3 | Blender kit round 1: corridor pieces, room pieces, door frame + windowed sliding door, pipes, barrels, crates, handgun, exported to .glb | **Done (round 2)**: 23 pieces in `assets/`, see `art/blender/README.md` |
| 4 | Build the maze from the kit; ray-traced lighting | **Done (round 4)**: inline-RT path tracer (see Rendering) |
| 5 | First-person walking with collision, doors that open, gun viewmodel, scripted walk-through capture | **Done (round 4)** |
| 6 | Part 2, round 5: 40 × 40 ship split into sections with moods (lit, dim, dark, derelict); lights that flicker, strobe, die or pulse red; flashlight; procedural grime and rust; haze with lit beams; zoomed minimap that only shows explored cells; slower camera bob; timing run by mood | **Done (round 5)**: see Sections and moods |
| 7 | Round 6: derelict Blender pieces (broken ceiling panels, hanging wires, open pipe ends, low pipes, debris, beacons), sparks, steam, crouch, the long walk film | **Done (rounds 6-7)**: see Derelict dressing and the gun |
| 8 | Round 7: arms holding the gun, firing (muzzle flash light, bolt, impacts, recoil) | **Done (rounds 6-7)** |
| - | Download: GitHub Actions builds every push to master and publishes `Bulkhead-windows-x64.zip` as a release | **Done** (`.github/workflows/build.yml`, `cmake/PackageZip.cmake`, `cmake/CheckPackage.cmake`) |
| 9 | Round 8: the creature (rigid armoured plates, Blender rig and crawl cycle), crawling on floors, walls and ceilings | |
| 10 | Round 9: hunting AI, leap, player death; four hits to kill | |
| 11 | Round 10: game loop with several creatures; full film pass | |

## Maze generation (`src/world/Maze.*`)

- The grid is made of **4 m cells**, with a 3.5 m ceiling. 1 Blender unit = 1 m. Each cell is `Void` (solid ship machinery), `Corridor` or `Room`. Each edge between cells is a `Wall`, `Open` or `Door` (Sliding with a window, Bulkhead, or Airlock).
- **Rooms first:** rectangles of 2–4 cells, sometimes +1 along one axis, are placed so at least one cell separates them. Placement stops at about 38% coverage.
- **Corridors:** a depth-first carver fills the rest of the grid. It prefers to keep going straight, because ship corridors are long runs.
- **Connections:** a random spanning tree over regions (rooms and corridor networks) guarantees that everything is connected. Every room–corridor joint becomes a door. Rooms then get extra doors, which creates loops so creatures can flank you.
- **Dead ends:** some are braided into loops; others are retracted back to the nearest junction. The ones left over become closets for weapon pickups.
- **Start and exit:** the start is the room nearest a random corner. The exit is the airlock room farthest away by walking distance. Doors into the airlock are Airlock doors.
- **Determinism:** a custom PCG32 RNG is used instead of `<random>` distributions, which differ between implementations. A seed gives the same ship everywhere.
- **Tests** (`tests/maze_tests.cpp`) check 920 mazes from 6×6 to 64×48:
  - Edges are symmetric, and the border is sealed.
  - Doors only join a room to a corridor.
  - Every room has a door, and no two rooms touch.
  - Everything can be reached from the start.
  - The exit is the farthest room.
  - The A* route is valid and as short as the BFS distance.
  - The same seed always produces the same layout.

## Sections and moods (round 5, `src/world/Sections.*`)

The ship is 40 × 40 cells (160 m square, about 1500 walkable cells, 4000 placed models,
2000 lights). It is split into about 17 **sections**, each with a **mood**:

| Mood | Lights | Wear | Haze (1/m) | Map colour |
|---|---|---|---|---|
| Lit | full, cool white; 5% flicker | 0.12 | 0.002 | steel blue |
| Dim | 17%, warm amber; 15% flicker, 23% dead | 0.4 | 0.007 | amber |
| Dark | 72% dead, the rest red emergency (pulsing) | 0.55 | 0.012 | indigo |
| Derelict | 40% dead, 26% flicker, 10% strobe, the rest 30%, sickly yellow | 0.92 | 0.016 | rust |

- **Sections** grow at the same time from anchors spread out by walking distance (the first anchor is
  the start). Rooms join a section whole, so every section is one connected piece.
- **Moods** get darker with depth: sections are ranked by the walking distance of their anchor from
  the start, the moods are dealt out in the order lit, dim, derelict, dark, and the ranks are
  shuffled a little. The start section is always lit. Neighbouring sections with the same mood are
  swapped apart where possible.
- **Per cell:** the light behaviour (`CellPower`: steady, flicker, strobe, dead, emergency), a wear
  value and a haze density. Wear and haze are softened across section borders. Rooms get 35% of the
  haze, because their size and number of lights would otherwise turn it into steam. The spawn
  room is steady; the exit airlock pulses red.
- **Determinism:** sections use their own `Rng` streams, so the maze (and `rng(1234).next()`) is
  unchanged. Flicker is a pure function of time, so a capture is the same every run.
- **Game:** every frame `Game` turns the cell powers into each light's radiance and each
  instance's emissive multiplier (dead panels go dark, emergency panels glow red). When the
  lighting near the camera changes a lot, the window keeps only 3 frames of history so flicker
  stays sharp.
- **Map:** the overhead map tints floors by mood and marks the openings between sections with
  dashed gates. In game, the minimap is a 13-cell window around the player, and both maps only
  show cells the player has seen (a whole room on entering it, straight down open corridors, side
  openings, and through doors while they are open).
- **Camera bob:** one bob per footstep at `1.55 + 0.17 × speed` steps a second (2.0 Hz on the
  scripted walk, was 3.5), with a small sideways sway at half that rate.

## Derelict dressing and the gun (rounds 6-7)

**Pieces** (Blender: `bk_corridor.py` broken variant, `bk_derelict.py`, `bk_arms.py`):
- `corridor_straight_broken` has the north ceiling tile torn open, showing ducts in the space above. One panel hangs from its hinge, the light fixture dangles on two cables, and there are sagging cable loops and bare ends.
- `pipes_low` is a hazard-striped bundle across the corridor, 1.30 m clear underneath. `pipe_burst` is a torn coolant pipe with jagged ends, rust runs and a puddle. `debris` is a fallen panel, chunks and a bent conduit. `beacon` is a red emergency lamp with its own light.
- `arms` are armoured gloves and suit sleeves in a two-handed grip, modelled in the gun's space. `muzzle_flash`, `bolt`, `spark` and `scorch` are the effect meshes.
- **Effect markers:** empties named `<piece>_fx_<name>` (`spark0..3`, `steam`, `muzzle`) are exported as glTF nodes. `Kit` reads them into `KitModel::markers`.

**Placement by mood** (`Level.cpp`) uses its own `Rng` stream, so the room walls and props are unchanged:

| | Lit | Dim | Dark | Derelict |
|---|---|---|---|---|
| broken ceiling (straight cells) | — | 5% | 10% | 35% |
| burst steam pipe | — | 10% | 8% | 20% |
| debris | — | 6% | 12% | 30% |
| beacon | — | — | 30% | 4% |

Low pipes go in two straight cells on the walk route, about a third and two thirds of the way and outside lit sections, so the film always ducks under one. Each other non-lit straight cell has a 3% chance of one. Seed 1234 has 64 broken ceilings, 46 steam pipes, 63 debris piles, 28 beacons, 7 low pipes, 256 spark emitters and 46 steam emitters.

**Crouching:**
- Collision boxes have an optional `bottom`. Overhead boxes only block bodies taller than that: standing is 1.85 m, crouching 1.2 m.
- In the window, Ctrl or C crouches; the eye drops from 1.7 to 1.05 m and speed halves. Walking into a pipe standing up stops you. Once under it, you stay crouched until you're clear.
- The scripted walk crouches within about 2 m of a low pipe and slows to 1.25 m/s there. A distance-to-time table keeps the walk deterministic.

**Effects:** all of them are a pure function of time, so captures are repeatable and `setWalkTime` can jump anywhere.
- **Sparks:** each spark emitter may burst once per 0.45 s slot (about every 2.5 s). A burst throws 16 streaks along analytic paths, each bouncing once off the floor, plus a 0.16 s orange flash light.
- **Steam:** steam emitters hiss on and off per 3.2 s slot. Each frame the CPU sums the light falling on every plume within 24 m.
- **Short-lived lights:** up to 16 a frame (spark flashes, the muzzle, bolts, impacts), sorted by brightness over distance. The renderer shades every pixel with them, shadowed on primary hits. The brightest also lights the haze.
- **Steam rendering:** a buoyant, widening cone with animated fBm turbulence, ray-marched in 14 steps inside its bounding sphere. It is lit by the fixtures, the flashlight and the short-lived lights.

**The gun:**
- **Firing:** left click fires every 0.28 s. The camera ray finds what the crosshair is on (`raycastLevel`: floor, ceiling and collision boxes, stepped and refined by bisection). A 70 m/s bolt then flies there from the muzzle.
- **Impacts and flash:** on arrival there are impact sparks, a 0.14 s cyan flash light, and a scorch decal that stays (the 48 newest). The muzzle flash mesh shows for 50 ms with a random roll and size, plus a 70 ms light.
- **Recoil:** the gun kicks back and up over about 0.1 s, and the aim kicks up 0.012 rad.
- **Viewmodel:** the arms share the gun's matrix. It sits 12.5 cm right, 13 cm down and 34 cm ahead of the eye.
- **Ray masks:** sparks and bolts have their own instance mask (`0x08`). Primary rays see them; shadow and bounce rays ignore them.

**Captures:**
- `--scene fire` stands in a dark corridor and fires six shots at walls and floor, easing the aim between targets (`film_fire`, 180 frames).
- `--shots` adds `broken_ceiling`, `steam`, `low_pipe` and `beacon`.
- The long walk (`film_walkthrough_a`/`_b`) ducks under both route pipes, at 71 s in a dim section and 140 s in a derelict one.

Timing run after rounds 6-7: median 6.7 ms (lit 7.5, dim 7.6, derelict 5.8, dark 5.4 ms).

## Rendering (round 4)

The plan said "raster G-buffer + DXR shadows". It became a full inline ray tracer, which is
simpler (one compute shader, no raster pipeline) and does more.

- **Geometry:** every kit part has its own bottom-level acceleration structure (BLAS), built
  once from one shared vertex/index pool. A top-level structure (TLAS) is rebuilt every
  frame from the instance list, because doors slide and the gun follows the camera.
  Instance masks separate the world, the gun and glass.
- **`shaders/trace.hlsl`** (`cs_6_5`, `RayQuery`), per pixel:
  - **Primary ray:** jittered, with back faces culled to match the Blender kit. Glass is passed through with a tint.
  - **Direct light:** every fixture light in the 3×3 surrounding cells, found through a per-cell light grid. Each gets a shadow ray to a random point on the light, which gives soft shadows.
  - **One diffuse bounce (GI):** lit by the hit point's direct light and emission.
  - **One glossy reflection ray:** skipped on rough surfaces.
  - **Clamping:** secondary radiance is clamped against fireflies.
- **Accumulation:**
  - Window: temporal reprojection, which checks that the reprojected pixel saw the same surface.
  - Captures: N sub-frames averaged from a still camera.
- **RT off:** unshadowed direct light plus flat ambient, with no bounce or reflections. Used for the comparison shots.
- **Round 5 additions:**
  - **Lights each frame:** the light buffer is re-uploaded every frame with current radiance, and dead lights skip their shadow rays (dark sections are the fastest to render).
  - **Instance emissive:** each instance's emissive is multiplied by the power of its cell.
  - **Wear:** procedural grime patches, narrow rust runs down walls, dirt on floors and soot on ceilings, scaled by the cell's wear and blended between cells. Grimy areas also get rougher and less metallic.
  - **Flashlight:** a shadowed spot light just past the gun muzzle (46 W/sr, 9°/24° cone and a faint spill), also lighting bounces.
  - **Haze:** single scattering along the primary ray. Two stratified distances each pick one fixture in proportion to its unshadowed contribution and send one shadow ray to it. One equiangular sample for the flashlight starts just past the light, where the ray enters the cone.

Timing run (`ctest -L bench`, whole 200 s walk, 1080p, 1 sample per pixel plus temporal history, RTX 4090 Laptop): **median 6.5 ms**, 95th percentile 8.9 ms. By mood: lit 7.6 ms, dim 7.7 ms, derelict 5.6 ms, dark 4.9 ms (medians).
- **`shaders/present.hlsl`:** exposure, AgX tone mapping (the same view transform as the Blender renders), crosshair, minimap and full map.
- Rendering goes into an offscreen target, which is either presented or read back, so the window and captures share one path.

## Testing and filming

Bash on this machine only runs `cmake` and `ctest`, so the game runs inside ctest.

- `Bulkhead.exe --capture <dir>` renders on the GPU without a window. It writes `frame_0001.png` and onward, plus `overhead.png`, `capture.log` and `capture.json`.
- `Bulkhead.exe --overhead <file.png>` writes the top-down map using only the CPU.
- `capture_check` verifies that each PNG has the right size, is not a flat colour, and that the frames change over time.
- Test labels:
  - `unit`: no GPU.
  - `smoke`: short captures, including one with the D3D12 debug layer on.
  - `film`: 1080p captures for the video. The walk is split into `film_walkthrough_a` and `_b`; run them one at a time, because a single ctest command is cut off after 10 minutes.
  - `bench`: the timing run over the whole walk.
  - `diag`: diagnostics, not part of the normal runs.

## Blender asset kit (milestone 3)

Source files are in `art/blender/*.blend`, kept for filming. They are exported to `assets/*.glb`, which the engine loads with cgltf.

- **Corridor:** straight, corner, T, cross and end cap pieces, with angled floor and ceiling corners, inset panels, grated floor, light strips and cable trays.
- **Room:** floor, wall variants, corners, ceiling with light fixture, and a wall with a door opening.
- **Doors:** frame with hazard trim, a sliding door with a small window, a heavy bulkhead door, and an airlock door.
- **Pipes and props:** pipe runs, elbows, valves and junction boxes; barrels; crates; consoles; vents; lockers.
- **Weapon:** a sci-fi handgun for the first-person view.
- **Look:** off-white and gunmetal panels, orange hazard accents, cool teal and white light strips, and red emergency light near the exit.
