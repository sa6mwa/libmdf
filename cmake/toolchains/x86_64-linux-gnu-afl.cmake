set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(LIBMDF_BOOTLIN_TARGET x86_64-linux-gnu)
include("${CMAKE_CURRENT_LIST_DIR}/linux-bootlin.cmake")

set(LIBMDF_AFLPP_RESOLVER "${LIBMDF_SOURCE_ROOT}/scripts/cpkt-aflpp.sh")
if(NOT EXISTS "${LIBMDF_AFLPP_RESOLVER}")
  message(FATAL_ERROR "libmdf AFL++ resolver is missing: ${LIBMDF_AFLPP_RESOLVER}")
endif()

execute_process(
  COMMAND "${LIBMDF_AFLPP_RESOLVER}" discover
  RESULT_VARIABLE LIBMDF_AFLPP_RESULT
  OUTPUT_VARIABLE LIBMDF_AFLPP_DESCRIPTION
  ERROR_VARIABLE LIBMDF_AFLPP_ERROR)
if(NOT LIBMDF_AFLPP_RESULT EQUAL 0)
  message(FATAL_ERROR "Unable to provision the pinned native AFL++ wrapper: ${LIBMDF_AFLPP_ERROR}")
endif()

function(libmdf_aflpp_value key output)
  string(REPLACE "\n" ";" lines "${LIBMDF_AFLPP_DESCRIPTION}")
  foreach(line IN LISTS lines)
    if(line MATCHES "^${key}=(.+)$")
      set(${output} "${CMAKE_MATCH_1}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "AFL++ resolver did not report ${key}")
endfunction()

foreach(key cc cxx helper)
  libmdf_aflpp_value(${key} LIBMDF_AFLPP_${key})
endforeach()

set(ENV{AFL_PATH} "${LIBMDF_AFLPP_helper}")
set(ENV{AFL_CC} "${LIBMDF_BOOTLIN_cc}")
set(ENV{AFL_CXX} "${LIBMDF_BOOTLIN_cxx}")
set(CMAKE_C_COMPILER "${LIBMDF_AFLPP_cc}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${LIBMDF_AFLPP_cxx}" CACHE FILEPATH "" FORCE)
