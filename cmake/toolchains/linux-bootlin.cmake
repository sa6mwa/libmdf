# This file is included by the Linux target toolchain files before project().
# The resolver is vendored so the repository can reproduce the complete pinned
# Bootlin collection without relying on a host compiler or local checkout path.

if(NOT DEFINED LIBMDF_BOOTLIN_TARGET)
  message(FATAL_ERROR "LIBMDF_BOOTLIN_TARGET must be set before including linux-bootlin.cmake")
endif()

get_filename_component(LIBMDF_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LIBMDF_BOOTLIN_RESOLVER "${LIBMDF_SOURCE_ROOT}/scripts/cpkt-toolchains.sh")
if(NOT EXISTS "${LIBMDF_BOOTLIN_RESOLVER}")
  message(FATAL_ERROR "libmdf Bootlin resolver is missing: ${LIBMDF_BOOTLIN_RESOLVER}")
endif()

execute_process(
  COMMAND "${LIBMDF_BOOTLIN_RESOLVER}" ensure "${LIBMDF_BOOTLIN_TARGET}"
  RESULT_VARIABLE LIBMDF_BOOTLIN_RESULT
  OUTPUT_VARIABLE LIBMDF_BOOTLIN_DESCRIPTION
  ERROR_VARIABLE LIBMDF_BOOTLIN_ERROR)
if(NOT LIBMDF_BOOTLIN_RESULT EQUAL 0)
  message(FATAL_ERROR "Unable to provision the pinned Bootlin toolchain for ${LIBMDF_BOOTLIN_TARGET}: ${LIBMDF_BOOTLIN_ERROR}")
endif()

function(libmdf_bootlin_value key output)
  string(REPLACE "\n" ";" lines "${LIBMDF_BOOTLIN_DESCRIPTION}")
  foreach(line IN LISTS lines)
    if(line MATCHES "^${key}=(.+)$")
      set(${output} "${CMAKE_MATCH_1}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "Bootlin resolver did not report ${key} for ${LIBMDF_BOOTLIN_TARGET}")
endfunction()

foreach(key cc cxx ld ar ranlib strip nm objcopy objdump addr2line readelf sysroot root)
  libmdf_bootlin_value(${key} LIBMDF_BOOTLIN_${key})
endforeach()

set(CMAKE_C_COMPILER "${LIBMDF_BOOTLIN_cc}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${LIBMDF_BOOTLIN_cxx}" CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER "${LIBMDF_BOOTLIN_ld}" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${LIBMDF_BOOTLIN_ar}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${LIBMDF_BOOTLIN_ranlib}" CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP "${LIBMDF_BOOTLIN_strip}" CACHE FILEPATH "" FORCE)
set(CMAKE_NM "${LIBMDF_BOOTLIN_nm}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${LIBMDF_BOOTLIN_objcopy}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJDUMP "${LIBMDF_BOOTLIN_objdump}" CACHE FILEPATH "" FORCE)
set(CMAKE_ADDR2LINE "${LIBMDF_BOOTLIN_addr2line}" CACHE FILEPATH "" FORCE)
set(CMAKE_READELF "${LIBMDF_BOOTLIN_readelf}" CACHE FILEPATH "" FORCE)
set(CMAKE_SYSROOT "${LIBMDF_BOOTLIN_sysroot}" CACHE PATH "" FORCE)
set(CMAKE_FIND_ROOT_PATH "${LIBMDF_BOOTLIN_sysroot}" "${LIBMDF_BOOTLIN_root}" CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY CACHE STRING "" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY CACHE STRING "" FORCE)

file(GLOB LIBMDF_BOOTLIN_LOADER_CANDIDATES
  "${LIBMDF_BOOTLIN_sysroot}/lib/ld-linux*.so*"
  "${LIBMDF_BOOTLIN_sysroot}/lib/ld-musl-*.so*")
list(SORT LIBMDF_BOOTLIN_LOADER_CANDIDATES)
list(GET LIBMDF_BOOTLIN_LOADER_CANDIDATES 0 LIBMDF_BOOTLIN_LOADER)
if(NOT EXISTS "${LIBMDF_BOOTLIN_LOADER}")
  message(FATAL_ERROR "Bootlin sysroot has no ELF interpreter: ${LIBMDF_BOOTLIN_sysroot}")
endif()

execute_process(
  COMMAND "${LIBMDF_BOOTLIN_cc}" -print-file-name=libgcc_s.so.1
  OUTPUT_VARIABLE LIBMDF_BOOTLIN_LIBGCC
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE LIBMDF_BOOTLIN_LIBGCC_RESULT)
if(NOT LIBMDF_BOOTLIN_LIBGCC_RESULT EQUAL 0 OR NOT EXISTS "${LIBMDF_BOOTLIN_LIBGCC}")
  message(FATAL_ERROR "Bootlin compiler did not locate libgcc_s.so.1")
endif()
get_filename_component(LIBMDF_BOOTLIN_LIBGCC_DIR "${LIBMDF_BOOTLIN_LIBGCC}" DIRECTORY)
set(LIBMDF_BOOTLIN_RUNTIME_DIRS
  "${LIBMDF_BOOTLIN_sysroot}/lib;${LIBMDF_BOOTLIN_sysroot}/usr/lib;${LIBMDF_BOOTLIN_LIBGCC_DIR}")
string(REPLACE ";" ":" LIBMDF_BOOTLIN_RUNTIME_RPATH "${LIBMDF_BOOTLIN_RUNTIME_DIRS}")

function(libmdf_configure_development_runtime target)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
  endif()
  target_link_options(${target} PRIVATE
    "-Wl,--dynamic-linker,${LIBMDF_BOOTLIN_LOADER}"
    "-Wl,--disable-new-dtags,-rpath,${LIBMDF_BOOTLIN_RUNTIME_RPATH}")
  set_property(TARGET ${target} PROPERTY LIBMDF_BOOTLIN_DEVELOPMENT_RUNTIME TRUE)
  set_property(GLOBAL APPEND PROPERTY LIBMDF_BOOTLIN_DEVELOPMENT_TARGETS ${target})
endfunction()
