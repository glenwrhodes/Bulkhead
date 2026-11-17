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
| next | Creature model, rig and animation (Blender); grid-path AI with ray-query line of sight; shooting; weapon pickups; audio; denoiser | |

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
- **`shaders/present.hlsl`:** exposure, AgX tone mapping (the same view transform as the Blender renders), crosshair, minimap and full map.
- Rendering goes into an offscreen target, which is either presented or read back, so the window and captures share one path.

## Testing and filming

Bash on this machine only runs `cmake` and `ctest`, so the game runs inside ctest.

- `Bulkhead.exe --capture <dir>` renders on the GPU without a window. It writes `frame_0001.png` and onward, plus `overhead.png`, `capture.log` and `capture.json`.
- `Bulkhead.exe --overhead <file.png>` writes the top-down map using only the CPU.
- `capture_check` verifies that each PNG has the right size, is not a flat colour, and that the frames change over time.
- Test labels: `unit` (no GPU), `smoke` (short captures, including one with the D3D12 debug layer on), and `film` (1080p captures for the video).

## Blender asset kit (milestone 3)

Source files are in `art/blender/*.blend`, kept for filming. They are exported to `assets/*.glb`, which the engine loads with cgltf.

- **Corridor:** straight, corner, T, cross and end cap pieces, with angled floor and ceiling corners, inset panels, grated floor, light strips and cable trays.
- **Room:** floor, wall variants, corners, ceiling with light fixture, and a wall with a door opening.
- **Doors:** frame with hazard trim, a sliding door with a small window, a heavy bulkhead door, and an airlock door.
- **Pipes and props:** pipe runs, elbows, valves and junction boxes; barrels; crates; consoles; vents; lockers.
- **Weapon:** a sci-fi handgun for the first-person view.
- **Look:** off-white and gunmetal panels, orange hazard accents, cool teal and white light strips, and red emergency light near the exit.
