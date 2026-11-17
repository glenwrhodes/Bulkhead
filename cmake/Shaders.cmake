# Locates dxc.exe from the Windows SDK and provides bulkhead_compile_shader(), which
# compiles one HLSL entry point into a C header holding the DXIL bytecode.

set(_bh_kits_bin "C:/Program Files (x86)/Windows Kits/10/bin")
set(_bh_dxc_hints "")
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
  list(APPEND _bh_dxc_hints "${_bh_kits_bin}/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
endif()
file(GLOB _bh_kit_versions LIST_DIRECTORIES true "${_bh_kits_bin}/10.*")
list(SORT _bh_kit_versions COMPARE NATURAL ORDER DESCENDING)
foreach(_v IN LISTS _bh_kit_versions)
  list(APPEND _bh_dxc_hints "${_v}/x64")
endforeach()

find_program(BULKHEAD_DXC dxc HINTS ${_bh_dxc_hints} DOC "DirectX Shader Compiler (dxc.exe)")
if(NOT BULKHEAD_DXC)
  message(FATAL_ERROR "dxc.exe not found. Install the Windows 10/11 SDK.")
endif()
message(STATUS "Bulkhead: using dxc ${BULKHEAD_DXC}")

set(BULKHEAD_SHADER_GEN_DIR "${CMAKE_BINARY_DIR}/generated/shaders")
file(MAKE_DIRECTORY "${BULKHEAD_SHADER_GEN_DIR}")

# bulkhead_compile_shader(<out_var> <hlsl> <entry> <profile> <c_variable_name> [extra deps...])
function(bulkhead_compile_shader out_var hlsl entry profile var_name)
  set(_src "${CMAKE_SOURCE_DIR}/${hlsl}")
  set(_out "${BULKHEAD_SHADER_GEN_DIR}/${var_name}.h")
  add_custom_command(
    OUTPUT  "${_out}"
    COMMAND "${BULKHEAD_DXC}" -nologo -T ${profile} -E ${entry} -O3 -WX -Fh "${_out}" -Vn ${var_name} "${_src}"
    DEPENDS "${_src}" ${ARGN}
    COMMENT "dxc ${profile} ${entry}  (${hlsl})"
    VERBATIM)
  set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()
