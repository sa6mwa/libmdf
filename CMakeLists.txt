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
  src/render_html.c
  src/render_parse.c)

function(libmdf_target_defaults target)
  target_include_directories(${target}
    PUBLIC
      "$<BUILD_INTERFACE:${LIBMDF_GENERATED_INCLUDE_DIR}>"
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  target_compile_features(${target} PUBLIC c_std_90)
  target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
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
    SOVERSION 0
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
  add_executable(cmdf src/cmdf.c src/cmdf_fonts.c)
  target_include_directories(cmdf PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(cmdf PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  target_compile_features(cmdf PRIVATE c_std_90)
  target_compile_definitions(cmdf PRIVATE _POSIX_C_SOURCE=200809L)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(cmdf PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  if(LIBMDF_CMDF_STATIC_RUNTIME AND NOT APPLE)
    target_link_options(cmdf PRIVATE -static)
  endif()
endif()

if(LIBMDF_BUILD_EXAMPLES)
  add_executable(example_basic examples/basic.c)
  target_include_directories(example_basic PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(example_basic PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
endif()

if(LIBMDF_BUILD_TESTS)
  enable_testing()
  add_executable(test_internal_api tests/test_api.c)
  target_include_directories(test_internal_api PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(test_internal_api PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  add_test(NAME test_internal_api COMMAND test_internal_api)
  set_tests_properties(test_internal_api PROPERTIES
    LABELS "unit;internal"
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_executable(test_public_api tests/test_public_api.c)
  target_include_directories(test_public_api PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(test_public_api PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  add_test(NAME test_public_api COMMAND test_public_api)
  set_tests_properties(test_public_api PROPERTIES LABELS "unit;api" TIMEOUT 10)

  add_executable(test_contracts tests/test_contracts.c)
  target_include_directories(test_contracts PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(test_contracts PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  add_test(NAME test_contracts COMMAND test_contracts)
  set_tests_properties(test_contracts PROPERTIES LABELS "unit;streaming;api" TIMEOUT 10)

  add_executable(test_token_dump tests/test_token_dump.c)
  target_include_directories(test_token_dump PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(test_token_dump PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)

  if(TARGET cmdf)
    add_executable(test_cmdf tests/test_cmdf.c)
    target_compile_features(test_cmdf PRIVATE c_std_90)
    target_compile_definitions(test_cmdf PRIVATE _POSIX_C_SOURCE=200809L)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(test_cmdf PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME test_cmdf COMMAND test_cmdf $<TARGET_FILE:cmdf>)
    set_tests_properties(test_cmdf PROPERTIES LABELS "integration;cli" TIMEOUT 10)

    add_test(NAME chart_corpus
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chart_corpus.sh
              $<TARGET_FILE:cmdf>
              ${CMAKE_CURRENT_SOURCE_DIR}/testdata/chart-corpus)
    set_tests_properties(chart_corpus PROPERTIES
      LABELS "integration;cli;charts"
      TIMEOUT 20
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  add_test(NAME header_self_sufficient
    COMMAND ${CMAKE_C_COMPILER} -std=c89 -I${LIBMDF_GENERATED_INCLUDE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/include -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/header_self_sufficient.c -o ${CMAKE_CURRENT_BINARY_DIR}/header_self_sufficient.o)
  set_tests_properties(header_self_sufficient PROPERTIES LABELS "unit;api" TIMEOUT 10)

  add_test(NAME version_source_archive COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_version_source.sh)
  set_tests_properties(version_source_archive PROPERTIES LABELS "unit;packaging" TIMEOUT 10)

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
  set_tests_properties(cmake_option_matrix PROPERTIES
    LABELS "unit;packaging"
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

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
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LIBMDF_BUILD_FUZZERS requires Clang with libFuzzer support")
  endif()
  add_executable(fuzz_smoke fuzz/fuzz_smoke.c)
  target_include_directories(fuzz_smoke PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(fuzz_smoke PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  target_compile_options(fuzz_smoke PRIVATE -fsanitize=fuzzer-no-link -fno-omit-frame-pointer)
  target_link_options(fuzz_smoke PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fuzz_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
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
    install(TARGETS cmdf RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES LICENSE README.md src/html_embedded/OFL.txt DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cmdf)
  endif()
  install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libmdf)
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
