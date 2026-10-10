if(LIBMDF_BUILD_TESTS)
  enable_testing()
  add_test(NAME lifecycle_shell_syntax COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_shell.sh")
  set_tests_properties(lifecycle_shell_syntax PROPERTIES LABELS "host;lifecycle" TIMEOUT 10)
  add_executable(benchmark_command benchmarks/command.c)
  target_compile_definitions(benchmark_command PRIVATE _POSIX_C_SOURCE=200809L)
  libmdf_configure_c89_target(benchmark_command)
  libmdf_configure_development_runtime(benchmark_command)
  add_test(NAME benchmark_runner COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark.sh"
    "$<TARGET_FILE:benchmark_command>" "${CMAKE_CURRENT_BINARY_DIR}/benchmark-fixture")
  set_tests_properties(benchmark_runner PROPERTIES LABELS "host;lifecycle" TIMEOUT 10)
  add_executable(test_internal_api tests/test_api.c)
  target_include_directories(test_internal_api PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  if(LIBMDF_BUILD_STATIC)
    target_link_libraries(test_internal_api PRIVATE mdf_static)
  else()
    # Internal API tests intentionally exercise parser entry points that are
    # not part of the shared-library ABI. Keep their implementation private
    # rather than weakening the shared export policy for test-only linkage.
    add_library(mdf_internal_test STATIC EXCLUDE_FROM_ALL ${LIBMDF_SOURCES})
    libmdf_target_defaults(mdf_internal_test)
    target_link_libraries(test_internal_api PRIVATE mdf_internal_test)
  endif()
  libmdf_configure_c89_target(test_internal_api)
  libmdf_configure_development_runtime(test_internal_api)
  add_test(NAME test_internal_api COMMAND test_internal_api)
  set_tests_properties(test_internal_api PROPERTIES
    LABELS "unit;internal"
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_executable(test_public_api tests/test_public_api.c)
  libmdf_configure_c89_target(test_public_api)
  target_compile_definitions(test_public_api PRIVATE _POSIX_C_SOURCE=200809L)
  target_include_directories(test_public_api PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(test_public_api PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_development_runtime(test_public_api)
  add_test(NAME test_public_api COMMAND test_public_api)
  set_tests_properties(test_public_api PROPERTIES LABELS "unit;api" TIMEOUT 10)

  add_executable(test_contracts tests/test_contracts.c)
  target_include_directories(test_contracts PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(test_contracts PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_c89_target(test_contracts)
  libmdf_configure_development_runtime(test_contracts)
  add_test(NAME test_contracts COMMAND test_contracts)
  set_tests_properties(test_contracts PROPERTIES LABELS "unit;streaming;api" TIMEOUT 10)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"
     AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")
    find_program(LIBMDF_VALGRIND valgrind REQUIRED)
    foreach(check public_api contracts)
      set(memory_args)
      if(check STREQUAL "contracts")
        set(memory_args --under-valgrind)
      endif()
      add_test(NAME memory_${check} COMMAND "${LIBMDF_VALGRIND}"
        --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1
        "$<TARGET_FILE:test_${check}>" ${memory_args})
      set_tests_properties(memory_${check} PROPERTIES LABELS "memory" TIMEOUT 120)
    endforeach()
  endif()

  add_executable(test_zero_ansi tests/test_zero_ansi.c)
  target_include_directories(test_zero_ansi PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(test_zero_ansi PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  target_compile_definitions(test_zero_ansi PRIVATE _XOPEN_SOURCE=600)
  libmdf_configure_c89_target(test_zero_ansi)
  libmdf_configure_development_runtime(test_zero_ansi)
  add_test(NAME test_zero_ansi COMMAND test_zero_ansi)
  set_tests_properties(test_zero_ansi PROPERTIES LABELS "unit;streaming;api" TIMEOUT 10)

  add_executable(incremental_decision_parity tests/incremental_decision_parity.c)
  libmdf_configure_c89_target(incremental_decision_parity)
  target_include_directories(incremental_decision_parity PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(incremental_decision_parity PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_development_runtime(incremental_decision_parity)
  add_test(NAME incremental_decision_goldens
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_incremental_decision_goldens.sh
            $<TARGET_FILE:incremental_decision_parity>)
  set_tests_properties(incremental_decision_goldens PROPERTIES
    LABELS "integration;streaming;api;golden"
    TIMEOUT 120
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_executable(golden_render tests/golden_render.c)
  libmdf_configure_c89_target(golden_render)
  target_include_directories(golden_render PRIVATE "${LIBMDF_GENERATED_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(golden_render PRIVATE $<IF:$<BOOL:${LIBMDF_BUILD_STATIC}>,mdf_static,mdf_shared>)
  libmdf_configure_development_runtime(golden_render)
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

    add_executable(test_destination_policy tests/test_destination_policy.c)
    libmdf_configure_c89_target(test_destination_policy)
    target_compile_definitions(test_destination_policy PRIVATE _XOPEN_SOURCE=700)
    libmdf_configure_development_runtime(test_destination_policy)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
    add_test(NAME cmdf_zero_ansi
      COMMAND test_destination_policy $<TARGET_FILE:cmdf> "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
    set_tests_properties(cmdf_zero_ansi PROPERTIES LABELS "integration;cli;streaming" TIMEOUT 30)

    add_executable(test_cmdf tests/test_cmdf.c)
    libmdf_configure_c89_target(test_cmdf)
    target_compile_definitions(test_cmdf PRIVATE _POSIX_C_SOURCE=200809L)
    libmdf_configure_development_runtime(test_cmdf)
    add_test(NAME test_cmdf COMMAND test_cmdf $<TARGET_FILE:cmdf>)
    set_tests_properties(test_cmdf PROPERTIES LABELS "integration;cli" TIMEOUT 30)

    # Keep the pager fault injector in a private test library so default
    # test-enabled builds never add it to installable static/shared targets.
    add_library(mdf_pager_test STATIC EXCLUDE_FROM_ALL ${LIBMDF_SOURCES})
    libmdf_target_defaults(mdf_pager_test)
    target_compile_definitions(mdf_pager_test PRIVATE LIBMDF_PAGER_TESTING)

    add_executable(test_pager tests/test_pager.c)
    libmdf_configure_development_runtime(test_pager)
    libmdf_configure_c89_target(test_pager)
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
    add_test(NAME test_pager COMMAND test_pager $<TARGET_FILE:cmdf>)
    # The terminal pager runs after forkpty. Keep this integration test separate
    # from the normal unit suite so it can retain a focused timeout and label.
    set_tests_properties(test_pager PROPERTIES LABELS "integration;cli;pager;forkpty" TIMEOUT 30)

    add_executable(test_pager_ratio tests/test_pager_ratio.c)
    libmdf_configure_development_runtime(test_pager_ratio)
    libmdf_configure_c89_target(test_pager_ratio)
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

  if(TARGET mdf_shared AND UNIX AND NOT APPLE)
    add_test(NAME mdf_shared_exports
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shared_exports.sh
              ${CMAKE_NM}
              $<TARGET_FILE:mdf_shared>)
    set_tests_properties(mdf_shared_exports PROPERTIES
      LABELS "unit;api;abi"
      TIMEOUT 10)
  endif()

  add_test(NAME header_self_sufficient
    COMMAND ${CMAKE_C_COMPILER} -std=c89 -I${LIBMDF_GENERATED_INCLUDE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/include -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/header_self_sufficient.c -o ${CMAKE_CURRENT_BINARY_DIR}/header_self_sufficient.o)
  set_tests_properties(header_self_sufficient PROPERTIES LABELS "unit;api" TIMEOUT 10)

  add_test(NAME source_header_self_sufficient
    COMMAND ${CMAKE_C_COMPILER} -std=c89 -I${CMAKE_CURRENT_SOURCE_DIR}/include -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/header_self_sufficient.c -o ${CMAKE_CURRENT_BINARY_DIR}/source_header_self_sufficient.o)
  set_tests_properties(source_header_self_sufficient PROPERTIES LABELS "unit;api" TIMEOUT 10)

  add_test(NAME public_header_documentation_sync
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DGENERATED_INCLUDE_DIR=${LIBMDF_GENERATED_INCLUDE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_public_header_sync.cmake)
  set_tests_properties(public_header_documentation_sync PROPERTIES LABELS "unit;api;documentation" TIMEOUT 10)

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

  if(LIBMDF_BUILD_HOST_TESTS AND CMAKE_SYSTEM_NAME STREQUAL CMAKE_HOST_SYSTEM_NAME
     AND CMAKE_SYSTEM_PROCESSOR STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
  add_test(NAME native_check_reuse COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_check_reuse.sh")
  set_tests_properties(native_check_reuse PROPERTIES LABELS "host;lifecycle" TIMEOUT 15)
  add_test(NAME artifact_check_reuse COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_artifact_check_reuse.sh")
  set_tests_properties(artifact_check_reuse PROPERTIES LABELS "host;lifecycle;packaging" TIMEOUT 15)
  add_test(NAME hardening_definition_reuse COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_hardening_definition_reuse.sh")
  set_tests_properties(hardening_definition_reuse PROPERTIES LABELS "host;lifecycle" TIMEOUT 60)
  add_test(NAME lifecycle_dependencies COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lifecycle_dependencies.sh")
  set_tests_properties(lifecycle_dependencies PROPERTIES LABELS "host;lifecycle;packaging;lua" TIMEOUT 120)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  foreach(resolver toolchain-resolvers aflpp-resolver)
    add_test(NAME ${resolver}
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-cpkt-${resolver}.sh")
    set_tests_properties(${resolver} PROPERTIES
      LABELS "host;lifecycle;resolver" TIMEOUT 30 RUN_SERIAL TRUE
      ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/fixtures")
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
  add_test(NAME release_inventory COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_inventory.sh")
  set_tests_properties(release_inventory PROPERTIES LABELS "host;packaging;lifecycle" TIMEOUT 10)
  add_test(NAME lua_package_reuse COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lua_package_reuse.sh")
  set_tests_properties(lua_package_reuse PROPERTIES LABELS "host;packaging;lifecycle;lua" TIMEOUT 15)


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


  add_test(NAME darwin_toolchain_discovery COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_darwin_discovery.sh)
  set_tests_properties(darwin_toolchain_discovery PROPERTIES
    LABELS "unit;lifecycle;packaging"
    TIMEOUT 15
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME configure_presets COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_configure_presets.sh)
  set_tests_properties(configure_presets PROPERTIES
    LABELS "unit;packaging"
    TIMEOUT 120
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME cmake_option_matrix COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cmake_options.sh)
  add_test(NAME cmdf_install_boundary COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cmdf_install.sh)
  set_tests_properties(cmdf_install_boundary PROPERTIES LABELS "unit;packaging" TIMEOUT 90)
  set_tests_properties(cmake_option_matrix PROPERTIES
    LABELS "unit;packaging"
    TIMEOUT 60
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  if(TARGET mdf_shared)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Parity.cmake")
  add_test(NAME parityjudge_suite_stdin COMMAND "${LIBMDF_PARITY_TEST}")
  set_tests_properties(parityjudge_suite_stdin PROPERTIES
    LABELS "integration;parity"
    TIMEOUT 60
    ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/fixtures"
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
      ENVIRONMENT "LIBMDF_PARITYJUDGE=${LIBMDF_PARITYJUDGE}"
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endforeach()
  endif()
  endif()
endif()

if(LIBMDF_BUILD_TESTS)
  set(test_scratch "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  file(MAKE_DIRECTORY "${test_scratch}")
  get_property(registered_tests DIRECTORY PROPERTY TESTS)
  foreach(test IN LISTS registered_tests)
    set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "TMPDIR=${test_scratch}")
  endforeach()
  foreach(test test_public_api test_contracts test_zero_ansi memory_public_api memory_contracts test_cmdf test_pager)
    if(TEST ${test})
      set_property(TEST ${test} PROPERTY WORKING_DIRECTORY "${test_scratch}")
    endif()
  endforeach()
  foreach(target test_public_api test_cmdf test_pager)
    if(TARGET ${target})
      target_compile_definitions(${target} PRIVATE "LIBMDF_TEST_SCRATCH_DIR=\"${test_scratch}\"")
    endif()
  endforeach()
endif()
