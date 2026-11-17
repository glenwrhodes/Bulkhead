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

# The timing run: window-mode rendering over the whole start -> exit walk of the 40 x 40
# ship at 1080p, with frame times for each mood in captures/bench_walk/bench.log.
# Label "bench": run it on its own (ctest -L bench).
bulkhead_png_test(bench_walk bench
  GAME  --bench "${CAP}/bench_walk" --res 1920x1080 --seed 1234
  CHECK --file "${CAP}/bench_walk/frame_final.png" --width 1920 --height 1080)

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

# Fixed views with ray tracing on and off, for side-by-side comparison: corridor, door,
# room, junction, and one corridor in each mood (lit, dim, dark, derelict).
bulkhead_png_test(film_shots film
  GAME  --shots "${CAP}/shots" --res 1920x1080 --spp 32 --seed ${FILM_SEED} --no-hud
  CHECK --file "${CAP}/shots/shot_room_rt.png" --width 1920 --height 1080)
foreach(mood lit dim dark derelict)
  add_test(NAME film_shots_${mood}_check COMMAND capture_check
    --file "${CAP}/shots/shot_corridor_${mood}_rt.png" --width 1920 --height 1080)
  set_tests_properties(film_shots_${mood}_check PROPERTIES LABELS film FIXTURES_REQUIRED film_shots)
endforeach()

foreach(seed 7 42 2026)
  bulkhead_png_test(film_overhead_${seed} film
    GAME  --overhead "${CAP}/overhead_seed${seed}.png" --seed ${seed}
    CHECK --file "${CAP}/overhead_seed${seed}.png" --width ${MAP_PX} --height ${MAP_PX})
endforeach()
