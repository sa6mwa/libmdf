# Suite-scoped success outputs use native timestamp dependencies. The checkout,
# configured tools and execution modes remain stable while a check owns it.
if(LIBMDF_BUILD_TESTS AND LIBMDF_BUILD_HOST_TESTS)
  include("${CMAKE_CURRENT_LIST_DIR}/StableInputs.cmake")
  libmdf_stable_input(CMakeCache.txt check_configuration)
  file(GLOB_RECURSE check_inputs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.[ch]" "${CMAKE_CURRENT_SOURCE_DIR}/include/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/*" "${CMAKE_CURRENT_SOURCE_DIR}/testdata/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/lua/*" "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/libmdf-scm-1.rockspec.in")
  list(APPEND check_inputs "${CMAKE_CURRENT_SOURCE_DIR}/Makefile"
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakePresets.json" "${check_configuration}")
  get_property(build_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  set(check_binaries)
  foreach(target IN LISTS build_targets)
    get_target_property(kind ${target} TYPE)
    if(kind MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY)$")
      list(APPEND check_binaries "$<TARGET_FILE:${target}>")
    endif()
  endforeach()
  if(DEFINED LIBMDF_PARITYJUDGE)
    list(APPEND check_binaries "${LIBMDF_PARITYJUDGE}" "${LIBMDF_PARITY_TEST}")
  endif()
  if(TARGET lua)
    list(APPEND check_binaries "$<TARGET_FILE:lua>" "${CMAKE_CURRENT_BINARY_DIR}/cmdf.sh")
  endif()
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/inputs/native-members.txt"
    CONTENT "${check_inputs}\n${check_binaries}\n")
  list(APPEND check_inputs "${CMAKE_CURRENT_BINARY_DIR}/inputs/native-members.txt")
  foreach(suite native memory)
    if(suite STREQUAL "native")
      set(selection -LE "memory|hardening|full-parity|quick-parity")
    else()
      set(selection -L memory)
    endif()
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/checks/${suite}.pass")
    set(inputs ${check_inputs})
    set(binaries ${check_binaries})
    if(suite STREQUAL "memory")
      set(inputs "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Tests.cmake" "${CMAKE_CURRENT_LIST_FILE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
        "${check_configuration}")
      set(binaries "$<TARGET_FILE:test_public_api>" "$<TARGET_FILE:test_contracts>")
    endif()
    add_custom_command(OUTPUT "${stamp}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/checks"
      COMMAND "${CMAKE_COMMAND}" -E rm -f "${stamp}"
      COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_CURRENT_BINARY_DIR}"
        --output-on-failure --stop-on-failure --no-tests=error -j1 ${selection}
      COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
      DEPENDS ${inputs} ${binaries}
      VERBATIM USES_TERMINAL)
    add_custom_target(check-${suite} DEPENDS "${stamp}")
  endforeach()
endif()
