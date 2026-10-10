find_program(LIBMDF_GO go REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/StableInputs.cmake")
libmdf_stable_input(CMakeCache.txt check_configuration)
set(LIBMDF_PARITYJUDGE "${CMAKE_CURRENT_BINARY_DIR}/parityjudge")
set(LIBMDF_PARITY_TEST "${CMAKE_CURRENT_BINARY_DIR}/parityjudge-test")
file(GLOB parity_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/parityjudge/*.go" "${CMAKE_CURRENT_SOURCE_DIR}/parityjudge/*.c")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/inputs/parity-source-members.txt" CONTENT "${parity_sources}\n")
set(parity_source_dir "${CMAKE_CURRENT_BINARY_DIR}/parity-source")
set(parity_names go.mod go.sum mdf.h)
foreach(source IN LISTS parity_sources)
  get_filename_component(name "${source}" NAME)
  list(APPEND parity_names "${name}")
endforeach()
file(GLOB staged_sources "${parity_source_dir}/*.go" "${parity_source_dir}/*.c" "${parity_source_dir}/*.h")
foreach(source IN LISTS staged_sources)
  get_filename_component(name "${source}" NAME)
  if(NOT name IN_LIST parity_names)
    file(REMOVE "${source}")
  endif()
endforeach()
foreach(source IN LISTS parity_sources)
  get_filename_component(name "${source}" NAME)
  configure_file("${source}" "${parity_source_dir}/${name}" COPYONLY)
endforeach()
foreach(name go.mod go.sum)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/parityjudge/${name}" "${parity_source_dir}/${name}" COPYONLY)
endforeach()
# cgo hashes headers in its own package directory. Keeping the generated public
# header beside the bridge makes API/version edits invalidate Go's native cache.
configure_file("${LIBMDF_GENERATED_INCLUDE_DIR}/libmdf/mdf.h" "${parity_source_dir}/mdf.h" COPYONLY)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set(parity_cgo_ldflags "\"-L${CMAKE_CURRENT_BINARY_DIR}\" -lmdf \"-Wl,-rpath,${CMAKE_CURRENT_BINARY_DIR}\"")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  string(APPEND parity_cgo_ldflags " \"-Wl,--dynamic-linker,${LIBMDF_BOOTLIN_LOADER}\" \"-Wl,--disable-new-dtags,-rpath,${LIBMDF_BOOTLIN_RUNTIME_RPATH}\"")
endif()
set(parity_env
  "CC=${CMAKE_C_COMPILER}" "CGO_ENABLED=1"
  "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/fixtures"
  "CGO_CFLAGS=\"-I${LIBMDF_GENERATED_INCLUDE_DIR}\""
  "CGO_LDFLAGS=${parity_cgo_ldflags}")
set(parity_link_flags "-linkmode=external")
foreach(kind binary test)
  if(kind STREQUAL "binary")
    set(output "${LIBMDF_PARITYJUDGE}")
    set(go_command build)
  else()
    set(output "${LIBMDF_PARITY_TEST}")
    set(go_command test -c)
  endif()
  add_custom_command(OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E env ${parity_env} "${LIBMDF_GO}" ${go_command}
      -ldflags "${parity_link_flags}" -o "${output}" .
    DEPENDS ${parity_sources} mdf_shared
      "${CMAKE_CURRENT_BINARY_DIR}/inputs/parity-source-members.txt"
      "${LIBMDF_GENERATED_INCLUDE_DIR}/libmdf/mdf.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/parityjudge/go.mod"
      "${CMAKE_CURRENT_SOURCE_DIR}/parityjudge/go.sum"
    WORKING_DIRECTORY "${parity_source_dir}"
    JOB_POOL lifecycle
    VERBATIM COMMENT "Building Go parity ${kind} against the configured shared library")
endforeach()
add_custom_target(parity-tools ALL DEPENDS "${LIBMDF_PARITYJUDGE}" "${LIBMDF_PARITY_TEST}")

file(GLOB_RECURSE parity_data CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/testdata/*")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/inputs/parity-members.txt" CONTENT "${parity_data}\n${parity_sources}\n")
list(APPEND parity_data "${CMAKE_CURRENT_BINARY_DIR}/inputs/parity-members.txt")
foreach(profile quick full)
  foreach(mode ansi html stream)
    if(profile STREQUAL "quick")
      set(label quick-parity)
      set(check check-quick-parity-${mode})
    else()
      set(label full-parity)
      set(check check-parity-${mode})
    endif()
    if(mode STREQUAL "stream")
      set(script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/stream_parity.sh")
      set(arg ansi)
    else()
      set(script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/parity.sh")
      set(arg ${mode})
    endif()
    set(test "${label}_${mode}")
    add_test(NAME ${test} COMMAND "${script}" "${arg}")
    set_tests_properties(${test} PROPERTIES LABELS "${label};parity" TIMEOUT 1800 RUN_SERIAL TRUE
      ENVIRONMENT "LIBMDF_PARITYJUDGE=${LIBMDF_PARITYJUDGE};LIBMDF_PARITY_PROFILE=${profile};LIBMDF_STREAM_PARITY_PROFILE=${profile}")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/checks/${test}.pass")
    add_custom_command(OUTPUT "${stamp}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/checks"
      COMMAND "${CMAKE_COMMAND}" -E rm -f "${stamp}"
      COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_CURRENT_BINARY_DIR}"
        --output-on-failure --stop-on-failure --no-tests=error -j1 -R "^${test}$"
      COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
      DEPENDS "${LIBMDF_PARITYJUDGE}" mdf_shared ${parity_data} "${script}"
        "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Parity.cmake"
        "${check_configuration}"
      VERBATIM USES_TERMINAL)
    add_custom_target(${check} DEPENDS "${stamp}")
  endforeach()
endforeach()
