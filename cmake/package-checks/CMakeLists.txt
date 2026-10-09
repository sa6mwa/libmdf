cmake_minimum_required(VERSION 3.24)
project(libmdf_artifact_checks NONE)
enable_testing()
include("${CMAKE_CURRENT_LIST_DIR}/../StableInputs.cmake")
libmdf_stable_input(CMakeCache.txt check_configuration)
get_filename_component(default_source "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LIBMDF_SOURCE_DIR "${default_source}" CACHE PATH "Owning source checkout")
set(LIBMDF_DIST_DIR "${LIBMDF_SOURCE_DIR}/dist" CACHE PATH "Artifact directory")
set(LIBMDF_PACKAGE_WORK_DIR "${LIBMDF_SOURCE_DIR}/build/package" CACHE PATH "Binary inventory directory")
set(LIBMDF_ARTIFACT_SCOPE binary CACHE STRING "binary, complete or lua")
set(LIBMDF_TEST_TARGETS "" CACHE STRING "Explicit rehearsal target selection")
execute_process(COMMAND "${LIBMDF_SOURCE_DIR}/scripts/version.sh"
  RESULT_VARIABLE result OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Unable to resolve artifact version")
endif()
set(environment "LIBMDF_DIST_DIR=${LIBMDF_DIST_DIR}" "LIBMDF_PACKAGE_WORK_DIR=${LIBMDF_PACKAGE_WORK_DIR}" "LIBMDF_TARGETS=${LIBMDF_TEST_TARGETS}")
if(LIBMDF_ARTIFACT_SCOPE STREQUAL "lua")
  set(script "${LIBMDF_SOURCE_DIR}/scripts/verify_lua_artifacts.sh")
  set(arguments)
  set(artifacts "${LIBMDF_DIST_DIR}/libmdf-lua-${version}.tar.gz" "${LIBMDF_DIST_DIR}/libmdf-${version}-1.rockspec"
    "${LIBMDF_DIST_DIR}/libmdf-${version}-1.src.rock" "${LIBMDF_DIST_DIR}/libmdf-${version}-x86_64-linux-gnu.tar.gz"
    "${LIBMDF_SOURCE_DIR}/build/debug/lua-runtime/bin/lua")
else()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${environment}
    "${LIBMDF_SOURCE_DIR}/scripts/release_artifacts.sh" "${LIBMDF_ARTIFACT_SCOPE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE names OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Invalid artifact verification scope")
  endif()
  string(REPLACE "\n" ";" names "${names}")
  set(artifacts)
  foreach(name IN LISTS names)
    list(APPEND artifacts "${LIBMDF_DIST_DIR}/${name}")
  endforeach()
  if(LIBMDF_ARTIFACT_SCOPE STREQUAL "binary")
    list(APPEND artifacts "${LIBMDF_PACKAGE_WORK_DIR}/binary-CHECKSUMS")
  else()
    list(APPEND artifacts "${LIBMDF_DIST_DIR}/libmdf-${version}-CHECKSUMS"
      "${LIBMDF_SOURCE_DIR}/build/debug/lua-runtime/bin/lua")
  endif()
  set(script "${LIBMDF_SOURCE_DIR}/scripts/package-verify.sh")
  set(arguments "${LIBMDF_ARTIFACT_SCOPE}")
endif()
foreach(artifact IN LISTS artifacts)
  if(NOT EXISTS "${artifact}")
    message(FATAL_ERROR "Missing prepared artifact prerequisite: ${artifact}")
  endif()
endforeach()
if(LIBMDF_ARTIFACT_SCOPE STREQUAL "complete")
  file(GLOB release_members CONFIGURE_DEPENDS
    "${LIBMDF_DIST_DIR}/libmdf-*.tar.gz" "${LIBMDF_DIST_DIR}/cmdf-*.tar.gz"
    "${LIBMDF_DIST_DIR}/libmdf-*-1.rockspec" "${LIBMDF_DIST_DIR}/libmdf-*-1.src.rock"
    "${LIBMDF_DIST_DIR}/libmdf-*-CHECKSUMS")
  # Native glob regeneration observes membership; this content output makes a
  # changed filename set invalidate success even when an added file is older.
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/release-members.txt" CONTENT "${release_members}\n")
  list(APPEND artifacts "${CMAKE_CURRENT_BINARY_DIR}/release-members.txt")
endif()
add_test(NAME verify_${LIBMDF_ARTIFACT_SCOPE} COMMAND "${CMAKE_COMMAND}" -E env ${environment} "${script}" ${arguments})
set_tests_properties(verify_${LIBMDF_ARTIFACT_SCOPE} PROPERTIES LABELS "packaging;e2e" TIMEOUT 600 RUN_SERIAL TRUE)
file(GLOB_RECURSE scripts CONFIGURE_DEPENDS "${LIBMDF_SOURCE_DIR}/scripts/*.sh" "${LIBMDF_SOURCE_DIR}/cmake/*.cmake"
  "${LIBMDF_SOURCE_DIR}/tests/package-consumer/*")
list(APPEND scripts "${LIBMDF_SOURCE_DIR}/tests/test_shared_exports.sh" "${LIBMDF_SOURCE_DIR}/CMakeLists.txt")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/verification-members.txt" CONTENT "${scripts}\n")
list(APPEND scripts "${CMAKE_CURRENT_BINARY_DIR}/verification-members.txt")
set(stamp "${CMAKE_CURRENT_BINARY_DIR}/artifacts.pass")
add_custom_command(OUTPUT "${stamp}"
  COMMAND "${CMAKE_COMMAND}" -E rm -f "${stamp}"
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_CURRENT_BINARY_DIR}"
    --output-on-failure --stop-on-failure --no-tests=error -j1
  COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
  DEPENDS ${artifacts} ${scripts} "${check_configuration}"
    "${CMAKE_CURRENT_LIST_FILE}"
  VERBATIM USES_TERMINAL)
add_custom_target(verify DEPENDS "${stamp}")
