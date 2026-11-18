# ctest suite. Labels:
#   unit  - pure C++ tests, no GPU (maze, kit loading, level, collision, walk, game)
#   smoke - runs the game headless (overhead map, short ray-traced walk, debug layer, shots)
#   film  - full-resolution captures for the video (the whole walk, RT on/off shots, maps)
# Run everything:  ctest --test-dir build -C Release --output-on-failure
# Quick check:     ctest --test-dir build -C Release -L "unit|smoke"

add_executable(maze_tests maze_tests.cpp)
target_link_libraries(maze_tests PRIVATE bulkhead_lib)
bulkhead_settings(maze_tests)
set_target_properties(maze_tests PROPERTIES FOLDER tests)

add_executable(level_tests level_tests.cpp)
target_link_libraries(level_tests PRIVATE bulkhead_lib)
bulkhead_settings(level_tests)
set_target_properties(level_tests PROPERTIES FOLDER tests)

add_executable(audio_tests audio_tests.cpp)
target_link_libraries(audio_tests PRIVATE bulkhead_lib)
bulkhead_settings(audio_tests)
set_target_properties(audio_tests PROPERTIES FOLDER tests)
add_test(NAME audio_tests COMMAND audio_tests "${CMAKE_SOURCE_DIR}/assets")
set_tests_properties(audio_tests PROPERTIES LABELS unit)

# The window's sound output (XAudio2) without the window. Label "diag" (needs a sound device).
add_test(NAME sound_device COMMAND Bulkhead --sound-check)
set_tests_properties(sound_device PROPERTIES LABELS diag)

add_executable(capture_check capture_check.cpp)
target_link_libraries(capture_check PRIVATE bulkhead_lib)
bulkhead_settings(capture_check)
set_target_properties(capture_check PROPERTIES FOLDER tests)

add_test(NAME maze_tests COMMAND maze_tests)
add_test(NAME level_tests COMMAND level_tests "${CMAKE_SOURCE_DIR}/assets")
set_tests_properties(maze_tests level_tests PROPERTIES LABELS unit)

set(CAP "${BULKHEAD_CAPTURE_DIR}")

# bulkhead_png_test(<name> <label> CHECK <check args...> GAME <game args...>)
# Runs the game, then capture_check on what it wrote (fixture ordering).
function(bulkhead_png_test name label)
  cmake_parse_arguments(ARG "" "" "CHECK;GAME" ${ARGN})
  add_test(NAME ${name}_run COMMAND Bulkhead ${ARG_GAME})
  add_test(NAME ${name}_check COMMAND capture_check ${ARG_CHECK})
  set_tests_properties(${name}_run PROPERTIES LABELS ${label} FIXTURES_SETUP ${name} RUN_SERIAL TRUE)
  set_tests_properties(${name}_check PROPERTIES LABELS ${label} FIXTURES_REQUIRED ${name})
endfunction()

# The ship is 40 x 40 cells; the overhead PNG is 64 px a cell plus a 32 px margin.
set(MAP_PX 2624)

# --- smoke ------------------------------------------------------------------
bulkhead_png_test(overhead_map smoke
  GAME  --overhead "${CAP}/overhead_seed1234.png" --seed 1234
  CHECK --file "${CAP}/overhead_seed1234.png" --width ${MAP_PX} --height ${MAP_PX})

bulkhead_png_test(capture_smoke smoke
  GAME  --capture "${CAP}/smoke" --frames 24 --res 640x360 --spp 2 --start 1.5 --seed 1234
  CHECK --dir "${CAP}/smoke" --count 24 --width 640 --height 360 --expect-motion)

# The D3D12 debug layer breaks on validation errors, so API misuse crashes this run.
bulkhead_png_test(capture_debug_layer smoke
  GAME  --capture "${CAP}/debug_layer" --frames 3 --res 320x180 --spp 2 --start 2 --seed 7 --debug
  CHECK --dir "${CAP}/debug_layer" --count 3 --width 320 --height 180)

# Firing: standing in a dark corridor, six shots (muzzle flash, bolts, impacts, scorch marks).
bulkhead_png_test(fire_smoke smoke
  GAME  --capture "${CAP}/fire_smoke" --scene fire --frames 24 --start 0.95 --res 640x360 --spp 2 --seed 1234
  CHECK --dir "${CAP}/fire_smoke" --count 24 --width 640 --height 360)

# The creature: up the corridor on the floor, onto the wall, over the ceiling.
bulkhead_png_test(crawl_smoke smoke
  GAME  --capture "${CAP}/crawl_smoke" --scene crawl --frames 24 --start 6 --res 640x360 --spp 2 --seed 42
  CHECK --dir "${CAP}/crawl_smoke" --count 24 --width 640 --height 360 --expect-motion)

# Walking with the forward key held (the window's own update code at 100 steps a second)
# down a straight corridor: the player must not weave. weave.csv logs every step's
# sideways distance from the centre line; weave_plot.png is the path from above.
bulkhead_png_test(weave_smoke smoke
  GAME  --capture "${CAP}/weave_smoke" --scene weave --frames 60 --res 640x360 --spp 2 --seed 1234
  CHECK --dir "${CAP}/weave_smoke" --count 60 --width 640 --height 360 --expect-motion)

# The same at 1080p, 30 fps, for the video. Label "weave": run it on its own.
# captures/weave_before was this capture on the code before the fix (the weave and the
# slide into the wall); captures/weave_after is the fixed walk.
bulkhead_png_test(weave_capture weave
  GAME  --capture "${CAP}/weave_after" --scene weave --res 1920x1080 --fps 30 --spp 12 --seed 1234
  CHECK --dir "${CAP}/weave_after" --at-least 120 --width 1920 --height 1080 --expect-motion)

# Hunters: the hunt (it kills you) and the fight (you kill it), short and small.
bulkhead_png_test(hunt_smoke smoke
  GAME  --capture "${CAP}/hunt_smoke" --scene hunt --res 480x270 --fps 15 --spp 2 --seed 1234
  CHECK --dir "${CAP}/hunt_smoke" --at-least 100 --width 480 --height 270 --expect-motion)
bulkhead_png_test(fight_smoke smoke
  GAME  --capture "${CAP}/fight_smoke" --scene fight --res 480x270 --fps 15 --spp 2 --seed 1234
  CHECK --dir "${CAP}/fight_smoke" --at-least 100 --width 480 --height 270 --expect-motion)

# Viewports (part 3): stars through the glass, the creature passing behind you in it.
bulkhead_png_test(window_smoke smoke
  GAME  --capture "${CAP}/window_smoke" --scene window --frames 24 --start 9 --res 640x360 --spp 2 --seed 1234
  CHECK --dir "${CAP}/window_smoke" --count 24 --width 640 --height 360 --expect-motion)

# The whole window scene at 2 frames a second, for looking it over. Label "diag".
add_test(NAME window_preview COMMAND Bulkhead --capture "${CAP}/window_preview" --scene window --fps 2 --res 960x540
         --spp 12 --seed 1234)
set_tests_properties(window_preview PROPERTIES LABELS diag RUN_SERIAL TRUE)

bulkhead_png_test(shots_smoke smoke
  GAME  --shots "${CAP}/shots_smoke" --res 640x360 --spp 4 --seed 1234
  CHECK --file "${CAP}/shots_smoke/shot_door_rt.png" --width 640 --height 360)

# Window-mode rendering (1 spp + temporal accumulation) at 1080p, timed. Frame times
# are in captures/bench/bench.log; frame_0120.png etc. show the noise level in motion.
bulkhead_png_test(bench smoke
  GAME  --bench "${CAP}/bench" --res 1920x1080 --frames 480 --start 1.0 --seed 1234
  CHECK --file "${CAP}/bench/frame_0480.png" --width 1920 --height 1080)

# Diagnostic: camera standing still (past the end of the walk), so temporal accumulation
# should converge completely. Label "diag": not part of the normal runs.
add_test(NAME bench_static COMMAND Bulkhead --bench "${CAP}/bench_static" --res 1280x720 --frames 240 --start 1000 --seed 1234)
set_tests_properties(bench_static PROPERTIES LABELS diag)

# What each short-lived light costs: the same 15 s of the walk at 1080p with 0..32 extra
# lights in a ring 2 m round the camera (most pixels within their range). Label "diag".
foreach(n 0 4 8 16 32)
  add_test(NAME light_cost_${n} COMMAND Bulkhead --bench "${CAP}/light_cost_${n}" --res 1920x1080 --start 60
           --frames 900 --seed 1234 --test-lights ${n})
  set_tests_properties(light_cost_${n} PROPERTIES LABELS diag RUN_SERIAL TRUE)
endforeach()

# The timing run: window-mode rendering over the whole start -> exit walk of the 40 x 40
# ship at 1080p, with frame times for each mood in captures/bench_walk/bench.log.
# Label "bench": run it on its own (ctest -L bench).
bulkhead_png_test(bench_walk bench
  GAME  --bench "${CAP}/bench_walk" --res 1920x1080 --seed 1234
  CHECK --file "${CAP}/bench_walk/frame_final.png" --width 1920 --height 1080)

# --- encounters (round 11, no GPU) --------------------------------------------
# How long a simulated player exploring the ship takes to first see a creature, with the
# creature numbers before round 11 ("original": three, no drift, no hearing) and after.
# Each writes encounters.log (summary at the end) and encounters.csv. Label "encounters".
add_test(NAME encounters_smoke COMMAND Bulkhead --encounters "${CAP}/encounters_smoke" --runs 2x1 --minutes 2)
set_tests_properties(encounters_smoke PROPERTIES LABELS unit)
add_test(NAME encounters_original COMMAND Bulkhead --encounters "${CAP}/encounters_original" --hunt original)
add_test(NAME encounters_new COMMAND Bulkhead --encounters "${CAP}/encounters_new")
set_tests_properties(encounters_original encounters_new PROPERTIES LABELS encounters RUN_SERIAL TRUE)
# How the round 11 numbers were chosen: a grid of counts, drift timings and hearing ranges
# (10 ships x 2 runs of 6 minutes each). Label "diag".
add_test(NAME encounters_sweep COMMAND Bulkhead --encounters "${CAP}/encounters_sweep" --sweep --runs 10x2 --minutes 6)
set_tests_properties(encounters_sweep PROPERTIES LABELS diag RUN_SERIAL TRUE)

# The timing run with creatures loose, hunting the walk (they can't grab it, so they stay
# close): the original three, and the round 11 number. Label "bench".
bulkhead_png_test(bench_creatures_original bench
  GAME  --bench "${CAP}/bench_creatures_original" --res 1920x1080 --seed 1234 --hunt original --creatures 3
  CHECK --file "${CAP}/bench_creatures_original/frame_final.png" --width 1920 --height 1080)
bulkhead_png_test(bench_creatures bench
  GAME  --bench "${CAP}/bench_creatures" --res 1920x1080 --seed 1234 --creatures 10
  CHECK --file "${CAP}/bench_creatures/frame_final.png" --width 1920 --height 1080)

# --- package ----------------------------------------------------------------
# The download: build the zip, unpack it into an empty folder, and run the game from
# there (help, --check, and a short ray-traced capture). GitHub's machines run the same
# check without the capture (no ray-tracing GPU there).
add_test(NAME package_zip COMMAND "${CMAKE_COMMAND}" "-DBUILD=${CMAKE_BINARY_DIR}" "-DOUT=${CMAKE_BINARY_DIR}/dist"
         "-DCONFIG=$<CONFIG>" -P "${CMAKE_SOURCE_DIR}/cmake/PackageZip.cmake")
add_test(NAME package_check COMMAND "${CMAKE_COMMAND}" "-DZIP=${CMAKE_BINARY_DIR}/dist/Bulkhead-windows-x64.zip"
         "-DWORK=${CMAKE_BINARY_DIR}/package_check" -DGPU=ON -P "${CMAKE_SOURCE_DIR}/cmake/CheckPackage.cmake")
set_tests_properties(package_zip PROPERTIES LABELS package FIXTURES_SETUP package RUN_SERIAL TRUE)
set_tests_properties(package_check PROPERTIES LABELS package FIXTURES_REQUIRED package RUN_SERIAL TRUE)

# --- film -------------------------------------------------------------------
# The walk from start to exit, 1080p at 30 fps, in two halves (one ctest run is cut off
# after 10 minutes; each half takes about 6). Seed 1234 passes lit, dim, lit, derelict
# and dark sections (see "Walk moods" in capture.log).
set(FILM_SEED 1234)
bulkhead_png_test(film_walkthrough_a film
  GAME  --capture "${CAP}/walkthrough_a" --res 1920x1080 --fps 30 --spp 12 --frames 3000 --seed ${FILM_SEED}
  CHECK --dir "${CAP}/walkthrough_a" --count 3000 --width 1920 --height 1080 --expect-motion)
bulkhead_png_test(film_walkthrough_b film
  GAME  --capture "${CAP}/walkthrough_b" --res 1920x1080 --fps 30 --spp 12 --start 100 --seed ${FILM_SEED}
  CHECK --dir "${CAP}/walkthrough_b" --at-least 2900 --width 1920 --height 1080 --expect-motion)
bulkhead_png_test(film_overhead_walk film
  GAME  --overhead "${CAP}/walkthrough_overhead_seed${FILM_SEED}.png" --seed ${FILM_SEED}
  CHECK --file "${CAP}/walkthrough_overhead_seed${FILM_SEED}.png" --width ${MAP_PX} --height ${MAP_PX})

# The gun: arms holding it, six shots in a dark corridor, 1080p at 30 fps (6 s).
bulkhead_png_test(film_fire film
  GAME  --capture "${CAP}/fire" --scene fire --res 1920x1080 --fps 30 --spp 16 --seed ${FILM_SEED}
  CHECK --dir "${CAP}/fire" --count 180 --width 1920 --height 1080 --expect-motion)

# The creature comes in from beyond the far end of a long corridor, crawls toward you on
# the floor, climbs the wall, stops to look at you, and crosses the ceiling overhead.
# Seed 42: its best corridor for this is in a dim section (seed 1234's is fully lit).
bulkhead_png_test(film_crawl film
  GAME  --capture "${CAP}/crawl" --scene crawl --res 1920x1080 --fps 30 --spp 16 --seed 42
  CHECK --dir "${CAP}/crawl" --at-least 300 --width 1920 --height 1080 --expect-motion)

# The hunt: a dark corridor, the flashlight; it stalks you on the ceiling, you back off and
# hit it twice, it charges, leaps, and you die. The AI plays it out at fixed steps.
bulkhead_png_test(film_hunt film
  GAME  --capture "${CAP}/hunt" --scene hunt --res 1920x1080 --fps 30 --spp 16 --seed ${FILM_SEED}
  CHECK --dir "${CAP}/hunt" --at-least 300 --width 1920 --height 1080 --expect-motion)
# The fight: one charges you; four hits and it dies on its back.
bulkhead_png_test(film_fight film
  GAME  --capture "${CAP}/fight" --scene fight --res 1920x1080 --fps 30 --spp 16 --seed ${FILM_SEED}
  CHECK --dir "${CAP}/fight" --at-least 250 --width 1920 --height 1080 --expect-motion)

# Viewports: walk up to one on the ship's edge, the stars outside, the corridor and the
# creature (crawling in behind you) reflected in the glass, the flashlight sweeping across
# it, then turn round to face the creature. 1080p at 30 fps, 17 s.
bulkhead_png_test(film_window film
  GAME  --capture "${CAP}/window" --scene window --res 1920x1080 --fps 30 --spp 16 --seed ${FILM_SEED}
  CHECK --dir "${CAP}/window" --at-least 480 --width 1920 --height 1080 --expect-motion)

# The approach (round 13): you stand still, one that has spotted you closes in and leaps.
# film_approach is the current behaviour. approach_original replays the behaviour before
# round 13 (--hunt original), the sprint along the floor Glen saw; captures/approach_before
# was made from the code before round 13 and is kept as it is. Label "diag".
#   approach_after     the same scene, seed and start as approach_before, current code
#   approach_original  the same with --hunt original (the old approach, replayed)
#   film_approach      a second example, seed 42 (another ship, another corridor)
bulkhead_png_test(film_approach film
  GAME  --capture "${CAP}/approach" --scene approach --res 1920x1080 --fps 30 --spp 16 --seed 42
  CHECK --dir "${CAP}/approach" --at-least 200 --width 1920 --height 1080 --expect-motion)
# The climb (round 15): the crawl as a showcase, floor, wall, the wall-ceiling edge,
# ceiling, wall, floor, from close setups; and the same frames with the system drawn on
# them (legs' bones, footholds, step arcs, the surface and its up, the gait), to cut together.
bulkhead_png_test(film_climb film
  GAME  --capture "${CAP}/climb" --scene climb --res 1920x1080 --fps 30 --spp 16 --seed ${FILM_SEED} --no-hud
  CHECK --dir "${CAP}/climb" --at-least 500 --width 1920 --height 1080 --expect-motion)
bulkhead_png_test(film_climb_overlay film
  GAME  --capture "${CAP}/climb_overlay" --scene climb --overlay --res 1920x1080 --fps 30 --spp 16 --seed ${FILM_SEED} --no-hud
  CHECK --dir "${CAP}/climb_overlay" --at-least 500 --width 1920 --height 1080 --expect-motion)
# Both at 2 frames a second, small, for looking them over. Label "diag".
add_test(NAME climb_preview COMMAND Bulkhead --capture "${CAP}/climb_preview" --scene climb --overlay --fps 2
         --res 960x540 --spp 8 --seed ${FILM_SEED} --no-hud)
set_tests_properties(climb_preview PROPERTIES LABELS diag RUN_SERIAL TRUE)

# The threat filmed where it reads: a lit corridor without sparks, the creature on the
# ceiling, its head kept in the middle of the frame, through the coil and the leap.
bulkhead_png_test(film_threat film
  GAME  --capture "${CAP}/threat" --scene threat --res 1920x1080 --fps 30 --spp 16 --seed ${FILM_SEED}
  CHECK --dir "${CAP}/threat" --at-least 150 --width 1920 --height 1080 --expect-motion)
add_test(NAME approach_after COMMAND Bulkhead --capture "${CAP}/approach_after" --scene approach --res 1920x1080
         --fps 30 --spp 16 --seed ${FILM_SEED})
add_test(NAME approach_original COMMAND Bulkhead --capture "${CAP}/approach_original" --scene approach --hunt original
         --res 1920x1080 --fps 30 --spp 16 --seed ${FILM_SEED})
set_tests_properties(approach_after approach_original PROPERTIES LABELS diag RUN_SERIAL TRUE)

# Fixed views with ray tracing on and off, for side-by-side comparison: corridor, door,
# room, junction, one corridor in each mood (lit, dim, dark, derelict), and the derelict
# dressing (broken ceiling, steam, low pipe, beacon).
bulkhead_png_test(film_shots film
  GAME  --shots "${CAP}/shots" --res 1920x1080 --spp 32 --seed ${FILM_SEED} --no-hud
  CHECK --file "${CAP}/shots/shot_room_rt.png" --width 1920 --height 1080)
foreach(shot corridor_lit corridor_dim corridor_dark corridor_derelict broken_ceiling steam low_pipe beacon)
  add_test(NAME film_shots_${shot}_check COMMAND capture_check
    --file "${CAP}/shots/shot_${shot}_rt.png" --width 1920 --height 1080)
  set_tests_properties(film_shots_${shot}_check PROPERTIES LABELS film FIXTURES_REQUIRED film_shots)
endforeach()

foreach(seed 7 42 2026)
  bulkhead_png_test(film_overhead_${seed} film
    GAME  --overhead "${CAP}/overhead_seed${seed}.png" --seed ${seed}
    CHECK --file "${CAP}/overhead_seed${seed}.png" --width ${MAP_PX} --height ${MAP_PX})
endforeach()
