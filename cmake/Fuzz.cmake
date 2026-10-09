enable_testing()
include("${CMAKE_CURRENT_LIST_DIR}/StableInputs.cmake")
libmdf_stable_input(CMakeCache.txt check_configuration)
add_executable(afl_gate_fixture tests/afl_gate_fixture.c)
libmdf_configure_c89_target(afl_gate_fixture)
libmdf_configure_development_runtime(afl_gate_fixture)
add_test(NAME afl_gate COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_afl_gate.sh"
  "$<TARGET_FILE:afl_gate_fixture>" "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME fuzz_smoke COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_fuzz.sh"
  "$<TARGET_FILE:fuzz_smoke>" "${CMAKE_CURRENT_BINARY_DIR}" smoke 3)
set_tests_properties(afl_gate fuzz_smoke PROPERTIES LABELS "hardening;fuzz" TIMEOUT 60 RUN_SERIAL TRUE)
file(GLOB_RECURSE fuzz_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/*")
set(stamp "${CMAKE_CURRENT_BINARY_DIR}/checks/fuzz.pass")
add_custom_command(OUTPUT "${stamp}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/checks"
  COMMAND "${CMAKE_COMMAND}" -E rm -f "${stamp}"
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_CURRENT_BINARY_DIR}"
    --output-on-failure --stop-on-failure --no-tests=error -j1 -L fuzz
  COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
  DEPENDS fuzz_smoke afl_gate_fixture ${fuzz_inputs}
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_fuzz.sh" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_afl_gate.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cpkt-aflpp.sh" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_afl_gate.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Fuzz.cmake"
    "${check_configuration}"
  VERBATIM USES_TERMINAL)
add_custom_target(check-fuzz DEPENDS "${stamp}")
