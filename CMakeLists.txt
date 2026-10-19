cmake_minimum_required(VERSION 3.24)

execute_process(
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/version.sh"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  OUTPUT_VARIABLE LIBMDF_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE LIBMDF_VERSION_RESULT)
if(NOT LIBMDF_VERSION_RESULT EQUAL 0)
  message(FATAL_ERROR "Unable to resolve the libmdf source version")
endif()
if(NOT LIBMDF_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "Invalid libmdf source version: ${LIBMDF_VERSION}")
endif()
string(REPLACE "." ";" LIBMDF_VERSION_PARTS "${LIBMDF_VERSION}")
list(GET LIBMDF_VERSION_PARTS 0 LIBMDF_VERSION_MAJOR)
list(GET LIBMDF_VERSION_PARTS 1 LIBMDF_VERSION_MINOR)
list(GET LIBMDF_VERSION_PARTS 2 LIBMDF_VERSION_PATCH)

project(libmdf VERSION "${LIBMDF_VERSION}" LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set_property(GLOBAL APPEND PROPERTY JOB_POOLS lifecycle=1)

set(LIBMDF_ABI_VERSION 6)

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)

set(LIBMDF_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/libmdf/mdf.h.in"
  "${LIBMDF_GENERATED_INCLUDE_DIR}/libmdf/mdf.h"
  @ONLY)

option(LIBMDF_BUILD_STATIC "Build static libmdf" ON)
option(LIBMDF_BUILD_SHARED "Build shared libmdf" ON)
option(LIBMDF_BUILD_BINARY "Build cmdf" ON)
option(LIBMDF_BUILD_EXAMPLES "Build examples" ON)
option(LIBMDF_BUILD_TESTS "Build tests" ON)
option(LIBMDF_BUILD_HOST_TESTS "Include host lifecycle and Go parity checks" ON)
option(LIBMDF_BUILD_LUA_TESTS "Build and test the Lua facade against an installed SDK" OFF)
option(LIBMDF_BUILD_FUZZERS "Build fuzzers" OFF)
option(LIBMDF_CMDF_STATIC_RUNTIME "Link cmdf with a static C runtime where the target supports it" OFF)
option(LIBMDF_INSTALL "Install libmdf" ON)
option(LIBMDF_INSTALL_BINARY "Install cmdf when it is built" ON)
set(LIBMDF_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist" CACHE PATH "Release artifact directory")
set(LIBMDF_TARGET_ID "" CACHE STRING "Release target id")
set(LIBMDF_TARGET_ARCH "" CACHE STRING "Release target arch")
set(LIBMDF_TARGET_OS "" CACHE STRING "Release target os")
set(LIBMDF_TARGET_LIBC "" CACHE STRING "Release target libc")

# Linux toolchain files provide the real implementation before project().
# Keep non-Linux packaging and source-inspection configurations valid without
# applying ELF-only flags to their executables.
if(NOT COMMAND libmdf_configure_development_runtime)
  function(libmdf_configure_development_runtime target)
  endfunction()
endif()

if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
  set(LIBMDF_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
  set(LIBMDF_PC_LIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}")
else()
  string(REPLACE "/" ";" LIBMDF_PC_LIBDIR_PARTS "${CMAKE_INSTALL_LIBDIR}")
  list(LENGTH LIBMDF_PC_LIBDIR_PARTS LIBMDF_PC_LIBDIR_DEPTH)
  set(LIBMDF_PC_PREFIX "\${pcfiledir}/..")
  foreach(_ RANGE 1 ${LIBMDF_PC_LIBDIR_DEPTH})
    set(LIBMDF_PC_PREFIX "${LIBMDF_PC_PREFIX}/..")
  endforeach()
  set(LIBMDF_PC_LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
  set(LIBMDF_PC_INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
else()
  set(LIBMDF_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()

set(LIBMDF_BUILD_LIBRARIES OFF)
foreach(LIBMDF_LIBRARY_OPTION LIBMDF_BUILD_STATIC LIBMDF_BUILD_SHARED)
  if(${LIBMDF_LIBRARY_OPTION})
    set(LIBMDF_BUILD_LIBRARIES ON)
  endif()
endforeach()
if(NOT LIBMDF_BUILD_LIBRARIES)
  message(FATAL_ERROR "libmdf requires LIBMDF_BUILD_STATIC or LIBMDF_BUILD_SHARED")
endif()

set(LIBMDF_C89_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/c89_assets.h")
add_custom_command(OUTPUT "${LIBMDF_C89_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/assets/c89"
    "-DOUTPUT=${LIBMDF_C89_HEADER}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedC89.cmake"
  DEPENDS cmake/EmbedC89.cmake assets/c89/c89-mark.svg assets/c89/c89-systems.svg
          assets/c89/style.css assets/c89/navigation.js
  VERBATIM)
add_custom_target(libmdf-c89-assets DEPENDS "${LIBMDF_C89_HEADER}")

set(LIBMDF_SOURCES
  src/mdf.c
  src/render.c
  src/render_ansi.c
  src/render_ansi_blocks.c
  src/html_fonts.c
  src/html_c89.c
  src/pager.c
  src/path.c
  src/render_html.c
  src/render_parse.c
  src/unicode_classify.c
  src/unicode_casefold.c)

function(libmdf_configure_c89_target target)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(${target} PRIVATE
      -std=c89
      -Wall
      -Wextra
      -Wpedantic
      -pedantic-errors
      -Werror)
  endif()
endfunction()

function(libmdf_target_defaults target)
  add_dependencies(${target} libmdf-c89-assets)
  target_include_directories(${target}
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    PUBLIC
      "$<BUILD_INTERFACE:${LIBMDF_GENERATED_INCLUDE_DIR}>"
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  libmdf_configure_c89_target(${target})
  target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
  # macOS keeps O_NOFOLLOW behind its extension feature macro even when the
  # POSIX interface level is selected. The forced font-dump path relies on it
  # to reject a final-component symlink before opening the target for writing.
  if(APPLE)
    target_compile_definitions(${target} PRIVATE _DARWIN_C_SOURCE)
  endif()
endfunction()

set(LIBMDF_EXPORT_TARGETS)

if(LIBMDF_BUILD_STATIC)
  add_library(mdf_static STATIC ${LIBMDF_SOURCES})
  add_library(libmdf::mdf_static ALIAS mdf_static)
  set_target_properties(mdf_static PROPERTIES OUTPUT_NAME mdf)
  libmdf_target_defaults(mdf_static)
  list(APPEND LIBMDF_EXPORT_TARGETS mdf_static)
endif()

if(LIBMDF_BUILD_SHARED)
  add_library(mdf_shared SHARED ${LIBMDF_SOURCES})
  add_library(libmdf::mdf_shared ALIAS mdf_shared)
  set_target_properties(mdf_shared PROPERTIES
    OUTPUT_NAME mdf
    SOVERSION ${LIBMDF_ABI_VERSION}
    VERSION ${PROJECT_VERSION}
    BUILD_RPATH ""
    INSTALL_RPATH "")
  if(UNIX AND NOT APPLE)
    set_property(TARGET mdf_shared APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libmdf.exports.map")
    target_link_options(mdf_shared PRIVATE
      "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/libmdf.exports.map")
  elseif(APPLE)
    set_property(TARGET mdf_shared APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libmdf.exports.darwin")
    target_link_options(mdf_shared PRIVATE
      "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/libmdf.exports.darwin")
  endif()
  if(APPLE)
    set_target_properties(mdf_shared PROPERTIES
      BUILD_WITH_INSTALL_NAME_DIR TRUE
      MACOSX_RPATH ON
      INSTALL_NAME_DIR "@rpath")
  endif()
  libmdf_target_defaults(mdf_shared)
  list(APPEND LIBMDF_EXPORT_TARGETS mdf_shared)
endif()

if(LIBMDF_BUILD_BINARY)
  add_executable(cmdf src/cmdf.c)
  target_include_directories(cmdf PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(cmdf PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_c89_target(cmdf)
  target_compile_definitions(cmdf PRIVATE _POSIX_C_SOURCE=200809L)
  if(LIBMDF_CMDF_STATIC_RUNTIME AND NOT APPLE)
    target_link_options(cmdf PRIVATE -static)
  else()
    libmdf_configure_development_runtime(cmdf)
  endif()
  include(cmake/LuaCli.cmake)
endif()

if(LIBMDF_BUILD_EXAMPLES)
  add_executable(example_basic examples/basic.c)
  target_include_directories(example_basic PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(example_basic PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_c89_target(example_basic)
  libmdf_configure_development_runtime(example_basic)
endif()

include(cmake/Tests.cmake)
if(LIBMDF_BUILD_LUA_TESTS)
  include(cmake/LuaTests.cmake)
endif()

if(LIBMDF_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER MATCHES "cpkt-afl-gcc$")
    message(FATAL_ERROR "LIBMDF_BUILD_FUZZERS requires the pinned AFL++ Bootlin toolchain")
  endif()
  add_executable(fuzz_smoke fuzz/fuzz_smoke.c)
  target_include_directories(fuzz_smoke PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(fuzz_smoke PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_c89_target(fuzz_smoke)
  libmdf_configure_development_runtime(fuzz_smoke)
  include(cmake/Fuzz.cmake)
endif()

include(cmake/Checks.cmake)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  get_property(LIBMDF_BUILD_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(LIBMDF_BUILD_TARGET IN LISTS LIBMDF_BUILD_TARGETS)
    get_target_property(LIBMDF_BUILD_TARGET_TYPE ${LIBMDF_BUILD_TARGET} TYPE)
    if(LIBMDF_BUILD_TARGET_TYPE STREQUAL "EXECUTABLE")
      if(LIBMDF_BUILD_TARGET STREQUAL "cmdf" AND LIBMDF_CMDF_STATIC_RUNTIME)
        continue()
      endif()
      get_target_property(LIBMDF_BUILD_TARGET_RUNTIME ${LIBMDF_BUILD_TARGET} LIBMDF_BOOTLIN_DEVELOPMENT_RUNTIME)
      if(NOT LIBMDF_BUILD_TARGET_RUNTIME)
        message(FATAL_ERROR "Non-shipped Linux executable ${LIBMDF_BUILD_TARGET} must use libmdf_configure_development_runtime")
      endif()
    endif()
  endforeach()
endif()

if(LIBMDF_INSTALL)
  install(FILES include/libmdf.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT sdk)
  install(FILES ${LIBMDF_GENERATED_INCLUDE_DIR}/libmdf/mdf.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libmdf COMPONENT sdk)
  if(LIBMDF_EXPORT_TARGETS)
    install(TARGETS ${LIBMDF_EXPORT_TARGETS}
      EXPORT libmdfTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT sdk)
    install(EXPORT libmdfTargets
      NAMESPACE libmdf::
      FILE libmdfTargets.cmake
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmdf COMPONENT sdk)
  endif()
  if(TARGET cmdf AND LIBMDF_INSTALL_BINARY)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT LIBMDF_CMDF_STATIC_RUNTIME)
      message(STATUS "Development cmdf is not installed: use LIBMDF_CMDF_STATIC_RUNTIME=ON for a portable Linux CLI")
    else()
      install(TARGETS cmdf RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT cli)
      install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/cmdf.lua" DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT cli)
      install(FILES LICENSE README.md src/html_embedded/OFL.txt DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cmdf COMPONENT cli)
    endif()
  endif()
  install(FILES LICENSE README.md src/html_embedded/OFL.txt DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libmdf COMPONENT sdk)
  configure_package_config_file(cmake/libmdfConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/libmdfConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmdf)
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/libmdfConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
  configure_file(cmake/libmdf.pc.in ${CMAKE_CURRENT_BINARY_DIR}/libmdf.pc @ONLY)
  configure_file(cmake/package-metadata.txt.in ${CMAKE_CURRENT_BINARY_DIR}/package-metadata.txt @ONLY)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/libmdfConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/libmdfConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmdf COMPONENT sdk)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libmdf.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig COMPONENT sdk)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/package-metadata.txt DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/libmdf COMPONENT sdk)
endif()

include(cmake/Package.cmake)
