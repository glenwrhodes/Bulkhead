# Builds the downloadable zip: installs the game into <OUT>/Bulkhead and zips that folder
# into <OUT>/Bulkhead-windows-x64.zip (one top-level "Bulkhead" folder inside).
#
#   cmake -DBUILD=<build dir> -DOUT=<output dir> [-DCONFIG=Release] -P cmake/PackageZip.cmake

if(NOT BUILD OR NOT OUT)
  message(FATAL_ERROR "usage: cmake -DBUILD=<build dir> -DOUT=<output dir> [-DCONFIG=Release] -P cmake/PackageZip.cmake")
endif()
if(NOT CONFIG)
  set(CONFIG Release)
endif()
file(TO_CMAKE_PATH "${BUILD}" BUILD)
file(TO_CMAKE_PATH "${OUT}" OUT)
get_filename_component(BUILD "${BUILD}" ABSOLUTE)
get_filename_component(OUT "${OUT}" ABSOLUTE)

set(stage "${OUT}/Bulkhead")
set(zip "${OUT}/Bulkhead-windows-x64.zip")
file(REMOVE_RECURSE "${stage}")
file(REMOVE "${zip}")
file(MAKE_DIRECTORY "${OUT}")

execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD}" --config "${CONFIG}" --prefix "${stage}"
                RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "package: cmake --install failed (${rc})")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${zip}" --format=zip Bulkhead
                WORKING_DIRECTORY "${OUT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "package: zipping failed (${rc})")
endif()
file(SIZE "${zip}" bytes)
math(EXPR mb "${bytes} / 1048576")
message(STATUS "package: ${zip} (${mb} MB)")
