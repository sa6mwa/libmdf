set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
get_filename_component(_LIBMDF_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
execute_process(
  COMMAND "${_LIBMDF_SOURCE_ROOT}/scripts/cpkt-toolchains.sh" ensure arm64-apple-darwin
  RESULT_VARIABLE _LIBMDF_DARWIN_RESULT
  OUTPUT_VARIABLE _LIBMDF_DARWIN_DESCRIPTION
  ERROR_VARIABLE _LIBMDF_DARWIN_ERROR)
if(NOT _LIBMDF_DARWIN_RESULT EQUAL 0)
  message(FATAL_ERROR "Unable to resolve the Darwin toolchain: ${_LIBMDF_DARWIN_ERROR}")
endif()
function(libmdf_darwin_value key output)
  string(REPLACE "\n" ";" lines "${_LIBMDF_DARWIN_DESCRIPTION}")
  foreach(line IN LISTS lines)
    if(line MATCHES "^${key}=(.+)$")
      set(${output} "${CMAKE_MATCH_1}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "Darwin resolver did not report ${key}")
endfunction()
libmdf_darwin_value(root _LIBMDF_OSXCROSS_ROOT)
libmdf_darwin_value(prefix _LIBMDF_OSXCROSS_HOST)

# Ask the selected collection for its SDK instead of assuming a version.
execute_process(
  COMMAND "${_LIBMDF_OSXCROSS_ROOT}/bin/${_LIBMDF_OSXCROSS_HOST}-osxcross-conf"
  RESULT_VARIABLE _LIBMDF_SDK_RESULT
  OUTPUT_VARIABLE _LIBMDF_SDK_DESCRIPTION
  ERROR_VARIABLE _LIBMDF_SDK_ERROR)
if(NOT _LIBMDF_SDK_RESULT EQUAL 0)
  message(FATAL_ERROR "Unable to query the osxcross SDK: ${_LIBMDF_SDK_ERROR}")
endif()
string(REGEX MATCH "(^|\n)export OSXCROSS_SDK=\"([^\"]+)\"" _LIBMDF_SDK_MATCH "${_LIBMDF_SDK_DESCRIPTION}")
if(NOT _LIBMDF_SDK_MATCH OR NOT IS_DIRECTORY "${CMAKE_MATCH_2}")
  message(FATAL_ERROR "Selected osxcross collection did not report an existing SDK")
endif()
file(REAL_PATH "${CMAKE_MATCH_2}" _LIBMDF_OSXCROSS_SDK)
if(DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  set(CMAKE_OSX_DEPLOYMENT_TARGET "$ENV{MACOSX_DEPLOYMENT_TARGET}" CACHE STRING "")
endif()
set(CMAKE_C_COMPILER "${_LIBMDF_OSXCROSS_ROOT}/bin/${_LIBMDF_OSXCROSS_HOST}-clang" CACHE FILEPATH "Darwin C compiler" FORCE)
set(CMAKE_AR "${_LIBMDF_OSXCROSS_ROOT}/bin/${_LIBMDF_OSXCROSS_HOST}-ar" CACHE FILEPATH "Darwin archiver" FORCE)
set(CMAKE_LINKER "${_LIBMDF_OSXCROSS_ROOT}/bin/${_LIBMDF_OSXCROSS_HOST}-ld" CACHE FILEPATH "Darwin linker" FORCE)
set(CMAKE_RANLIB "${_LIBMDF_OSXCROSS_ROOT}/bin/${_LIBMDF_OSXCROSS_HOST}-ranlib" CACHE FILEPATH "Darwin ranlib" FORCE)
set(CMAKE_INSTALL_NAME_TOOL "${_LIBMDF_OSXCROSS_ROOT}/bin/${_LIBMDF_OSXCROSS_HOST}-install_name_tool" CACHE FILEPATH "Darwin install_name_tool" FORCE)
set(CMAKE_OTOOL "${_LIBMDF_OSXCROSS_ROOT}/bin/${_LIBMDF_OSXCROSS_HOST}-otool" CACHE FILEPATH "Darwin otool" FORCE)
set(CMAKE_STRIP "${_LIBMDF_OSXCROSS_ROOT}/bin/${_LIBMDF_OSXCROSS_HOST}-strip" CACHE FILEPATH "Darwin strip" FORCE)
set(CMAKE_OSX_SYSROOT "${_LIBMDF_OSXCROSS_SDK}" CACHE PATH "Selected osxcross SDK" FORCE)
# Clang accepts an absolute linker path through --ld-path; -fuse-ld only
# selects a linker flavor and may route Darwin links through the host linker.
set(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path=${CMAKE_LINKER}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--ld-path=${CMAKE_LINKER}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "--ld-path=${CMAKE_LINKER}")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_OSX_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
