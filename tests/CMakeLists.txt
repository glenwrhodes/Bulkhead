# ctest suite. Labels:
#   unit  - pure C++ tests, no GPU
#   smoke - runs the game headless (overhead map, short GPU capture) and checks the PNGs
#   film  - longer, full-resolution captures for the video
# Run everything:  ctest --test-dir build -C Release --output-on-failure
# Quick check:     ctest --test-dir build -C Release -L "unit|smoke"

add_executable(maze_tests maze_tests.cpp)
target_link_libraries(maze_tests PRIVATE bulkhead_lib)
bulkhead_settings(maze_tests)
set_target_properties(maze_tests PROPERTIES FOLDER tests)

add_executable(capture_check capture_check.cpp)
target_link_libraries(capture_check PRIVATE bulkhead_lib)
bulkhead_settings(capture_check)
set_target_properties(capture_check PROPERTIES FOLDER tests)

add_test(NAME maze_tests COMMAND maze_tests)
set_tests_properties(maze_tests PROPERTIES LABELS unit)

set(CAP "${BULKHEAD_CAPTURE_DIR}")

# bulkhead_png_test(<name> <label> <check args...> GAME <game args...>)
# Runs the game, then capture_check on what it wrote (fixture ordering).
function(bulkhead_png_test name label)
  cmake_parse_arguments(ARG "" "" "CHECK;GAME" ${ARGN})
  add_test(NAME ${name}_run COMMAND Bulkhead ${ARG_GAME})
  add_test(NAME ${name}_check COMMAND capture_check ${ARG_CHECK})
  set_tests_properties(${name}_run PROPERTIES LABELS ${label} FIXTURES_SETUP ${name} RUN_SERIAL TRUE)
  set_tests_properties(${name}_check PROPERTIES LABELS ${label} FIXTURES_REQUIRED ${name})
endfunction()

# --- smoke ------------------------------------------------------------------
bulkhead_png_test(overhead_map smoke
  GAME  --overhead "${CAP}/overhead_seed1234.png" --seed 1234
  CHECK --file "${CAP}/overhead_seed1234.png" --width 832 --height 832)

bulkhead_png_test(capture_smoke smoke
  GAME  --capture "${CAP}/smoke" --frames 60 --res 1280x720 --seed 1234
  CHECK --dir "${CAP}/smoke" --count 60 --width 1280 --height 720 --expect-motion)

# Same path with the D3D12 debug layer on. Validation errors are set to break, which
# crashes the run when no debugger is attached, so this test fails on API misuse.
bulkhead_png_test(capture_debug_layer smoke
  GAME  --capture "${CAP}/debug_layer" --frames 4 --res 640x360 --seed 7 --debug
  CHECK --dir "${CAP}/debug_layer" --count 4 --width 640 --height 360 --expect-motion)

# --- film -------------------------------------------------------------------
bulkhead_png_test(film_map_tour film
  GAME  --capture "${CAP}/round1_map_tour" --frames 240 --res 1920x1080 --fps 30 --seed 1234
  CHECK --dir "${CAP}/round1_map_tour" --count 240 --width 1920 --height 1080 --expect-motion)

foreach(seed 7 42 2026)
  bulkhead_png_test(film_overhead_${seed} film
    GAME  --overhead "${CAP}/overhead_seed${seed}.png" --seed ${seed}
    CHECK --file "${CAP}/overhead_seed${seed}.png" --width 832 --height 832)
endforeach()

bulkhead_png_test(film_overhead_large film
  GAME  --overhead "${CAP}/overhead_24x24_seed99.png" --seed 99 --size 24x24 --cell-px 48
  CHECK --file "${CAP}/overhead_24x24_seed99.png" --width 1200 --height 1200)
