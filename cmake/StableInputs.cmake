include_guard(GLOBAL)

# CMake rewrites some generated control files on no-op configuration. Native
# value-only snapshots/restat preserve a content-stable prerequisite for checks.
function(libmdf_stable_input name output)
  set(snapshot "${CMAKE_CURRENT_BINARY_DIR}/inputs/${name}")
  if(NOT TARGET libmdf-input-${name})
    add_custom_command(OUTPUT "${snapshot}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/inputs"
      COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/${name}" "-DOUTPUT=${snapshot}"
        -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CacheInput.cmake"
      DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${name}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CacheInput.cmake"
      VERBATIM)
    add_custom_target(libmdf-input-${name} DEPENDS "${snapshot}")
  endif()
  set(${output} "${snapshot}" PARENT_SCOPE)
endfunction()
