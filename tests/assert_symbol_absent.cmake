if(NOT DEFINED NM OR NM STREQUAL "")
  message(FATAL_ERROR "symbol check requires CMAKE_NM")
endif()
if(NOT DEFINED LIBRARY OR NOT EXISTS "${LIBRARY}")
  message(FATAL_ERROR "symbol check library does not exist: ${LIBRARY}")
endif()
if(NOT DEFINED SYMBOL OR SYMBOL STREQUAL "")
  message(FATAL_ERROR "symbol check requires SYMBOL")
endif()

execute_process(
  COMMAND "${NM}" -g "${LIBRARY}"
  RESULT_VARIABLE NM_RESULT
  OUTPUT_VARIABLE NM_OUTPUT
  ERROR_VARIABLE NM_ERROR)
if(NOT NM_RESULT EQUAL 0)
  message(FATAL_ERROR "nm failed for ${LIBRARY}: ${NM_ERROR}")
endif()

string(FIND "${NM_OUTPUT}" "${SYMBOL}" SYMBOL_OFFSET)
if(NOT SYMBOL_OFFSET EQUAL -1)
  message(FATAL_ERROR "production library exposes test-only symbol ${SYMBOL}: ${LIBRARY}")
endif()
