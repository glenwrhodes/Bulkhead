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
| 14 | Round 13: sound (mixer, window output, WAV beside every capture, ~25 sounds, credits) | **Done (round 13)**: see Sound |
| 15 | Round 14: the approach (crawl in on walls and ceiling, stop, hiss, crouch, leap from any surface), turning round without a snap | **Done (round 14)**: see The approach |
| 16 | Round 15: lights without a cap: sample a few lights per pixel by importance instead of shadow-testing all of them (see Viewports, "How many lights") | Proposed |
| 17 | Round 16: the threat filmed close; the climbing film, plain and with the crawl overlay (see "The climb") | Done |
| 18 | Round 17: the motion scanner (blips on the minimap and map), the scanner film; the legs 20% slower | Done (see "The motion scanner") |
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

## Sound (round 13)

**The sounds** (`assets/sounds`, 23 MP3s, about 1 MB). Each was generated with ElevenLabs Sound
Effects (v3) through GlenTools from a prompt written for it. `assets/sounds/CREDITS.md` lists
every file with what it's for, its prompt, its length, its request id and the licence note
(made on a paid ElevenLabs plan, confirmed by Glen: commercial use, games and YouTube allowed).
Swapping one means dropping in a file with the same name.

**The mixer** (`src/audio/Mixer.*`, no Windows; the same for the window and the captures):
- **Clips:** `SoundBank` decodes the MP3s (dr_mp3) or WAVs (dr_wav) to 44.1 kHz float.
- **Voices:** one-shots (`SoundEvent`), and loops keyed by the game (`SoundLoop`). A new key
  fades in over 0.25 s, a missing one fades out. At most 64 voices; the oldest one-shot goes.
- **Placing a sound round the listener:**
  - **Distance:** 1 / (1 + (d - 1 m) / 3 m), so half at 4 m, a quarter at 10 m, a seventh at
    20 m, gone past 50 m. That's gentler than real life, so a hiss down the corridor is heard.
  - **Left/right:** equal-power panning (80% wide).
  - **Muffling:** a one-pole low-pass from 18 kHz down toward 6.5 kHz behind you, and 900 Hz
    behind walls (with the level down 45%). The game finds the walls with `raycastLevel`.
- **Room and output:**
  - A small metallic room: a Schroeder reverb (Freeverb's 4 comb + 2 all-pass lengths per
    side). Far sounds send more to it.
  - Master level 0.85 and a soft knee above 0.9. The peak before it and the number of samples
    it bent are logged.

**What the game makes sound** (`Game::collectSounds`, called once a frame after the step, so
it works the same in the window and every scripted film):
- **Pushed as they happen:**
  - The gun.
  - A bolt's hit: metal, or the creature, plus its pain cry if it lives.
- **Found by comparing with the last frame:**
  - **Footsteps:** one per head bob (window and walk), or one per 0.8 m in scenes that place
    the camera themselves.
  - **Claws:** each creature foot going from lifted to planted (`Creature::planted`), so the
    clicks follow the gait.
  - **Hunter state changes:** stalking starts (hiss), leap (screech), dying (death cry).
  - **The scripted crawl creature:** its jaws opening wide (`Creature::jaw()`, hiss).
  - **Doors** starting to open or close (sliding, or heavy for bulkhead and airlock).
  - **Spark bursts** within 16 m (the same slots the effects use).
  - **Being caught:** the slam.
  - **Now and then off in the ship:** a groan or a knock, every 23 s slot with a chance of
    70%, placed 10 m away and muffled.
- **Loops:** the ship's hum, each venting steam jet, a creature charging (skitter), and being
  fed on.

**The window** (`src/audio/AudioDevice.*`, XAudio2, delay-loaded):
- Three 1024-frame buffers stay queued (~70 ms in total). XAudio2's thread mixes the next one
  under the lock the game holds while it hands over the frame's sounds.
- With no device, or no `xaudio2_9.dll`, it logs why and stays silent.
- `--sound-check` (ctest `sound_device`, label `diag`) opens the device and plays two sounds.
  Here it played 1.28 s.

**Captures** (`src/game/AudioRecorder.*`): every `--capture` except the weave writes, beside its
frames:
- `audio.wav` (44.1 kHz 16-bit stereo; each frame gets exactly 44100 / fps samples, 1470 at
  30 fps, so it is as long as the frames to the sample)
- `audio_events.csv` (every one-shot with frame, time, position, distance, gain and occlusion,
  plus loops starting and stopping)
- `audio_wave.png` (left above right, a marker lane coloured by group: cyan player, yellow
  gun, orange creature, green world, blue ambience; a line every second; the limiter's knee in red)

**Checked:**
- `audio_tests` (unit): all 23 decode and none is silent. A sound starts on the first sample
  of the frame after `play()`. A source 3 m to the right is 6.5× louder on the right. 10 m is
  1/3.0 of 2 m. A wall takes it to 37%. A dropped loop is gone half a second later.
- The hunt and fight scenes make their sounds: claws, hiss, gunfire and hits; a screech
  before the slam and feeding in the hunt; one death cry in the fight.
- Re-rendered with sound: `film_fight` (11.57 s, peak 0.86, nothing limited) and `film_window`
  (17.50 s = 525 frames × 1470 samples, peak 0.49: the door, four footsteps, claws closing in from 6.5 to 3.5 m, the hiss at 2.8 m at
  14.5 s).
- The sound itself has not been listened to by anyone on the AI side.

## The approach (round 14)

Glen: "It's so much cooler if we could see the alien, crawling toward you, hissing and
opening its mouth, leaning down, before leaping at you."

**Before** (`captures/approach_before`, made from the code before this round and kept as it
is). In `--scene approach` you stand still and don't shoot; seed 1234, a derelict corridor:
- **0.5-5 s:** it stalks, edging in along the ceiling from 18 m to 13 m.
- **5 s:** it charges at 4 m/s and drops to the floor for the last three cells.
- **7.5 s:** it leaps from 3.3 m.
- **7.9 s:** it has you.
- **What you saw:** a floor sprint and a leap (frame 218 is the sprint, 230 the leap).

**Now** (`HuntTuning::approach`, on in the current tuning and off with `--hunt original`):
- **Closing in:**
  - Stalking and charging, it keeps to the ceiling and walls. In straight corridors each cell
    gets the ceiling (45%), the right wall or the left wall, from a hash of the cell, so a
    re-route doesn't reshuffle the cells ahead. Rooms and doorways are still floor.
  - It charges at 3 m/s (was 4).
- **Threat (1.5 s):** within 5 m with a clear line to you, it stops (3.2 m since round 16; see
  "The threat, filmed").
  - It rears: thorax, neck and head pivot up to 40° about the waist (49° since round 16), away
    from the surface it is on. On the ceiling that's leaning down at you; on a wall, swinging out into the corridor.
  - Its head turns to you, its jaws open wide and quiver, and its scythes rise.
  - Its seams and throat flare, lighting the surface it's on.
  - A deeper, closer hiss than the one it gives on spotting you.
  - If you back off past 7.5 m it chases again. A hit makes it coil at once.
- **Coil (0.6 s):** it sinks toward the surface, 42% lower with the head lowest. Its knees
  bend through the IK, and its claws stay planted (0.000 m of slide in the test).
- **Leap:**
  - **Distance:** from whatever it's on, if you're within 7 m (else it closes in again).
  - **Flight:** it takes longer the farther it goes (distance / 8 m/s, 0.42-0.8 s). Off a
    wall or the ceiling it drops at you with little arc and twists upright on the way.
  - **After a miss:** it lands and charges, and for 4 s leaps close without threatening.
- **Pose controls:** `CreaturePose` gained `rear`, `crouch` and `arms`, eased in and out by
  the hunter.

**Turning round no longer snaps** (the 0.7 m snap from round 11, and two more found here):
- **The cause:** a re-route that turns at the cell it stands in makes a rounded corner there,
  up to half a metre from its line.
- **The fix:** instead, the new path starts with one virtual cell, a wall's if need be, so the
  body lies straight on it. The creature is placed so its middle stays put (found by projecting
  onto the new path) and swings onto the new direction about its middle. Half a turn takes
  0.8 s, less for less.
- **Turning round:** the virtual cell is the one ahead. It turns, goes back to the centre and
  takes the new way.
- **Already past the centre with the way on to one side:** the same.
- **At the centre with the way on to one side:** the virtual cell is behind it on the new way;
  it turns on the spot.
- **Hunting re-routes** (the alert, stalk to chase, threat or coil back to chase, each cell
  centre) now go through `Hunter::reaim`. That is `routeAhead`, anchored at the first cell
  centre at least 3.4 m ahead, or a turn if you're behind it.
  - The alert had been snapping it 1.2 m sideways: it re-routed mid-cell in the first cell of
    its path.
- **Checks:**
  - `level_tests`: ten creatures for 150 s, the largest step while turning is 0.01 m (was 0.7).
  - The approach scene: no step above 0.4 m before the leap, under either tuning.

**After** (`captures/approach_after`, same scene, seed and start):

| | Before | After |
|---|---|---|
| Stalking | 0.5 s | 0.5 s |
| Charge | 5.0 s, finishing on the floor | 5.0 s, over the walls and ceiling at 3 m/s |
| Threat | none | 8.1-9.6 s, at 5.0 m, on the right wall |
| Coil | none | 9.6-10.2 s |
| Leap | 7.5 s, from 3.3 m off the floor | 10.2 s, from 5.1 m off the wall |
| It has you | 7.9 s | 10.9 s |

- **The frames:** 230 is coming in high along the wall, 262 the threat (reared off the wall,
  the pale open jaws turned to you, behind a cable's sparks), 300 the coil and 318 the leap
  (jaws wide, scythes up).
- `film_approach` is a second example on seed 42, in a cleaner dim corridor: threat at
  5.1 m, leap from 5.5 m. Frame 322, the leap straight at the camera, is the strongest frame.
- The hunt film now ends threat (from 2.9 m) → coil → leap → feed. In the fight film the
  creature dies on the wall at 5.8 m, before it can threaten.
- **Weak spot:** a threat on a wall reads less clearly than one on the floor or ceiling,
  because rearing swings it sideways rather than up.

**Encounters with the approach** (`encounters_new`, 30 runs of 10 min):
- First seen 71 s on average (median 58 s), still in the 45-75 s target.
- First death 87 s (was 67).
- Deaths 6.5 per 10 min (was 9.0).
- Creatures in view 22 s per 10 min (was 10.8).
- Killed before seeing one: 1 of 30 runs.

## The threat, filmed (round 16)

At 5 m the head was about 40 px tall on a 1080p screen, and in `approach_after` a sparking
cable hung between it and the camera. What changed, and why:
- `kThreatRange` 5 → 3.2 m: the head is about three times bigger on screen. It still threatens
  before `kLeapRange` (3.3 m), because `Hunter` now only leaps straight from a chase if the
  approach is off, it threatened in the last 4 s, or you are within 2.2 m.
- Rear 0.7 → 0.85 rad (40° → 49°), and the threat jaw opens to 1.05 (with a quiver), so the
  pale inside of the jaws faces the camera.
- `--scene threat` (`film_threat`, seed 1234): a lit corridor with no sparks, and
  `Hunter::setHuntSurface(2)` keeps it on the ceiling. Threat 8.2 s at 3.1 m (frame 265:
  reared down off the ceiling, jaws open at the camera), coil 9.7 s (frame 300), leap 10.3 s
  (frame 316).
- Encounters afterwards (`encounters_new`): first seen 74 s mean, 58 s median (was 71 / 58),
  first death 81 s (was 87), 6.2 deaths per 10 min (was 6.5), 21.7 s in view, killed before
  seeing one in 2 of 30 runs (was 1). `film_approach` re-rendered: threat at 3.2 m on the
  left wall, coil 10.2 s, leap 10.8 s.

## The climb (round 16)

`--scene climb` (`film_climb`) and the same with `--overlay` (`film_climb_overlay`): same
seed, same timing, frame for frame, so the two cut together.
- **The corridor:** the best straight run of 5+ cells, scored for length, lit (+60) or dim
  (+30), and against low pipes, broken and window pieces, clutter and spark emitters within a
  cell. On seed 1234 it is 20 m of lit corridor, and on the way down the left wall it crawls past a viewport.
- **The route** (u round the cross-section as a function of s, eased between keys): floor,
  up the right wall, the wall, the edge into the ceiling (slowed from 0.75 to 0.32 m/s),
  ceiling, down the left wall, the floor. 27.5 s.
- **Camera:** seven setups, cut on which stage the thorax is in: low ahead, on the feet at
  the wall's foot, across from it on the wall, a slow pass under the edge (2.4 m of dolly),
  under it on the ceiling, across from it on the far wall, low again. Flashlight off; no
  gun, no HUD, no hunters.
- **The overlay** (`src/game/CrawlOverlay.*`): drawn on the rendered frame on the CPU,
  in Segoe UI through stb_truetype. The surface strip under it and its normal ("up"), each
  leg's femur and tibia in its colour, next-foothold rings lying flat on their surface, the
  dashed step arc of each swinging leg, labels kept clear of each other, the stage caption and
  a step-cycle chart (planted | swinging, split at `kDuty`). `Creature::setKeepLegs` makes
  `pose` keep each leg's solved hip, knee, claw and footholds (`Creature::legs()`).
- **Tail fix found here:** each tail segment sat at its own s, so where the route also moves
  round the cross-section (onto a wall) the segments pulled apart and floated. They are now
  chained at their rest spacing from the pelvis back. This affects every creature film a
  little; `film_threat` and both climb films were rendered after it, `film_approach`, hunt and
  fight before.

## Slower legs (round 17)

Glen: the legs move "a little fast... about 20% slower". The legs cycle once per `kStride`
of travel, so there were two ways: the same stride with the creature 20% slower, or a 25%
longer stride at the same speed. **The stride is longer (0.55 → 0.6875 m), the speeds are
unchanged.** Slowing the creatures would have changed what was measured and tuned (the
encounter timer, the charge, the leap), and Glen liked how it hunts; he only asked about the
legs. Claws still land and stay put; each lands 25% farther ahead, and the claw sounds come
20% less often.
- The longer stride first failed the reach check: on the floor-to-wall bevel a hind claw
  was 0.20 m out of reach. When no sideways placement fits, `foothold` used to fall back to
  the last one it tried (1.9 × out); it now keeps the one that misses least, and also tries
  up to 0.16 m along the path. Worst miss in `level_tests`: 0.000 m, slide 0.000 m. Where a
  placement fitted before, the same one is chosen.
- Encounters: first seen 74.3 s mean / 57.8 s median, first death 81.1 s, identical (head bob
  follows the stride, which shifts later events a hair: 6.1 deaths per 10 min, was 6.2).
- Re-rendered: both climb films. Not re-rendered: hunt, fight, approach, threat, window.

## The motion scanner (round 17)

Glen: "the aliens to show up as a blip on the map... the general direction of an enemy, but
not a specific location... blobs indicating there was 'something coming'." `src/game/Scanner.*`
(no D3D12; drawn in `present.hlsl` on the minimap and the M map).
- **The pulse:** every 2.4 s with nothing about, a pulse goes out from you; its front (a bright
  green ring with a fading wash behind it) sweeps to 25 m in 1 s. A faint ring marks 25 m.
- **Blips:** anything moving faster than 0.2 m/s that the front passes, through walls, leaves
  a soft green blob. It is put up to 0.8 m + 7% of the distance away from the creature (a fresh
  random offset each pulse) and is 2 m + 8% of the distance across, so at 20 m you know a
  direction and a rough distance, not a cell. It flashes as the front finds it and fades over
  ~2 s, so a creature leaves a short trail of fading blobs that shows which way it is going.
  Two close together run into one blob. Standing still (a threat, a coil, the dead): nothing.
- **The ping:** one per pulse (`scanner_ping`, not positional). With something found, the next
  pulse comes sooner (2.0 s at 25 m down to 0.9 s at 5 m), louder (0.30 → 0.75) and higher
  (pitch up to 1.45).
- **When:** whenever creatures are loose (the window with `--creatures`, and the hunt, fight,
  approach, threat and scanner scenes). Walks and other captures have no creatures, so no
  scanner. It doesn't change the AI: the encounter numbers are identical.
- **`--scene scanner`** (`film_scanner`, seed 1234, 18.6 s): you hold a junction (cell 29, 29,
  lit), facing down its longest straight way. Three creatures start about 40 m of walking
  away, one down each way, and roam toward you (`Hunter::headFor`, drift pace). Blips at the
  edge of range from 6 s, from three directions; the one ahead shows at the far end of the
  corridor at 10.8 s (20 m, on the ceiling), hisses, stalks, charges at 15.5 s; the scene ends
  a second after it is inside 9 m. A second one, from the side, has also seen you and is
  charging off screen. 10 pings, 2.4 s apart at first, 1.56 s at the end, louder each time.
- **Checks:** `level_tests` "the motion scanner": a moving contact at 10 m leaves blips 0-1.6 m
  off (mean 1.2 m), never one near a still contact or one at 30 m; the first ping is the empty
  one, later ones higher and louder and closer together; the scene shows one in view between 5
  and 20 s, with blips from at least two directions, and you alive.

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
