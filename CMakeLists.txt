cmake_minimum_required(VERSION 3.20)

execute_process(
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/version.sh"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  OUTPUT_VARIABLE LIBMDF_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE LIBMDF_VERSION_RESULT)
if(NOT LIBMDF_VERSION_RESULT EQUAL 0)
  set(LIBMDF_VERSION "0.0.0")
endif()
if(NOT LIBMDF_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  set(LIBMDF_VERSION "0.0.0")
endif()
string(REPLACE "." ";" LIBMDF_VERSION_PARTS "${LIBMDF_VERSION}")
list(GET LIBMDF_VERSION_PARTS 0 LIBMDF_VERSION_MAJOR)
list(GET LIBMDF_VERSION_PARTS 1 LIBMDF_VERSION_MINOR)
list(GET LIBMDF_VERSION_PARTS 2 LIBMDF_VERSION_PATCH)

project(libmdf VERSION "${LIBMDF_VERSION}" LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(LIBMDF_ABI_VERSION 4)

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

set(LIBMDF_SOURCES
  src/mdf.c
  src/render.c
  src/render_ansi.c
  src/render_ansi_blocks.c
  src/html_fonts.c
  src/pager.c
  src/path.c
  src/render_html.c
  src/render_parse.c
  src/unicode_classify.c
  src/unicode_casefold.c)

function(libmdf_target_defaults target)
  target_include_directories(${target}
    PUBLIC
      "$<BUILD_INTERFACE:${LIBMDF_GENERATED_INCLUDE_DIR}>"
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  target_compile_features(${target} PUBLIC c_std_90)
  target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
  # macOS keeps O_NOFOLLOW behind its extension feature macro even when the
  # POSIX interface level is selected. The forced font-dump path relies on it
  # to reject a final-component symlink before opening the target for writing.
  if(APPLE)
    target_compile_definitions(${target} PRIVATE _DARWIN_C_SOURCE)
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
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
  target_compile_features(cmdf PRIVATE c_std_90)
  target_compile_definitions(cmdf PRIVATE _POSIX_C_SOURCE=200809L)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(cmdf PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  if(LIBMDF_CMDF_STATIC_RUNTIME AND NOT APPLE)
    target_link_options(cmdf PRIVATE -static)
  else()
    libmdf_configure_development_runtime(cmdf)
  endif()
endif()

if(LIBMDF_BUILD_EXAMPLES)
  add_executable(example_basic examples/basic.c)
  target_include_directories(example_basic PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(example_basic PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_development_runtime(example_basic)
endif()

if(LIBMDF_BUILD_TESTS)
  enable_testing()
  add_executable(test_internal_api tests/test_api.c)
  target_include_directories(test_internal_api PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(test_internal_api PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_development_runtime(test_internal_api)
  add_test(NAME test_internal_api COMMAND test_internal_api)
  set_tests_properties(test_internal_api PROPERTIES
    LABELS "unit;internal"
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_executable(test_public_api tests/test_public_api.c)
  target_compile_features(test_public_api PRIVATE c_std_90)
  target_compile_definitions(test_public_api PRIVATE _POSIX_C_SOURCE=200809L)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(test_public_api PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  target_include_directories(test_public_api PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(test_public_api PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_development_runtime(test_public_api)
  add_test(NAME test_public_api COMMAND test_public_api)
  set_tests_properties(test_public_api PROPERTIES LABELS "unit;api" TIMEOUT 10)

  add_executable(test_contracts tests/test_contracts.c)
  target_include_directories(test_contracts PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(test_contracts PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_development_runtime(test_contracts)
  add_test(NAME test_contracts COMMAND test_contracts)
  set_tests_properties(test_contracts PROPERTIES LABELS "unit;streaming;api" TIMEOUT 10)

  add_executable(incremental_decision_parity tests/incremental_decision_parity.c)
  target_compile_features(incremental_decision_parity PRIVATE c_std_90)
  target_include_directories(incremental_decision_parity PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(incremental_decision_parity PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_development_runtime(incremental_decision_parity)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(incremental_decision_parity PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME incremental_decision_goldens
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_incremental_decision_goldens.sh
            $<TARGET_FILE:incremental_decision_parity>)
  set_tests_properties(incremental_decision_goldens PROPERTIES
    LABELS "integration;streaming;api;golden"
    TIMEOUT 120
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_executable(golden_render tests/golden_render.c)
  target_compile_features(golden_render PRIVATE c_std_90)
  target_include_directories(golden_render PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(golden_render PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_development_runtime(golden_render)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(golden_render PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME libmdf_goldens
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_libmdf_goldens.sh
            $<TARGET_FILE:golden_render>)
  set_tests_properties(libmdf_goldens PROPERTIES
    LABELS "integration;api;golden"
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  if(TARGET cmdf)
    add_test(NAME narrow_quote_url_streaming
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_narrow_quote_url_streaming.sh
              $<TARGET_FILE:golden_render>
              $<TARGET_FILE:incremental_decision_parity>
              $<TARGET_FILE:cmdf>)
    set_tests_properties(narrow_quote_url_streaming PROPERTIES
      LABELS "integration;streaming;api;cli;golden"
      TIMEOUT 20
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(NAME pending_emphasis_wrap_streaming
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pending_emphasis_wrap_streaming.sh
              $<TARGET_FILE:golden_render>
              $<TARGET_FILE:incremental_decision_parity>
              $<TARGET_FILE:cmdf>)
    set_tests_properties(pending_emphasis_wrap_streaming PROPERTIES
      LABELS "integration;streaming;api;cli;golden"
      TIMEOUT 20
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(test_cmdf tests/test_cmdf.c)
    target_compile_features(test_cmdf PRIVATE c_std_90)
    target_compile_definitions(test_cmdf PRIVATE _POSIX_C_SOURCE=200809L)
    libmdf_configure_development_runtime(test_cmdf)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(test_cmdf PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME test_cmdf COMMAND test_cmdf $<TARGET_FILE:cmdf>)
    set_tests_properties(test_cmdf PROPERTIES LABELS "integration;cli" TIMEOUT 30)

    # Keep the pager fault injector in a private test library so default
    # test-enabled builds never add it to installable static/shared targets.
    add_library(mdf_pager_test STATIC EXCLUDE_FROM_ALL ${LIBMDF_SOURCES})
    libmdf_target_defaults(mdf_pager_test)
    target_compile_definitions(mdf_pager_test PRIVATE LIBMDF_PAGER_TESTING)

    add_executable(test_pager tests/test_pager.c)
    libmdf_configure_development_runtime(test_pager)
    target_compile_features(test_pager PRIVATE c_std_90)
    target_compile_definitions(test_pager PRIVATE _POSIX_C_SOURCE=200809L LIBMDF_PAGER_TESTING)
    # Darwin places terminal resize signals and anonymous-mapping aliases
    # behind its extension feature selector. The PTY integration test uses
    # those native terminal interfaces on that target.
    if(APPLE)
      target_compile_definitions(test_pager PRIVATE _DARWIN_C_SOURCE)
    endif()
    target_include_directories(test_pager PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_pager PRIVATE mdf_pager_test)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_libraries(test_pager PRIVATE util)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(test_pager PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME test_pager COMMAND test_pager $<TARGET_FILE:cmdf>)
    # The terminal pager runs after forkpty. Keep this integration test separate
    # from the normal unit suite so it can retain a focused timeout and label.
    set_tests_properties(test_pager PROPERTIES LABELS "integration;cli;pager;forkpty" TIMEOUT 30)

    add_executable(test_pager_ratio tests/test_pager_ratio.c)
    libmdf_configure_development_runtime(test_pager_ratio)
    target_compile_features(test_pager_ratio PRIVATE c_std_90)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(test_pager_ratio PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME test_pager_ratio COMMAND test_pager_ratio)
    set_tests_properties(test_pager_ratio PROPERTIES LABELS "unit;pager" TIMEOUT 10)

    add_test(NAME chart_corpus
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chart_corpus.sh
              $<TARGET_FILE:cmdf>
              ${CMAKE_CURRENT_SOURCE_DIR}/testdata/chart-corpus)
    set_tests_properties(chart_corpus PROPERTIES
      LABELS "integration;cli;charts"
      TIMEOUT 20
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(NAME cmdf_goldens
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cmdf_goldens.sh
              $<TARGET_FILE:cmdf>)
    set_tests_properties(cmdf_goldens PROPERTIES
      LABELS "integration;cli;golden"
      TIMEOUT 30
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(NAME cmdf_incremental
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cmdf_incremental.sh
              $<TARGET_FILE:cmdf>)
    set_tests_properties(cmdf_incremental PROPERTIES
      LABELS "integration;cli;streaming;golden"
      TIMEOUT 30
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  foreach(LIBMDF_PRODUCTION_TARGET IN LISTS LIBMDF_EXPORT_TARGETS)
    add_test(NAME ${LIBMDF_PRODUCTION_TARGET}_excludes_pager_test_hook
      COMMAND ${CMAKE_COMMAND}
              -DNM=${CMAKE_NM}
              -DLIBRARY=$<TARGET_FILE:${LIBMDF_PRODUCTION_TARGET}>
              -DSYMBOL=mdf_pager_test_set_termination_handler_install_failure
              -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_symbol_absent.cmake)
    set_tests_properties(${LIBMDF_PRODUCTION_TARGET}_excludes_pager_test_hook PROPERTIES
      LABELS "unit;api"
      TIMEOUT 10)
  endforeach()

  add_test(NAME header_self_sufficient
    COMMAND ${CMAKE_C_COMPILER} -std=c89 -I${LIBMDF_GENERATED_INCLUDE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/include -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/header_self_sufficient.c -o ${CMAKE_CURRENT_BINARY_DIR}/header_self_sufficient.o)
  set_tests_properties(header_self_sufficient PROPERTIES LABELS "unit;api" TIMEOUT 10)

  add_test(NAME source_header_self_sufficient
    COMMAND ${CMAKE_C_COMPILER} -std=c89 -I${CMAKE_CURRENT_SOURCE_DIR}/include -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/header_self_sufficient.c -o ${CMAKE_CURRENT_BINARY_DIR}/source_header_self_sufficient.o)
  set_tests_properties(source_header_self_sufficient PROPERTIES LABELS "unit;api" TIMEOUT 10)

  get_property(LIBMDF_BOOTLIN_DEVELOPMENT_TARGETS GLOBAL PROPERTY LIBMDF_BOOTLIN_DEVELOPMENT_TARGETS)
  foreach(LIBMDF_BOOTLIN_TARGET IN LISTS LIBMDF_BOOTLIN_DEVELOPMENT_TARGETS)
    add_test(NAME ${LIBMDF_BOOTLIN_TARGET}_uses_bootlin_runtime
      COMMAND ${CMAKE_COMMAND}
              -DREADELF=${CMAKE_READELF}
              -DEXECUTABLE=$<TARGET_FILE:${LIBMDF_BOOTLIN_TARGET}>
              -DINTERPRETER=${LIBMDF_BOOTLIN_LOADER}
              -DRUNTIME_DIR=${LIBMDF_BOOTLIN_sysroot}/lib
              -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_bootlin_runtime.cmake)
    set_tests_properties(${LIBMDF_BOOTLIN_TARGET}_uses_bootlin_runtime PROPERTIES
      LABELS "unit;lifecycle;runtime"
      TIMEOUT 10)
  endforeach()

  add_test(NAME version_source_archive COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_version_source.sh)
  set_tests_properties(version_source_archive PROPERTIES LABELS "unit;packaging" TIMEOUT 10)

  add_test(NAME release_lifecycle COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_lifecycle.sh)
  set_tests_properties(release_lifecycle PROPERTIES LABELS "unit;lifecycle" TIMEOUT 10
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME release_privacy_negative COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_privacy_negative.sh)
  set_tests_properties(release_privacy_negative PROPERTIES
    LABELS "unit;packaging"
    TIMEOUT 10
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME emission_surface_invariants COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_emission_surface.sh)
  set_tests_properties(emission_surface_invariants PROPERTIES LABELS "unit;streaming" TIMEOUT 10)

  add_test(NAME package_install_cache COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_install_cache.sh)
  set_tests_properties(package_install_cache PROPERTIES
    LABELS "packaging"
    TIMEOUT 120
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME discover_target_tools COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_discover_target_tools.sh)
  set_tests_properties(discover_target_tools PROPERTIES
    LABELS "unit;packaging"
    TIMEOUT 10
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME verify_release_privacy_macho COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_verify_release_privacy_macho.sh)
  set_tests_properties(verify_release_privacy_macho PROPERTIES
    LABELS "unit;packaging"
    TIMEOUT 10
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME darwin_linker_route COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_darwin_linker_route.sh)
  set_tests_properties(darwin_linker_route PROPERTIES
    LABELS "unit;packaging"
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME configure_profiles COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_configure_profiles.sh)
  set_tests_properties(configure_profiles PROPERTIES
    LABELS "unit;packaging"
    TIMEOUT 120
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME cmake_option_matrix COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cmake_options.sh)
  add_test(NAME cmdf_install_boundary COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cmdf_install.sh)
  set_tests_properties(cmdf_install_boundary PROPERTIES LABELS "unit;packaging" TIMEOUT 90)
  set_tests_properties(cmake_option_matrix PROPERTIES
    LABELS "unit;packaging"
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME parityjudge_suite_stdin
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_parityjudge.sh)
  set_tests_properties(parityjudge_suite_stdin PROPERTIES
    LABELS "integration;parity"
    TIMEOUT 60
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/parityjudge)

  foreach(case_name
      ansi_table_wire
      ansi_table_boring
      ansi_table_row_trace
      ansi_regressions
      ansi_inline_code_punctuation
      html_regressions)
    add_test(NAME parityjudge_option_${case_name}
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parityjudge_options.sh ${case_name})
    set_tests_properties(parityjudge_option_${case_name} PROPERTIES
      LABELS "integration;parity"
      TIMEOUT 60
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endforeach()
endif()

if(LIBMDF_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER MATCHES "cpkt-afl-gcc$")
    message(FATAL_ERROR "LIBMDF_BUILD_FUZZERS requires the pinned AFL++ Bootlin toolchain")
  endif()
  add_executable(fuzz_smoke fuzz/fuzz_smoke.c)
  target_include_directories(fuzz_smoke PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(fuzz_smoke PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_development_runtime(fuzz_smoke)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fuzz_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

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
  install(FILES include/libmdf.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(FILES ${LIBMDF_GENERATED_INCLUDE_DIR}/libmdf/mdf.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libmdf)
  if(LIBMDF_EXPORT_TARGETS)
    install(TARGETS ${LIBMDF_EXPORT_TARGETS}
      EXPORT libmdfTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(EXPORT libmdfTargets
      NAMESPACE libmdf::
      FILE libmdfTargets.cmake
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmdf)
  endif()
  if(TARGET cmdf AND LIBMDF_INSTALL_BINARY)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT LIBMDF_CMDF_STATIC_RUNTIME)
      message(STATUS "Development cmdf is not installed: use LIBMDF_CMDF_STATIC_RUNTIME=ON for a portable Linux CLI")
    else()
      install(TARGETS cmdf RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
      install(FILES LICENSE README.md src/html_embedded/OFL.txt DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cmdf)
    endif()
  endif()
  install(FILES LICENSE README.md src/html_embedded/OFL.txt DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libmdf)
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
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmdf)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libmdf.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/package-metadata.txt DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/libmdf)
endif()
