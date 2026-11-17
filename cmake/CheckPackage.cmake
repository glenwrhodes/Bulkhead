# Checks the downloadable zip the way a player gets it:
#   1. unpack it into an empty folder;
#   2. every file is there (the EXE, the models, the text files) and no test programs;
#   3. run it from that folder with nothing but Windows on PATH:
#        Bulkhead.exe --help      starts at all (no missing DLL)
#        Bulkhead.exe --check     finds and loads its own models (not the source tree's),
#                                 builds a ship (CPU only)
#      and with -DGPU=ON (this PC, which has a ray-tracing GPU):
#        Bulkhead.exe --capture   renders frames headless from the unpacked copy
#
#   cmake -DZIP=<zip> -DWORK=<empty folder to use> [-DGPU=ON] [-DOUT=<summary file>] -P cmake/CheckPackage.cmake

if(NOT ZIP OR NOT WORK)
  message(FATAL_ERROR "usage: cmake -DZIP=<zip> -DWORK=<folder> [-DGPU=ON] [-DOUT=<file>] -P cmake/CheckPackage.cmake")
endif()
file(TO_CMAKE_PATH "${ZIP}" ZIP)
file(TO_CMAKE_PATH "${WORK}" WORK)
get_filename_component(ZIP "${ZIP}" ABSOLUTE)
get_filename_component(WORK "${WORK}" ABSOLUTE)
if(OUT)
  file(TO_CMAKE_PATH "${OUT}" OUT)
endif()
if(NOT EXISTS "${ZIP}")
  message(FATAL_ERROR "package: no zip at ${ZIP}")
endif()

# 1. unpack into an empty folder
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${ZIP}" WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "package: could not unpack ${ZIP}")
endif()
set(DIR "${WORK}/Bulkhead")

# 2. contents
set(required Bulkhead.exe PLAY.txt THIRD_PARTY_NOTICES.txt assets/kit_manifest.json
             assets/corridor_straight.glb assets/door_sliding.glb assets/room_floor.glb assets/handgun.glb)
foreach(f IN LISTS required)
  if(NOT EXISTS "${DIR}/${f}")
    message(FATAL_ERROR "package: ${f} is missing")
  endif()
endforeach()
foreach(f maze_tests.exe level_tests.exe capture_check.exe)
  if(EXISTS "${DIR}/${f}")
    message(FATAL_ERROR "package: test program ${f} should not be in the download")
  endif()
endforeach()
file(GLOB glbs "${DIR}/assets/*.glb")
list(LENGTH glbs glbCount)
file(GLOB_RECURSE all RELATIVE "${DIR}" "${DIR}/*")
list(LENGTH all count)
file(SIZE "${ZIP}" zipBytes)
math(EXPR zipMb "${zipBytes} / 1048576")
message(STATUS "package: ${count} files (${glbCount} models), zip ${zipMb} MB")

# 3. run it from the unpacked folder, with nothing but Windows on PATH
set(ENV{PATH} "$ENV{SystemRoot}\\System32;$ENV{SystemRoot}")
execute_process(COMMAND "${DIR}/Bulkhead.exe" --help WORKING_DIRECTORY "${DIR}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "package: Bulkhead.exe --help failed from the unpacked folder: exit ${rc}\n${out}\n${err}")
endif()

# a working folder elsewhere, so the models can only be found beside the EXE
set(run "${WORK}/run")
file(MAKE_DIRECTORY "${run}")
execute_process(COMMAND "${DIR}/Bulkhead.exe" --check WORKING_DIRECTORY "${run}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
set(log "${out}\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "package: Bulkhead.exe --check failed: exit ${rc}\n${log}")
endif()
string(REGEX MATCH "Check OK: assets ([^\r\n]*)" _ "${log}")
set(found "${CMAKE_MATCH_1}")
file(TO_CMAKE_PATH "${found}" foundCm)
string(TOLOWER "${foundCm}" foundLower)
string(TOLOWER "${DIR}/assets" wantLower)
if(NOT foundLower STREQUAL wantLower)
  message(FATAL_ERROR "package: the EXE loaded its models from '${found}', not from its own folder '${DIR}/assets'\n${log}")
endif()
string(REGEX MATCH "Level: [^\r\n]*" levelLine "${log}")
message(STATUS "package: --check loaded the models from the unpacked folder; ${levelLine}")
set(ran "starts, and `--check` loads its own models and builds a ship (no GPU needed)")

if(GPU)
  set(cap "${run}/capture")
  execute_process(COMMAND "${DIR}/Bulkhead.exe" --capture "${cap}" --frames 3 --res 320x180 --spp 2 --start 2
                  WORKING_DIRECTORY "${run}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "package: capture from the unpacked folder failed: exit ${rc}\n${out}\n${err}")
  endif()
  foreach(i 1 2 3)
    if(NOT EXISTS "${cap}/frame_000${i}.png")
      message(FATAL_ERROR "package: the capture wrote no frame_000${i}.png")
    endif()
  endforeach()
  file(READ "${cap}/capture.log" capLog)
  file(TO_NATIVE_PATH "${DIR}/assets" dirNative)
  string(TOLOWER "${capLog}" capLower)
  string(TOLOWER "${dirNative}" dirLower)
  string(FIND "${capLower}" "${dirLower}" at)
  if(at LESS 0)
    message(FATAL_ERROR "package: the capture did not load its models from ${dirNative}\n${capLog}")
  endif()
  message(STATUS "package: a ray-traced capture ran from the unpacked folder (3 frames)")
  set(ran "${ran}, and a ray-traced capture runs from the unpacked folder")
endif()

if(OUT)
  file(APPEND "${OUT}" "## Download\n\n`Bulkhead-windows-x64.zip`: ${count} files (${glbCount} models), ${zipMb} MB. Unpacked into an empty folder, `Bulkhead.exe` ${ran}.\n\n")
endif()
