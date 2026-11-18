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
| 9 | Round 8: the creature (rigid armoured plates, Blender rig and crawl cycle), crawling on floors, walls and ceilings | **Done (round 8)**: see The creature |
| 10 | Round 9: hunting AI, leap, player death; four hits to kill | **Done (rounds 9-10)**: see Hunters |
| 11 | Round 10: game loop with several creatures; full film pass | **Done (rounds 9-10)** |
| 12 | Part 3, round 11: measure how long it takes to meet a creature; more of them, drifting toward the player and hearing footsteps; frame rate with them loose | **Done (round 11)**: see Finding the creatures |
| 13 | Round 12: viewports into space on the ship's edge (Blender piece built in steps, stars, glass that reflects), the window film | **Done (round 12)**: see Viewports |
| 14 | Round 13: sound (mixer, window output, WAV beside every capture, ~25 sounds, credits) | Planned |
| 15 | Round 14: the approach (crawl in on walls and ceiling, stop, hiss, crouch, leap from any surface) | Planned |
| 16 | Round 15: lights without a cap: sample a few lights per pixel by importance instead of shadow-testing all of them (see Viewports, "How many lights") | Proposed |
| 17 | Round 16: the climbing film (floor, wall, ceiling, wall, floor, close up) | Planned |
| 18 | Round 17: the motion scanner (blips on the minimap and map) | Planned |
| 19 | Round 18: final pass (re-measure, every film with sound) | Planned |

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

## The creature (round 8)

**The model** (`art/blender/scripts/bk_creature.py`, our own design): about 2.4 m long, low to the ground, built from rigid graphite-black armour plates on a ribbed bone frame.
- **Head:** an eyeless, flat wedge of three overlapping crown plates with bone rims. Four petal jaws (top, bottom, left, right) are lined with needle teeth and split open in a cross, with a ring of teeth and an amber glow in the throat.
- **Back and flanks:** a spine ridge of raked bone fins. Tendons like armoured cable conduit run along the flanks, with exposed ribs below.
- **Limbs:** four spindly walking legs with high spiked knees and bowed, serrated lower legs ending in hooked claws. Two plated forelimbs fold under the head, ending in scythe hooks.
- **Tail:** six segments ending in a hook.
- **Glow:** amber seams under every plate's front edge, along the flanks and at each lower-leg joint.
- **Mesh:** 27 rigid parts (`creature_<bone>`, 112k triangles), with `fx_bone_<bone>` pivots and `fx_foot_<leg>` claw tips.

**In Blender:** `build_rig()` makes `CREATURE_RIG`, an armature with the parts parented to bones and two-bone IK legs. Each leg's pole angle was found by search so the rest pose is exact. The rig has a looping 32-frame crawl (`CR_crawl`): diagonal pairs step in turn, the body bobs, the spine and tail sway, and the jaws breathe. Renders are in `video/blender/creature/`: `turntable/` (36 frames), `hero_creature.png` and `crawl/` (32 frames).

**In the engine** (`src/world/CrawlSurface.*`, `src/game/Creature.*`):
- **Crawl surface:** the corridor's exact cross-section (`bk_corridor.py` OUTLINE) is a closed loop swept along a smoothed cell path. A point on it is (s along the path, u round the loop): u = 0 is the floor, `rightWallU` the wall, `ceilingU` the ceiling. Normals blend round the corners.
- **Body:** u is a function of s, so the head climbs first and the body follows. Pelvis, waist, thorax, neck and head each sit on the surface at their own s, lifted by their rest height along the normal. The tail trails the pelvis and sways.
- **Gait:** computed from distance travelled, not time, so the same distance always gives the same step. Diagonal pairs alternate with a stride of 0.55 m, and each foot is planted for 62% of the cycle. Each foothold depends only on its step number, so a planted claw never slides. It is pulled in or pushed out round the cross-section until the hip can reach it over the whole stance, which matters when the body rolls onto a wall. In the air a claw arcs out and stays within reach.
- **IK:** two bones, with knees up and out. Each rigid part's frame maps its rest bone onto the solved one.
- **Head:** turns toward a look target (limited), and the jaws open up to 0.6 rad.
- **Glow and doors:** the glow multiplies the amber emissive. Doors open for the creature as well as the player.

**Crawl film** (`--scene crawl`, `film_crawl`, seed 42, 15 s):
- **Corridor choice:** the scene picks a straight corridor of at least five cells, preferring dim, then derelict sections. Runs with low pipes are rejected; broken ceilings and props count against a run.
- **Camera:** stands about 13 m from the far end and tracks the head with a short lag. The flashlight follows its aim.
- **Path:** the creature comes in from beyond the far end. It crawls along the floor, climbs the right wall and stops there to hiss at you, then crosses the ceiling. The film ends with it right overhead.
- **Seed:** 42, because seed 1234's best corridor is fully lit.

`level_tests` checks the parts and markers, the cross-section, and that the scene reaches both wall and ceiling. It also checks that planted claws don't slide and that every leg reaches its foothold.

**Lighting fixes this round:**
- Fixture lights fall off as I/(d² + 0.12), and the flashlight as I/(d² + 0.35). Before, a surface brushing a light (the creature on the ceiling, the hanging panel in the flashlight) blew out to white.
- The muzzle flash now has three layers: a white-hot core, cyan inner petals and deep-blue outer petals.

## Hunters (rounds 9-10, `src/game/Hunter.*`)

**The model rework** (`bk_creature.py`), for more mass and more surface:
- **Limbs** are built by `plated_limb()`: a sinew core, three or four overlapping armour sleeves (each flaring to a bone lip with a glowing gap), armoured tendon conduits, and spines down the back. The thighs are about 1.6 times as thick as before.
- **Back and skull:** `dorsal_detail()` adds paired dorsal conduits, transverse bone ribbing and flank nodules on the pelvis and thorax. The skull gets five bone ridges and four cheek-to-neck conduits.
- **Materials:** the chitin is wetter (roughness 0.13, metallic 0.4) and the bone darker and waxier. The amber glow is at strength 16, and there's a glowing ring at the root of the jaws.
- **Size:** 213k triangles in total.

**AI states** (fixed steps, its own `Rng`, so captures are repeatable):

| State | What it does |
|---|---|
| Roam | Wanders to cells 4–9 away, mostly out of lit sections, on the ceiling (50%), a wall or the floor. It avoids U-turns unless cornered. 1.1 m/s |
| Stalk | Sees you (line of sight through the level, up to 22 m): a 1.1 s pause to hiss, then it closes in along the ceiling at 1.35 m/s, glow brightening |
| Chase | Within 7.5 m, after 4.5 s of stalking, when hit, or when you fire within 30 m: it rushes along the ceiling at 4 m/s and drops to the floor for the last three cells, re-aiming at your cell at every cell centre |
| Leap | Within 3.3 m with a clear line: a 0.42 s lunge at your face, scythes raised, legs thrown forward, jaws wide. If it misses, it lands and charges again |
| Feed | It has you: the death scene |
| Dying / Dead | Four hits: it spasms, glow flaring, falls onto its back (rolling over if it was on the floor or a wall), and curls its legs over its belly as the glow dies. The body stays |

- **Surfaces:** each cell on its path gets one. Room cells and door crossings are always floor. Walls only appear in straight corridor cells; at junctions they become ceiling. The surface changes smoothly between cell centres, and the cells just behind it keep theirs when it re-routes, so a new path never jumps under it.
- **Hits:** 18 spheres on its bones are tested against the camera ray. A hit makes it flinch for 0.35 s (it jolts back, the head pitches up, the jaws snap and the glow flares) and slows it. Hits throw amber sparks and an amber light, and leave no scorch.
- **Light:** each hunter carries two amber lights, at the throat and above its back. In the dark it shows as a shape against its own glow.
- **Death scene:** the camera is knocked back and down to the floor, looking up, with shaking. The creature sits on your face with its scythes raised and claws spread, biting in rhythm. Red drains the colour and closes in from the edges, then it fades to black. The window restarts the run after 4.6 s.
- **Window play:** `--creatures N` (default 3; 10 since round 11) spawns them in non-lit sections at least 12 cells' walk from you. A dead one is replaced 30 s later, at least 14 cells away. The window title counts kills.
- **Films:**
  - **`--scene hunt` (`film_hunt`, seed 1234, derelict corridor):** you walk in, it stalks on the ceiling, you back off and hit it twice, it charges, leaps, and you die. The scene ends 3 s into the fade.
  - **`--scene fight` (`film_fight`):** you hold the corridor; four hits, and its last lunge ends with it dead on its back.
  - The `level_tests` hunter checks require: the hunt reaches stalk, chase, leap and feed; the fight kills the creature with the player alive; three hits leave it alive and four kill it; and roaming hunters never move more than 0.4 m in a step.

## Finding the creatures (round 11)

Glen walked the ship for 5-10 minutes before he met a creature. Three of them roamed
about 1500 walkable cells, wandering 4-9 cells at a time with nothing drawing them toward
the player, and they only noticed you on sight (22 m) or within 5 m.

**The encounter timer** (`src/game/Encounters.*`, `Bulkhead.exe --encounters <dir>`, no GPU):
- A simulated player walks the ship at the window's walking speed (3.4 m/s, never stopping),
  always heading for the nearest cell the map hasn't shown yet and facing where it walks.
  It passes through props, never crouches and never fires.
- The real `Game` and creature AI run around it at 30 steps a second. A run lasts 10 minutes;
  after a death it restarts at the spawn, as the window does.
- **Seen:** a live creature's head or body within 25 m, inside the window's field of view
  (65° tall, about 97° wide), with nothing in the way (`raycastLevel`). Lighting is ignored.
- **Encounter:** seen, or killed by one never seen (an ambush).
- 10 ships (1234, 7, 42, 2026, 1-6) x 3 runs. Each run varies the player's choices and
  where the creatures start (`spawnHunters(count, salt)`). 30 runs take 15-70 s on all cores.
- ctest: `encounters_original` and `encounters_new` (label `encounters`), `encounters_smoke`
  (unit), and `encounters_sweep` (diag: the grid used to pick the numbers).

**Before and after** (30 runs of 10 minutes; "+" means some runs had none, counted as 600 s):

| | Before (3, no drift) | After (10, drift, hearing) |
|---|---|---|
| First seen: mean / median | 279 s+ / 237 s (5 of 30 runs: none in 10 min) | **69 s / 57 s** (none: 0) |
| First encounter (seen or killed): mean / median | 232 s+ / 159 s | 65 s / 53 s |
| First within 25 m (what a scanner would show) | 128 s+ / 97 s | 30 s / 31 s |
| Killed before ever seeing one | 6 of 30 runs | 3 of 30 |
| Sightings per 10 min / creature seconds in view | 2.5 / 2.7 s | 8.9 / 10.8 s |
| First seen on floor / wall / ceiling / mid-leap | 7 / 2 / 13 / 3 | 9 / 4 / 15 / 2 |

The "before" column is the unchanged code (kept in `captures/encounters_baseline_before_round11`).
`--hunt original` replays those numbers but includes this round's path fixes (below), which
moves its mean first sighting to 303 s+, within the run-to-run spread.

**What changed** (`HuntTuning`, `--hunt new|original`, `--creatures N` up to 24):
- **Ten creatures** (was 3). With more than 3 they spawn at least 6 cells apart (was 8), so
  all ten fit.
- **Drift:** each roaming creature arms a timer (15-30 s). When it runs out, the creature
  heads for a cell within 4 cells of the player (never closer than 2) from its next cell
  centre, at 1.5 m/s (roaming is 1.1). Drift goals and, in this tuning, wander goals avoid
  corridor closets, because reaching one means turning round (see below).
- **Hearing:** footsteps carry 16 m of walking distance (running 35 m; creeping crouched is
  silent). A roaming creature that hears you goes to look from its next cell centre.
- **The sweep** (8, 10, 12 creatures x drift every 25-50 or 15-30 s, 20 runs of 6 min):
  - Waiting for the end of a leg made the drift timing irrelevant, because a leg takes
    20-50 s. Drifting from the next cell centre fixed that.
  - 12 creatures gave the same numbers as 10, because only about 10 fit at 8 cells apart.
  - 10 creatures with drifts every 15-30 s landed in the 45-75 s target.

**Path changes that moved the creature** (found by the new 150 s, 10-creature check in
`level_tests`):
- **A leg ending on a turn.** A wandering leg ended exactly on a cell centre. If the next leg
  turned there, the rounded corner (Chaikin, up to 1.1 m cut each side) runs up to about
  half a metre from the centre, so the creature snapped sideways by up to 1 m.
  - This was in the game before this round, with the original tuning too: 12 such snaps in
    150 s across 10 creatures.
  - Fix: one cell before the end, the next leg is planned with `routeAhead`. It keeps the old
    path up to and including the next cell, so the new corner begins metres ahead.
  - Drift and hearing re-routes use `routeAhead` as well.
- **A route that loops back past itself.** `CrawlPath::project` searched the whole path for
  the closest point, so a cell centre could snap onto a later stretch that passes nearby
  (a 5 m jump).
  - Fix: `routeAhead` projects each cell centre only within 8 m after the previous one
    (`project(p, from, to)`).
- **Turning round (known issue, not fixed).** When the only way on is back (a dead end, a
  room with one door), the new path starts where it stands. The body behind the head piles
  up on that point and unfolds as it moves off: a 0.7 m snap.
  - It happened once in 150 s across ten creatures, with either tuning.
  - `Hunter::uTurns()` counts these, and `level_tests` prints them instead of failing.
  - It needs a real turning animation (round 13).
- The biggest step outside a turn is now 0.17 m (round 11 tuning) and 0.12 m (original).
  The hunt and fight scenes play exactly as before.

**Frame rate** (`bench_creatures` / `bench_creatures_original`, label `bench`; the walk at 1080p,
1 spp + history):
- **How the bench runs:** the creatures hunt the walk but can't grab it.
  - At first, a creature whose leap reached the camera just landed and leapt again. With
    ten, several piled up on the camera, and 2 lights each shaded every pixel: median
    12 ms, 15 ms in dark sections.
  - Now one that reaches the camera is replaced far away, as if it had been killed.
  - In play, only the **three nearest creatures cast light**. The rest still glow, they just
    don't light their surroundings.

| | Median | 95th | With a creature in view (median) | Creature AI + lights, CPU (median / 95th / worst) |
|---|---|---|---|---|
| No creatures (`bench_walk`) | 7.2 ms | 9.7 ms | - | - |
| 3, original tuning | 7.8 ms | 11.0 ms | 6.1 ms | 0.18 / 0.31 / 8.8 ms |
| 10, round 11 (two runs) | 6.8-8.1 ms | 9.2-11.6 ms | 8.6-9.1 ms | 0.64 / 1.08 / 23 ms |

Run to run, the same section varies by 0.5-1 ms on this laptop, so the cost of ten
creatures is within that. The 23 ms CPU worst case is a one-frame spike, most likely a
creature being replaced (a new `Creature` built from the kit plus a path over the whole
ship); that hasn't been confirmed.

## Viewports (round 12)

Windows on the ship's edge that look out into space, modelled in Blender and shown step by
step for the video.

**The piece** (`art/blender/scripts/bk_window.py`, `corridor_straight_window`, 50k triangles):
- **Base:** a straight corridor. Its east wall has two rounded viewports, one either side of the
  cell's central rib, which becomes the mullion. The cell ends keep the normal wall profile, so it
  joins any neighbour.
- **Each viewport:**
  - A flat wall plate with a rounded opening (1.16 × 1.10 m, corners 0.22 m).
  - A reveal: a 4 cm lip, then the hull's thickness splayed 8 cm toward the glass at
    x = 1.50, open to space beyond x = 1.64.
  - A bezel standing 45 mm proud, with 18 hex bolts.
  - A rubber gasket and the pane, in the new `BH_ViewGlass` (dark base colour, alpha 0.12,
    roughness 0.02: almost no diffuse, so it reads as clear).
- **Below:** a sill with a teal light strip under it, and stencils (`VIEWPORT 7-A`/`7-B`,
  `PRESSURE HULL - DO NOT STRIKE`) with a green pressure light.
- **Built in stages** (`build_window(parent, upto)`): 1 the corridor, 2 the panels cut away
  (`wall_run(omit=...)`, the trim rail capped), 3 the plate, 4 the reveals, 5 the bezels, 6 the
  gaskets and glass, 7 the sill and stencils.
- **Renders:** `video/blender/window/step_01..08.png` (step 8 adds `bk_studio.space_world()`,
  procedural stars and a galactic band). `hero_window.png` and `hero_window_looking_out.png` are
  Cycles at 1080p. `ui/blender_window_piece.png` is the Blender window with the piece selected.
- **Two corrections along the way:**
  - The first version's bezels cut through the thin pipe at z = 2.13 and a handrail crossed the
    glass. The openings were lowered and shortened, and the handrail dropped.
  - A light base colour at 10% opacity made the glass look frosted.

**Placement** (`Level.cpp`):
- **Where:** about half the straight corridor cells whose wall is on the ship's edge, turned so
  the windows face out. Their own `Rng` stream.
- **The dressing stream:** a window cell still takes every draw the dressing stream would have
  (the broken-ceiling roll and its four spark phases, and any wall piece that would have gone on
  the window wall, skipped with `skipDraws`), so every other cell is dressed exactly as before.
- **Seed 1234:** 24 viewports. Broken ceilings 62 (was 64), debris 61 (63), beacons 26 (28),
  steam pipes 46 (46).
- **Checks:** `buildLevel(maze, kit, false)` builds without them. `level_tests` checks that
  every model and emitter outside the viewport cells, phases included, is identical to that
  build; that each viewport is on the edge; and that its glass lies beyond the wall on the
  outward side.

**Rendering** (`shaders/trace.hlsl`):
- **Space:** a primary ray that leaves the ship sees `sky(dir)`.
  - Stars on a cube-face grid (300 cells a face, about 5 px at 1080p). One cell in nine has a
    star, each a soft ~1 px disc: mostly faint, 1.5% bright, tinted from orange to blue-white.
  - A faint galactic band and a dim violet nebula.
  - It is a pure function of direction, so captures repeat.
- **Glass**, which also improves the door windows:
  - A reflection ray, fully shaded (`shade()`, so the reflected corridor has its shadows and
    lighting), weighted by Fresnel with F0 = 0.09: two faces of about 4% each. At 4% the dim
    corridor barely showed over the stars.
  - Highlights from every light and the flashlight at roughness 0.015. At the shader's usual
    minimum of 0.06, the flashlight smeared into a blob about 6° wide.
  - A faint film of its own colour (alpha).
  - The rest transmitted, tinted by the glass's hue.
- **Window mode:** a pixel whose ray went through glass to space stores a point 500 m along the
  ray and a reserved id, so its history reprojects as if infinitely far and the stars don't
  smear. The reflection on top still reprojects like the glass surface (some ghosting while
  moving; not measured).

**The window film** (`--scene window`, `film_window`, 17.5 s, seed 1234, a dim section):
- **0-4 s:** you walk up a corridor along the edge, past a gallery of viewports.
- **4-10 s:** you stop at one and look out at the stars, with the corridor reflected over them.
- **10-13 s:** you turn to the next viewport, the flashlight sweeping across the glass and reveal.
  Looked at that steeply, it mirrors the opposite wall 4-6 m down the corridor, where the
  creature is coming in along the ceiling and down the wall.
- **13-17.5 s:** you turn to face it, about 3 m away on the wall, hissing.
- **Known flaw:** during that pan the creature, waiting on the ceiling far down the corridor, is
  briefly visible directly at the right edge of the frame before it shows in the reflection.
- **Other tests:** `window_smoke` (smoke) and `window_preview` (diag, 2 frames a second).
  `level_tests` checks that the scene finds a viewport and that the creature comes within 1-4 m.

**How many lights** (answering "why is there a light cap if it's ray traced?"):
- **What the shader does, per pixel, at the surface the camera sees:**
  - **Fixture lights:** it loops over every light in the 3 × 3 cells around the hit. Each one in
    range that isn't dead gets **one shadow ray** to a random point on it (that makes the soft
    shadows).
  - **Short-lived lights** (sparks, muzzle, bolts, impacts, creature glow): it loops over all of
    them, up to 32. Each within range gets another shadow ray.
  - **Then** one bounce ray and one glossy ray. Bounce hits are lit without shadow rays.
- **So per pixel is not free:** ray tracing costs per ray, and every light in range is one more
  ray for every pixel it reaches. That is about 2 million rays a frame at 1080p.
- **Measured** (`light_cost_N`, label `diag`; the same 15 s of the walk at 1080p, N extra
  lights in a ring 2 m round the camera):

| Extra lights in range | 0 | 4 | 8 | 16 | 32 |
|---|---|---|---|---|---|
| Median frame | 8.8 ms | 10.2 ms | 10.9 ms | 13.9 ms | 17.9 ms |

  About 0.3 ms a light; 32 lights double the frame. The fixtures already add 11 lit lights on
  average in the 3 × 3 cells round the camera (14 at most).
- **The caps:**
  - The renderer passes up to 32 short-lived lights, and the game keeps the 16 brightest.
  - Round 11 kept the creatures' glow lights to the three nearest.
  - Without a cap, ten creatures piled on the camera in the bench took the median from 7 to
    12 ms (15 ms in dark sections).
- **The fix** (round 15, proposed): sample lights instead of testing them all. For each pixel,
  pick one or two lights at random in proportion to how much each could add there (unshadowed),
  send shadow rays only to those, and divide by the chance of picking them. The temporal history
  (window) and the sub-frames (captures) average out the extra noise.
  - Then the cost is the same for 5 lights or 500, and the caps can go.
  - Reusing good picks from neighbouring pixels and the last frame (ReSTIR) keeps the noise down
    when many lights compete.
  - Fixtures and short-lived lights can share one pick.

Timing after this round (`bench_walk`, whole walk, 1080p): median 7.6 ms, 95th percentile
10.2 ms (7.2 / 9.7 ms last round; the same section varies by 0.5-1 ms run to run on this
laptop).

## The walking weave (fixed after round 10)

**The bug:** in the window, walking weaved from side to side.
- **Cause:** `Game::update` drew a sideways head sway (±1.2 cm) by adding it to the camera position. The next step then read the camera position back as where the player stood, so every step's sway was added into the real position.
- **Size:** over half a sway cycle that sums to roughly 0.012 m × fps × 0.03 s, so it grew with frame rate. A side-to-side swing of 0.11 m at 30 fps, 0.36 m at 100 fps, 0.52 m at 144 fps and 0.86 m at 240 fps, all to one side of the centre line.
- **Pressing against something:** when the player stopped against a low pipe with the key held, the bob phase froze and the same offset was added every step, sliding the player 0.9 m sideways into the wall.
- **Why the films missed it:** the scripted walk (`setWalkTime`) sets the position outright each frame, so they never showed this.

**The fix:** the sway is now only a view offset (`Game::sway_`). It is subtracted before each step and cleared wherever the camera is set directly. The easing of walking speed, bob strength and crouch also changed from `min(1, rate·dt)` to `1 − e^(−rate·dt)`, which gives the same result at any frame rate.

**Checks:**
- `--scene weave` (ctest `weave_smoke`, and `weave_capture`, label `weave`) drives the window's own `update` with the forward key held, at `--step-hz` (default 100) steps a second, down the longest straight corridor of seed 1234. It writes 30 fps frames, `weave.csv` (every step's sideways distance) and `weave_plot.png` (the path from above, sideways stretched).
- `captures/weave_before` is the old code (swing 0.36 m, then 0.89 m into the wall at the pipe). `captures/weave_after` is the fix (±0.012 m throughout).
- `level_tests` "walking straight" holds forward for 6 s at 30, 60, 100, 144 and 240 Hz. It requires staying within 2.5 cm of the line, and distances at 2 s that agree to 8 cm. It fails on the old code at every rate.

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
