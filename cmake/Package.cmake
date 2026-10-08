# One archive output per component and effective Release producer.
if(LIBMDF_INSTALL AND CMAKE_BUILD_TYPE STREQUAL "Release" AND LIBMDF_TARGET_ID)
  set(package_inputs
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_install.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_component.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/archive.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/html_embedded/OFL.txt")
  foreach(component sdk cli)
    if(component STREQUAL "sdk")
      set(name "libmdf-${PROJECT_VERSION}-${LIBMDF_TARGET_ID}")
      set(dependencies ${LIBMDF_EXPORT_TARGETS}
        "${CMAKE_CURRENT_SOURCE_DIR}/include/libmdf.h"
        "${LIBMDF_GENERATED_INCLUDE_DIR}/libmdf/mdf.h"
        "${CMAKE_CURRENT_BINARY_DIR}/libmdfConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/libmdfConfigVersion.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/libmdf.pc"
        "${CMAKE_CURRENT_BINARY_DIR}/package-metadata.txt")
    elseif(TARGET cmdf AND LIBMDF_INSTALL_BINARY)
      set(name "cmdf-${PROJECT_VERSION}-${LIBMDF_TARGET_ID}")
      set(dependencies cmdf "${CMAKE_CURRENT_BINARY_DIR}/cmdf.lua")
    else()
      continue()
    endif()
    set(archive "${LIBMDF_DIST_DIR}/${name}.tar.gz")
    add_custom_command(OUTPUT "${archive}"
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_component.sh"
        "${CMAKE_CURRENT_BINARY_DIR}" "${component}" "${archive}" "${CMAKE_SYSTEM_NAME}"
      DEPENDS ${dependencies} ${package_inputs}
      VERBATIM USES_TERMINAL)
    add_custom_target(package-${component} DEPENDS "${archive}")
  endforeach()
endif()
