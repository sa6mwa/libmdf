cmake_minimum_required(VERSION 3.24)
project(libmdf_lua_artifacts NONE)
get_filename_component(source "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LIBMDF_DIST_DIR "${source}/dist" CACHE PATH "Lua release artifact directory")
execute_process(COMMAND "${source}/scripts/version.sh"
  RESULT_VARIABLE result OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Unable to resolve Lua artifact version")
endif()
set(outputs "${LIBMDF_DIST_DIR}/libmdf-lua-${version}.tar.gz"
  "${LIBMDF_DIST_DIR}/libmdf-${version}-1.rockspec"
  "${LIBMDF_DIST_DIR}/libmdf-${version}-1.src.rock")
add_custom_command(OUTPUT ${outputs}
  COMMAND bash "${source}/scripts/package_lua_sources.sh"
    "${version}" "${LIBMDF_DIST_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS "${source}/LICENSE" "${source}/README.md"
    "${source}/libmdf-scm-1.rockspec.in"
    "${source}/lua/libmdf.lua" "${source}/lua/libmdf_core.c"
    "${source}/scripts/package_lua_sources.sh" "${source}/scripts/stage_lua_rock_sources.sh"
    "${source}/scripts/require_build_path.sh"
    "${source}/scripts/render_release_rockspec.sh" "${source}/scripts/archive.sh"
    "${source}/scripts/version.sh" "${CMAKE_CURRENT_LIST_FILE}"
  VERBATIM USES_TERMINAL)
add_custom_target(package DEPENDS ${outputs})
