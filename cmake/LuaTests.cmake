if(NOT TARGET mdf_shared OR NOT TARGET test_destination_policy OR
   NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")
  message(FATAL_ERROR "Lua verification requires native x86_64 Linux, shared libmdf, CLI and tests")
endif()
set(sdk "${CMAKE_CURRENT_BINARY_DIR}/lua-sdk")
add_custom_command(OUTPUT "${sdk}/installed.stamp"
  BYPRODUCTS "${sdk}/lib/libmdf.so" "${sdk}/include/libmdf/mdf.h"
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_CURRENT_BINARY_DIR}" --component sdk --prefix "${sdk}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${sdk}/installed.stamp"
  DEPENDS mdf_shared mdf_static "${CMAKE_CURRENT_BINARY_DIR}/cmake_install.cmake"
    "${LIBMDF_GENERATED_INCLUDE_DIR}/libmdf/mdf.h"
  VERBATIM)
add_custom_target(lua-sdk DEPENDS "${sdk}/installed.stamp")
set(LIBMDF_LUA_MDF_LIBDIR "${sdk}/lib")
add_subdirectory(cmake/lua-runtime lua-runtime)
add_library(libmdf_lua_core MODULE lua/libmdf_core.c)
add_dependencies(libmdf_lua_core lua-sdk)
set_target_properties(libmdf_lua_core PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_compile_options(libmdf_lua_core PRIVATE -O2 -Wall -Wextra -Werror)
target_compile_definitions(libmdf_lua_core PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(libmdf_lua_core PRIVATE "${sdk}/include" "${CMAKE_CURRENT_BINARY_DIR}/lua-runtime/include")
target_link_libraries(libmdf_lua_core PRIVATE "${sdk}/lib/libmdf.so")
set_target_properties(libmdf_lua_core PROPERTIES PREFIX "" OUTPUT_NAME core
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua-tree/lib/lua/5.5/libmdf"
  BUILD_RPATH "${sdk}/lib")
set(tree "${CMAKE_CURRENT_BINARY_DIR}/lua-tree")
configure_file(lua/libmdf.lua "${tree}/share/lua/5.5/libmdf.lua" COPYONLY)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmdf.lua" "${CMAKE_CURRENT_BINARY_DIR}/cmdf.sh"
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_cmdf_lua.sh" "${CMAKE_CURRENT_BINARY_DIR}/cmdf.lua"
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_cmdf_dev_sh.sh" "${CMAKE_CURRENT_BINARY_DIR}/cmdf.sh"
    "$<TARGET_FILE:lua>" "${tree}" "${CMAKE_CURRENT_BINARY_DIR}/cmdf.lua"
  DEPENDS scripts/generate_cmdf_lua.sh scripts/generate_cmdf_dev_sh.sh
  VERBATIM)
add_custom_target(lua-cli ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/cmdf.lua" "${CMAKE_CURRENT_BINARY_DIR}/cmdf.sh")
enable_testing()
add_test(NAME lua_smoke COMMAND lua "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_smoke.lua")
add_test(NAME lua_destination_policy COMMAND test_destination_policy "$<TARGET_FILE:lua>"
  "${CMAKE_CURRENT_BINARY_DIR}/fixtures" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_destination_policy.lua")
add_test(NAME lua_cli_destination_policy COMMAND test_destination_policy "${CMAKE_CURRENT_BINARY_DIR}/cmdf.sh"
  "${CMAKE_CURRENT_BINARY_DIR}/fixtures" --lua-cli)
add_test(NAME lua_runtime COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lua_runtime.sh"
  "$<TARGET_FILE:lua>" "${tree}" "${sdk}")
set_tests_properties(lua_smoke lua_destination_policy lua_cli_destination_policy lua_runtime PROPERTIES
  LABELS "integration;lua;e2e" TIMEOUT 60
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  ENVIRONMENT "LUA_PATH=${tree}/share/lua/5.5/?.lua\;\;;LUA_CPATH=${tree}/lib/lua/5.5/?.so\;\;;LIBMDF_CMDF=$<TARGET_FILE:cmdf>;LIBMDF_CMDF_LUA=${CMAKE_CURRENT_BINARY_DIR}/cmdf.lua")
