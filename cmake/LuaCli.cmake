add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmdf.lua"
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_cmdf_lua.sh" "${CMAKE_CURRENT_BINARY_DIR}/cmdf.lua"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_cmdf_lua.sh" VERBATIM)
add_custom_target(cmdf-lua ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/cmdf.lua")
